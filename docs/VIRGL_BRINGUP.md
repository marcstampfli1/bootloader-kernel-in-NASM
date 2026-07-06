# virtio-gpu 3D (virgl) bring-up plan

Accelerated OpenGL for MakaOS: turn the working 2D KMS stack into a real
GL-capable stack via virtio-gpu's 3D (virgl) path, so the compositor can use a
GLES2 renderer and SDL/apps get hardware GL instead of software rasterisation.

This is the concrete engineering plan behind ROADMAP.md's one-line "defer virgl
until after Hyprland lands on the 2D path". It is a LARGE, multi-phase effort
(the Mesa port alone is substantial); it is written as dependency-ordered,
independently-verifiable steps so it can be landed incrementally without ever
regressing the working 2D desktop.

## Where we are (verified 2026-07)

2D KMS is genuinely complete and real:

- `kernel/drivers/video/virtio_gpu.c` drives virtio-gpu **2D only**.
  `DRIVER_FEATURES` (virtio_gpu.c:40) = `VIRTIO_F_VERSION_1 | VIRTIO_GPU_F_EDID`.
  `VIRTIO_GPU_F_VIRGL` is `#define`d (virtio_gpu.c:34) but NOT negotiated.
  Implemented commands: GET_DISPLAY_INFO, RESOURCE_CREATE_2D, RESOURCE_UNREF,
  RESOURCE_ATTACH_BACKING, SET_SCANOUT, TRANSFER_TO_HOST_2D, RESOURCE_FLUSH,
  UPDATE_CURSOR, MOVE_CURSOR. All commands are synchronous (ctrl_hdr fence_id
  path unused).
- `kernel/drivers/video/drm.c` exposes a real Linux DRM ioctl uAPI on
  `/dev/dri/card0` (major 226 / minor 0, name "makaos-drm"): MODE_* modeset,
  dumb buffers (CREATE_DUMB / MAP_DUMB / DESTROY_DUMB), ADDFB2, atomic,
  page-flip, cursor, and a PRIME shim over dumb-buffer handles.
- Present path: `drm_commit_apply()` (drm.c:823) does TRANSFER_TO_HOST_2D ->
  SET_SCANOUT -> RESOURCE_FLUSH per scanout.
- wlroots runs `-Dbackends=drm,libinput`, `-Drenderers=` empty (pixman software
  only), `-Dallocators=` empty (dumb buffers, no GBM). See `scripts/port-wlroots.sh`.
- There is **no Mesa / GBM / EGL / GL / virgl anywhere** in the tree. The only
  GL-ish surface is stubs: `userland/libc/wayland_egl_stub.c` (returns NULL) and
  stub headers `wayland-egl.h` / `wayland-egl-core.h`. `scripts/port-libdrm.sh`
  hard-patches `drmGetRenderDeviceNameFromFd` to NULL (marker
  `MAKAOS_NO_RENDER_NODE`).
- QEMU is launched with plain `-device virtio-gpu-pci` (no `gl=on`), see
  `build.sh` / `scripts/run-sway-gui.sh`.

## Target architecture

The virgl data flow we are building toward:

```
  app / compositor
     |  GL / GLES2 calls
     v
  Mesa gallium "virgl" driver  (userland .so)
     |  encodes GL into a virgl command stream
     v
  MakaOS DRM winsys  (libdrm_virtgpu + our render node)
     |  DRM_IOCTL_VIRTGPU_{RESOURCE_CREATE,MAP,EXECBUFFER,TRANSFER_*,WAIT,GET_CAPS}
     v
  kernel drm.c  (render-node uAPI)
     |  virtio-gpu 3D control queue
     v
  kernel virtio_gpu.c  (CTX_CREATE, RESOURCE_CREATE_3D, SUBMIT_3D, TRANSFER_*_3D, fences)
     |  virtqueue
     v
  QEMU virtio-gpu-gl + virglrenderer  (host)
     |  real OpenGL on the host GPU
     v
  host GL driver
```

Two independent, replaceable clients drive that stack: wlroots (GLES2 renderer +
GBM allocator) for the compositor, and SDL/native apps via EGL.

## The hard constraint that sets the order

None of the kernel 3D work is independently testable until a userland GL client
exists to drive it. So we do NOT land kernel 3D commands speculatively on top of
the working 2D stack (that risks the desktop for zero verifiable gain). Instead
we build a minimal END-TO-END vertical slice first (Phase 0), then widen it.

Every phase has a concrete verification GATE. Do not start phase N+1 until
phase N's gate passes.

## Phases

### Phase 0 - host + capability probe (safe, no behaviour change)

STATUS: DONE (commit 0152728).  On the gl launch the driver logs the host's
virgl capsets (id=1 VIRGL, id=2 VIRGL2 max_size 1384); on the plain 2D launch
it logs "offered: no" and the 2D path is byte-identical.  scripts/run-gl.sh is
the gl launch variant.

