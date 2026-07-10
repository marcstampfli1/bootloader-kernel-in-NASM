# Xonotic (DarkPlaces engine) port -- status & plan

Goal: run Xonotic (open-source arena FPS, DarkPlaces engine) natively on MakaOS
through the SDL3 + Mesa/virgl desktop-GL stack.

## Verification honesty
This is a GRAPHICAL game. Automated verification here is HEADLESS ONLY (build
clean, launch in-guest, GL context created, assets load, frames rendered
offscreen without crashing, via `run-gl.sh` + serial). Whether it *looks* like
Xonotic is a human visual check via `run-gl-gui.sh` -- never claimed "verified
working" from a headless run alone. The windowed/NVIDIA path is never launched
from automation (it froze the host GPU before).

## Feasibility: GOOD (large but tractable)
- DarkPlaces has a **PRELOAD static build** (`LDFLAGS_UNIXSDL_PRELOAD`) that
  links codec libs directly instead of `dlopen` -- sidesteps MakaOS having no
  dynamic loader.
- DarkPlaces' portability surface is almost all standard POSIX MakaOS already
  supports (sockets, dirent, sys/mman, signal, pthreads). Only frictions:
  `dlopen` (1 file, off in PRELOAD), `backtrace`/`execinfo.h` (2 files, stub).
- SDL2->SDL3 bridged by **sdl2-compat** (2 TUs, builds over our SDL3).

## DONE (this pass)
- libogg           -> scripts/port-libogg.sh    (libogg.a)
- libvorbis(+file+enc) -> scripts/port-libvorbis.sh
- libjpeg (IJG 9e) -> scripts/port-libjpeg.sh   (libjpeg.a)
- (already had: zlib, libpng16, freetype, SDL3, GLESv2/EGL, Mesa/virgl)

## REMAINING (ordered)
1. **sdl2-compat**: build the SDL2 API shim over SDL3 (fetched at
   build/third_party/sdl2-compat). Reuses all the SDL3 static-EGL work.
2. **libGL desktop front-end** OR confirm DarkPlaces resolves GL via
   SDL_GL_GetProcAddress (it does -- no libGL link needed).
3. **Disk image bump**: EXT2_SECTORS in build.sh (currently 384 MiB) -> ~2 GiB
   for Xonotic assets (or ship a minimal map subset first).
4. **Build DarkPlaces** (126 C files) in PRELOAD static mode: patch out the
   `dlopen` path + stub backtrace; link SDL2(compat)+GL+ogg/vorbis/jpeg/png/z;
   fix MakaOS libc gaps as they surface.
5. **Assets**: fetch Xonotic data (or a minimal `data/` + one map .pk3),
   install into the image like doom1.wad.
6. **Headless launch** via run-gl.sh; iterate on serial until it inits GL,
   loads a map, and renders frames. Then hand to user for the visual check.

## Risks / unknowns
- sdl2-compat may exercise SDL3 entry points our build stubbed.
- DarkPlaces GL path assumes desktop GL 2.x+/GLSL -- we have GL 3.3 core +
  compat, so OK, but shader-path quirks possible.
- Asset size vs image size; net/getaddrinfo for the master server (menu can run
  offline).
- This is multi-session work; each engine build surfaces new libc gaps.
