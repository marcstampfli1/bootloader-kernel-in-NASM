/*
 * virgltest -- exercises the virtio-gpu 3D render-node uAPI end to end from
 * USERLAND (docs/VIRGL_BRINGUP.md phase 2a).  Opens /dev/dri/renderD128 and:
 *   GETPARAM(3D_FEATURES) -> CONTEXT_INIT(capset VIRGL2) -> RESOURCE_CREATE a
 *   16x16 BGRA render target -> MAP + mmap its backing -> EXECBUFFER a virgl
 *   command stream that CLEARs it to magenta -> TRANSFER_FROM_HOST -> WAIT,
 * then reads the mmap'd pixel and checks it is exactly magenta (0xFFFF00FF).
 * That means the host GPU rendered on our behalf, driven purely through the
 * DRM ioctl uAPI Mesa's virgl driver uses.
 *
 * Also drives the reject paths (bad handle, zero-size execbuffer) to prove
 * the ioctls fail-closed, not just the happy path.
 */
#include "libc.h"   // open/close/mmap/munmap/ioctl/printf + O_RDWR/PROT_*/MAP_SHARED

/* ioctl numbers -- byte-exact with libdrm drm/virtgpu_drm.h (see drm.c). */
#define DRM_IOCTL_VIRTGPU_MAP               0xC0106441u
#define DRM_IOCTL_VIRTGPU_EXECBUFFER        0xC0406442u
#define DRM_IOCTL_VIRTGPU_GETPARAM          0xC0106443u
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE   0xC0386444u
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST 0xC02C6446u
#define DRM_IOCTL_VIRTGPU_WAIT              0xC0086448u
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT      0xC010644Bu

#define VIRTGPU_PARAM_3D_FEATURES        1
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID  0x0001
#define VIRTGPU_DRM_CAPSET_VIRGL2        2

struct dvg_getparam { unsigned long long param, value; };
struct dvg_ctx_set_param { unsigned long long param, value; };
struct dvg_context_init { unsigned int num_params, pad; unsigned long long ctx_set_params; };
struct dvg_resource_create {
    unsigned int target, format, bind, width, height, depth, array_size,
                 last_level, nr_samples, flags, bo_handle, res_handle, size, stride;
};
struct dvg_map { unsigned long long offset; unsigned int handle, pad; };
struct dvg_execbuffer {
    unsigned int flags, size; unsigned long long command, bo_handles;
    unsigned int num_bo_handles; int fence_fd; unsigned int ring_idx, syncobj_stride;
    unsigned int num_in_syncobjs, num_out_syncobjs; unsigned long long in_syncobjs, out_syncobjs;
};
struct dvg_3d_box { unsigned int x, y, z, w, h, d; };
struct dvg_transfer { unsigned int bo_handle; struct dvg_3d_box box;
                      unsigned int level, offset, stride, layer_stride; };
struct dvg_wait { unsigned int handle, flags; };

/* virgl encodings (virgl_protocol.h / virgl_hw.h). */
#define VIRGL_CMD0(cmd, obj, len)         ((unsigned int)((cmd) | ((obj) << 8) | ((len) << 16)))
#define VIRGL_CCMD_CREATE_OBJECT          1u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE  5u
#define VIRGL_CCMD_CLEAR                  7u
#define VIRGL_OBJECT_SURFACE              8u
#define VIRGL_FORMAT_B8G8R8A8_UNORM       1u
#define PIPE_TEXTURE_2D                   2u
#define VIRGL_BIND_RENDER_TARGET          (1u << 1)
#define PIPE_CLEAR_COLOR0                 (1u << 2)

#define W 16u
#define H 16u
#define SURF 1u