Goal: confirm the host offers virgl and read its capset, WITHOUT enabling
VIRTIO_GPU_F_VIRGL in the live driver (2D path stays byte-identical).

- Host: add a virgl-enabled QEMU launch variant: `-device virtio-gpu-gl-pci`
  (or `virtio-vga-gl`) + `-display ...,gl=on` (headless: egl-headless), host
  `virglrenderer` present. Keep the existing 2D launch as the default.
- Kernel: in a probe-only path, negotiate VIRGL on a scratch feature mask, send
  GET_CAPSET_INFO / GET_CAPSET (opcodes already `#define`d at virtio_gpu.c:57-58,
  never sent), log capset id/version/size. Do NOT change `DRIVER_FEATURES`.
- Gate: serial shows a virgl capset (id 1 or 2) with a sane size on the gl launch,
  and the 2D desktop is unchanged on the normal launch.
- Risk: low (adds a read-only command; gated behind the gl launch).
- Effort: small.

### Phase 1 - kernel 3D command layer + fences

STATUS: DONE (1a commit 698d83d, 1b commit bd3c28f).  VIRGL is added to
DRIVER_FEATURES but only negotiated when the host offers it (masked out on a
plain virtio-gpu-pci device), so the 2D path is untouched.  CTX_CREATE/DESTROY,
CTX_ATTACH_RESOURCE, RESOURCE_CREATE_3D, TRANSFER_TO/FROM_HOST_3D and SUBMIT_3D
are implemented; two init self-tests pass on the gl launch: (1a) a 3D-resource
upload/wipe/download round-trip, and (1b) a hand-encoded virgl CREATE_SURFACE +
SET_FRAMEBUFFER_STATE + CLEAR that virglrenderer runs on the host GPU, read back
byte-exact.

NOTE on fences: the self-tests use the existing SYNCHRONOUS control-queue path
(FIFO order => a later response proves all earlier commands retired), which is
correct and needs no fence object/list/workqueue.  The async fence path below
(VIRTIO_GPU_FLAG_FENCE + an O(1) monotonic completion counter woken from the
control-queue IRQ, no per-fence allocation -- better than Linux) is deferred to
Phase 2, where userland EXECBUFFER/WAIT is the actual consumer.

Goal: the kernel can create a 3D context, create/transfer 3D resources, submit a
virgl command buffer, and wait on a fence. Still no userland client; verified by
a tiny in-kernel selftest that submits a trivial virgl clear.

- virtio_gpu.c: add VIRTIO_GPU_F_VIRGL (+ likely CONTEXT_INIT) to
  `DRIVER_FEATURES` ONLY when the gl launch is detected/available; implement
  CTX_CREATE / CTX_DESTROY, CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE,
  RESOURCE_CREATE_3D, TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D, SUBMIT_3D.
- Fences: today every command is synchronous. SUBMIT_3D needs the async
  fence_id path: allocate a fence id, set VIRTIO_GPU_FLAG_FENCE in ctrl_hdr,
  and complete on the control-queue used-ring interrupt. Reuse the existing
  virtqueue completion path; add a fence wait-queue (the same
  irq_waitq/wait_queue_t pattern the HDA/AHCI drivers use).
- Gate: an in-kernel selftest builds a minimal virgl cmd stream (create ctx +
  3D resource, SUBMIT_3D a clear-to-colour, TRANSFER_FROM_HOST_3D, fence-wait)
  and reads back the expected pixel. Runs under the gl launch only.
- Risk: medium. Confined to virtio_gpu.c; the 2D path is untouched when VIRGL is
  not negotiated. The fence path is the subtle part (async completion ordering).
- Effort: medium-large.

### Phase 2 - DRM render node uAPI

STATUS: 2a DONE (commit 0ea7fc8) -- /dev/dri/renderD128 + the DRM_IOCTL_VIRTGPU_*
ioctls (GETPARAM, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, MAP,
EXECBUFFER, TRANSFER_TO/FROM_HOST, WAIT), byte-exact with libdrm's
drm/virtgpu_drm.h, on the synchronous command path.  O(1) handle-indexed 3D bo
table (no idr).  Verified by userland virgltest driving a GPU clear through the
ioctls + mmap and reading back byte-exact magenta (283/283), reject paths
included; 2D path byte-identical.  REMAINING: 2b the O(1) async fence
(EXECBUFFER out-fence + WAIT), and real dma-buf PRIME (today PRIME is the
dumb-handle shim).

Goal: expose the virtio-gpu 3D uAPI Mesa's virgl winsys actually calls, on a
render node.

