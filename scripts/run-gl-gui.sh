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
# needs a host OpenGL >= 3.0 context.  If the display toolkit lands on X11 /
# XWayland, NVIDIA GLX context creation fails ("Unable to create OpenGL context
# >= 3.0" -> "virgl could not be initialized").  So we FORCE the toolkit to
# native Wayland (GDK_BACKEND=wayland / SDL_VIDEODRIVER=wayland), which uses the
# session's working Wayland EGL context.  virtio-gpu is the ONLY display device
# (no -vga std), so the window shows the compositor head directly -- no
# Ctrl+Alt+<n> head switching needed.
#
# If the window is black or QEMU prints a virgl/GL error, try another backend:
#   DISP='sdl,gl=on'  bash scripts/run-gl-gui.sh     # SDL instead of GTK
#   DISP='gtk,gl=es'  bash scripts/run-gl-gui.sh     # request GLES instead of desktop GL
#   DISP='sdl,gl=es'  bash scripts/run-gl-gui.sh
# Serial (kernel log) -> build/serial-gl.txt.  Quit: close the window or
# `pkill qemu-system-x86` from another terminal.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Force native-Wayland toolkit backends so the host GL context comes from the
# session's Wayland EGL (works on NVIDIA) rather than XWayland GLX (fails).
export GDK_BACKEND="${GDK_BACKEND:-wayland}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"

SERIAL="${SERIAL:-build/serial-gl.txt}"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS_gl.fd"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$OVMF_VARS"

# GTK's Wayland GL area is the most reliable on NVIDIA/Wayland; SDL is the
# fallback.  Override entirely with DISP=... (see header).
DISPLAY_OPT="${DISP:-gtk,gl=on}"

pkill -9 -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true

echo "[run-gl-gui] virtio-gpu-gl + $DISPLAY_OPT  (GDK_BACKEND=$GDK_BACKEND SDL_VIDEODRIVER=$SDL_VIDEODRIVER)"
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
