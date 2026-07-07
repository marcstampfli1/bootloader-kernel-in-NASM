#!/usr/bin/env bash
# Boot MakaOS under QEMU with the virtio-gpu 3D (virgl) device in a VISIBLE,
# GL-accelerated window -- the windowed counterpart of run-gl.sh (which is
# headless via egl-headless).  Use this to WATCH the GLES2/virgl compositor
# (tinywl / sway) run and interact with it (keyboard + absolute mouse).
#
#   AUTOLOGIN=1 AUTOLOGIN_SPEC=root:/bin/tinywl NO_QEMU=1 bash build.sh
#   bash scripts/run-gl-gui.sh
#
# HOST GL CONTEXT (the tricky bit on hybrid-NVIDIA laptops): QEMU's virtio-gpu-gl
# needs a host OpenGL >= 3.0 context.  On a hybrid laptop the default GL provider
# is the Intel iGPU / a software context, which can fail ("Unable to create
# OpenGL context >= 3.0" -> "virgl could not be initialized").  The standard fix
# is NVIDIA PRIME render offload: point the GL context at the NVIDIA GPU (GL 4.x)
# via __NV_PRIME_RENDER_OFFLOAD / __GLX_VENDOR_LIBRARY_NAME.  virtio-gpu is the
# ONLY display device (no -vga std), so the window shows the compositor head
# directly -- no Ctrl+Alt+<n> head switching.
#
# If it still fails, try (one at a time) -- and tell me which works:
#   GLPROVIDER=intel bash scripts/run-gl-gui.sh                 # use the Intel iGPU instead
#   DISP='sdl,gl=on' bash scripts/run-gl-gui.sh                 # SDL instead of GTK
#   DISP='gtk,gl=es' bash scripts/run-gl-gui.sh                 # request GLES, not desktop GL
#   GDK_BACKEND=wayland SDL_VIDEODRIVER=wayland bash scripts/run-gl-gui.sh   # native Wayland
# Serial (kernel log) -> build/serial-gl.txt.  Quit: close the window or
# `pkill qemu-system-x86` from another terminal.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Which host GPU provides QEMU's OpenGL context.  DEFAULT = intel: use the Intel
# iGPU via Mesa (mature, stable, and the exact GL path proven to work headless).
# This deliberately avoids the brand-new NVIDIA Blackwell driver, whose repeated
# GL-context creation can hang and freeze the whole machine.  GLPROVIDER=nvidia
# forces the RTX via PRIME offload if you specifically want that.
case "${GLPROVIDER:-intel}" in
  intel)
    export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-/usr/share/glvnd/egl_vendor.d/50_mesa.json}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-mesa}" ;;
  nvidia)
    export __NV_PRIME_RENDER_OFFLOAD="${__NV_PRIME_RENDER_OFFLOAD:-1}"
    export __GLX_VENDOR_LIBRARY_NAME="${__GLX_VENDOR_LIBRARY_NAME:-nvidia}"
    export __EGL_VENDOR_LIBRARY_FILENAMES="${__EGL_VENDOR_LIBRARY_FILENAMES:-/usr/share/glvnd/egl_vendor.d/10_nvidia.json}" ;;
  *) : ;;   # leave the GL vendor env untouched
esac

SERIAL="${SERIAL:-build/serial-gl.txt}"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS_gl.fd"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$OVMF_VARS"

# SDL opens a window on this host (GTK hit eglMakeCurrent errors); override with
# DISP=... (see header).
DISPLAY_OPT="${DISP:-sdl,gl=on}"

pkill -9 -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true

echo "[run-gl-gui] virtio-gpu-gl + $DISPLAY_OPT  (GLPROVIDER=${GLPROVIDER:-intel}, host GL EGL=${__EGL_VENDOR_LIBRARY_FILENAMES:-default})"
echo "[run-gl-gui] serial -> $SERIAL"
exec qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 1024M -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file=build/disk.img,if=none,id=hd0,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -device virtio-gpu-gl-pci,id=vgpu \
  -display "$DISPLAY_OPT" \
  -device virtio-keyboard-pci -device virtio-tablet-pci \
  -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -serial file:"$SERIAL" \
  -no-reboot -no-shutdown \
  "$@"