- drm.c: implement the `DRM_IOCTL_VIRTGPU_*` set: GETPARAM, GET_CAPS,
  CONTEXT_INIT, RESOURCE_CREATE, RESOURCE_INFO, MAP, TRANSFER_TO_HOST,
  TRANSFER_FROM_HOST, EXECBUFFER, WAIT. These are thin wrappers over the Phase 1
  kernel primitives.
- Add a `renderD128` device node (currently only `dri/card0` exists,
  `kernel/fs/virtfs.c:37`) and route it to a render-node open path.
- Make PRIME real: dma-buf export/import backed by GEM objects a 3D context can
  import, not the current dumb-handle shim (drm.c:1442/1476).
- Undo the `MAKAOS_NO_RENDER_NODE` patch in `scripts/port-libdrm.sh:113-124` and
  build `libdrm_virtgpu`.
- Gate: a userland test using libdrm_virtgpu directly (VIRTGPU_GET_CAPS +
  RESOURCE_CREATE + EXECBUFFER a clear + TRANSFER_FROM_HOST) reads back the
  expected pixel. This is the first userland-driven 3D test.
- Risk: medium. New ioctls are additive; render node is a new device.
- Effort: medium-large.

### Phase 3 - Mesa port (the long pole)

Goal: build Mesa configured for the gallium `virgl` driver plus a MakaOS DRM
winsys, producing `libgbm`, `libEGL`, `libGLESv2` (and `libGL`).

- New `scripts/port-mesa.sh`: cross-build Mesa with meson for the MakaOS
  sysroot. Enable `gallium-drivers=virgl`, `platforms=wayland`, `egl`, `gbm`,
  `gles2`; disable X11, LLVM (virgl doesn't need it), Vulkan initially.
- Provide the MakaOS DRM winsys: virgl's `virgl_drm_winsys` talks to
  libdrm_virtgpu (Phase 2). Verify it binds to our render node.
- Replace the EGL stubs: delete `userland/libc/wayland_egl_stub.c` and the stub
  headers once real Mesa `wayland-egl` is installed.
- Sub-gates (Mesa is big; stage it):
  - 3a: Mesa configures + builds `libgbm` against our libdrm; a gbm smoke test
    allocates a scanout buffer via GBM.
  - 3b: `libEGL` + `libGLESv2` build; `eglinfo`-style probe reports the virgl
    renderer string from the host.
  - 3c: a minimal EGL+GLES2 test app (create context on the Wayland platform,
    clear + draw a triangle, present) renders on screen through the wl_shm or
    dmabuf path.
- Risk: HIGH (Mesa cross-build for a from-scratch OS is the biggest single
  chunk; expect toolchain/dependency friction).
- Effort: very large (own multi-session sub-project).

### Phase 4 - wire the clients

Goal: the compositor and SDL use the GL stack.

- wlroots: flip `scripts/port-wlroots.sh` to `-Drenderers=gles2`
  `-Dallocators=gbm`; wlroots now composites with GLES2 over GBM/dmabuf.
- SDL3: enable a real GL/GLES video path (the current build forces
  `-DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF`, port-sdl3.sh); SDL renderer picks the
  GL backend over the wl_shm software framebuffer.
- Gate: `sdl3_hello` renders through the GL renderer (not the software one), and
  sway composites via GLES2, both confirmed via the host virgl renderer string +
  a screendump. Software paths remain as fallback.
- Risk: medium (config flips over a now-tested stack).
- Effort: medium.

## Verification strategy

- Host: a dedicated gl launch (`-device virtio-gpu-gl-pci -display egl-headless,gl=on`)
  with host `virglrenderer`; keep the 2D launch as default and CI so 2D never
  regresses.
- Each phase reads back a known pixel or a renderer string -- proxies like "no
  error" are not acceptable (see the audio/spinlock verification in this repo:
  capture the actual output and prove the specific claim).
- The 2D KMS path MUST keep passing at every phase (VIRGL negotiation is gated,
  software renderers stay as fallback).

## Open decisions

- CONTEXT_INIT vs legacy 3D context: prefer CONTEXT_INIT + capset selection
  (matches modern Mesa virgl).
- blob resources (RESOURCE_CREATE_BLOB / SET_SCANOUT_BLOB): defer; classic 3D
  resources + TRANSFER are enough for first light.
- Vulkan (venus): out of scope; virgl GL first.

## Effort summary

Phase 0 small, Phase 1 medium-large, Phase 2 medium-large, Phase 3 very large
(Mesa), Phase 4 medium. Phase 3 dominates and is best treated as its own
sub-project. Recommendation: land Phase 0 whenever a gl-enabled host is
available (it is safe and de-risks the rest), then schedule Phase 1-2 together
(they are the kernel vertical slice and share the fence/uAPI design), then commit
to the Mesa port as a focused effort.