int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR, 0);
    if (fd < 0) { printf("virgltest: FAIL open renderD128 (%d)\n", fd); return 1; }

    /* 3D available? */
    unsigned long long feat = 0;
    struct dvg_getparam gp = { .param = VIRTGPU_PARAM_3D_FEATURES,
                               .value = (unsigned long long)(unsigned long)&feat };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) != 0 || !feat) {
        printf("virgltest: FAIL 3D not available (feat=%llu)\n", feat); return 1;
    }

    /* Context with the VIRGL2 capset. */
    struct dvg_ctx_set_param cp = { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID,
                                    .value = VIRTGPU_DRM_CAPSET_VIRGL2 };
    struct dvg_context_init ci = { .num_params = 1, .pad = 0,
                                   .ctx_set_params = (unsigned long long)(unsigned long)&cp };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci) != 0) {
        printf("virgltest: FAIL CONTEXT_INIT\n"); return 1;
    }

    /* A 16x16 BGRA render target. */
    struct dvg_resource_create rc = {0};
    rc.target = PIPE_TEXTURE_2D; rc.format = VIRGL_FORMAT_B8G8R8A8_UNORM;
    rc.bind = VIRGL_BIND_RENDER_TARGET; rc.width = W; rc.height = H;
    rc.depth = 1; rc.array_size = 1;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &rc) != 0 || rc.bo_handle == 0) {
        printf("virgltest: FAIL RESOURCE_CREATE\n"); return 1;
    }

    /* Map + mmap the backing so we can read the result. */
    struct dvg_map mp = { .handle = rc.bo_handle };
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_MAP, &mp) != 0) {
        printf("virgltest: FAIL MAP\n"); return 1;
    }
    unsigned int* px = (unsigned int*)mmap(0, W * H * 4u, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, fd, (long)mp.offset);
    if (px == (void*)-1) { printf("virgltest: FAIL mmap\n"); return 1; }
    for (unsigned i = 0; i < W * H; i++) px[i] = 0xDEADBEEFu;   /* pre-poison */

    /* virgl command stream: surface view -> bind as cbuf0 -> clear magenta. */
    unsigned int cmd[19], n = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    cmd[n++] = SURF; cmd[n++] = rc.res_handle; cmd[n++] = VIRGL_FORMAT_B8G8R8A8_UNORM;
    cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    cmd[n++] = 1; cmd[n++] = 0; cmd[n++] = SURF;
    cmd[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    cmd[n++] = PIPE_CLEAR_COLOR0;
    cmd[n++] = 0x3F800000u; cmd[n++] = 0x00000000u; cmd[n++] = 0x3F800000u; cmd[n++] = 0x3F800000u;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;

    struct dvg_execbuffer eb = {0};
    eb.size = n * 4u; eb.command = (unsigned long long)(unsigned long)cmd;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb) != 0) {
        printf("virgltest: FAIL EXECBUFFER\n"); return 1;
    }

    struct dvg_transfer tr = {0};
    tr.bo_handle = rc.bo_handle; tr.box.w = W; tr.box.h = H; tr.box.d = 1;
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &tr) != 0) {
        printf("virgltest: FAIL TRANSFER_FROM_HOST\n"); return 1;
    }
    struct dvg_wait wt = { .handle = rc.bo_handle };
    ioctl(fd, DRM_IOCTL_VIRTGPU_WAIT, &wt);

    /* Verify the GPU cleared it to magenta. */
    unsigned int bad = 0, first = px[0];
    for (unsigned i = 0; i < W * H; i++) if (px[i] != 0xFFFF00FFu) bad++;

    /* ── Non-happy paths: the ioctls must reject bad input, not succeed ── */
    struct dvg_transfer bt = {0}; bt.bo_handle = 999u; bt.box.w = W; bt.box.h = H;
    int rej_badhandle = ioctl(fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &bt);
    struct dvg_execbuffer zb = {0}; zb.size = 0; zb.command = (unsigned long long)(unsigned long)cmd;
    int rej_zerocmd = ioctl(fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &zb);

    int pass = (bad == 0 && rej_badhandle != 0 && rej_zerocmd != 0);
    printf("virgltest: pixel first=%x bad=%u/%u  reject bh=%d zc=%d -> %s\n",
           first, bad, W * H, rej_badhandle, rej_zerocmd, pass ? "PASS" : "FAIL");

    // App stdout goes to the VGA console, not the serial log, and the gl
    // launch's console surface isn't screendumpable -- so also signal the
    // verdict to the kernel serial log via an unknown-ioctl request number the
    // drm layer pr_warns verbatim: 0x600D600D = PASS, 0x0BADBAD0 = FAIL.
    ioctl(fd, pass ? 0x600D600DUL : 0x0BADBAD0UL, (void*)0);

    munmap(px, W * H * 4u);
    close(fd);
    return 0;
}
