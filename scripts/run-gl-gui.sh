#!/usr/bin/env bash
# Boot MakaOS under QEMU with the virtio-gpu 3D (virgl) device in a VISIBLE,
# GL-accelerated window -- the windowed counterpart of run-gl.sh (which is
# headless via egl-headless).  Use this to actually WATCH the GLES2/virgl
# compositor (tinywl / sway) run and to interact with it (keyboard + absolute
# mouse via virtio-tablet).
#
#   AUTOLOGIN=1 AUTOLOGIN_SPEC=root:/bin/tinywl NO_QEMU=1 bash build.sh
#   bash scripts/run-gl-gui.sh
#
# The compositor renders on the virtio-gpu head; the window opens on the plain
# VGA head first, so once MakaOS has booted press  Ctrl+Alt+2  (SDL) to switch
# to the virtio-gpu head and see the compositor.  Ctrl+Alt+G releases the mouse.
# Serial (kernel log) is tee'd to build/serial-gl.txt.  Quit: close the window
# or `pkill qemu-system-x86` from another terminal.
#
# Display backend: SDL with gl=on presents cleanly on hybrid-NVIDIA / Wayland
# hosts where GTK's gl=on fails with "eglMakeCurrent failed".  Override with
# e.g.  DISP='gtk,gl=on' bash scripts/run-gl-gui.sh  if SDL misbehaves.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

SERIAL="${SERIAL:-build/serial-gl.txt}"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="build/OVMF_VARS_gl.fd"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$OVMF_VARS"

# Prefer SDL (gl=on presents cleanly on NVIDIA/Wayland where GTK's EGL fails).
DISPLAY_OPT="${DISP:-sdl,gl=on}"
if ! qemu-system-x86_64 -display help 2>/dev/null | grep -qw sdl; then
    DISPLAY_OPT="gtk,gl=on"
fi

pkill -9 -f "qemu-system-x86_64.*disk\.img" 2>/dev/null || true

echo "[run-gl-gui] virtio-gpu-gl + $DISPLAY_OPT (visible window); serial -> $SERIAL"
exec qemu-system-x86_64 \
  -accel kvm -cpu host -smp 4 -m 1024M -nodefaults -no-user-config \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file="$OVMF_VARS" \
  -drive format=raw,file=build/disk.img,if=none,id=hd0,snapshot=on \
  -device ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -vga std -device virtio-gpu-gl-pci,id=vgpu \
  -display "$DISPLAY_OPT" \
  -device virtio-keyboard-pci -device virtio-tablet-pci \
  -audiodev pa,id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
  -serial file:"$SERIAL" \
  -no-reboot -no-shutdown \
  "$@"
