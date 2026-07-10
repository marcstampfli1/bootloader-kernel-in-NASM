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
1. **sdl2-compat** -- **DONE, UNMODIFIED** (the static-bind-patch idea below is
   obsolete: MakaOS now HAS a real dlopen loader, see docs/DYNLINKER_PLAN.md).
   sdl2-compat's native path -- `dlopen("libSDL3.so.0")` + a dlsym'd
   `SDL3_##fn` jump table -- just works.  Built via scripts/port-sdl2-compat.sh
   as `libSDL2.a`, pinned to `release-2.30.50` (the tag targeting our SDL3 3.2.0;
   newer sdl2-compat needs 3.2.4's integer_x/y wheel API).  Proven by sdl2test
   (CR2=0x5D20001F): SDL_Init loads SDL3 and the API round-trips, headless.
   The SDL2/SDL3 `SDL_*` clash is a non-issue here: the shim dlsyms SDL3 per
   libSDL3 handle (not global), libSDL3 imports no `SDL_*` from the exe, and the
   shim's `dlopen(NULL)` quirk-probe is `#ifdef __linux__` (skipped on MakaOS).
   DarkPlaces links `-lSDL2` (this libSDL2.a) and reaches SDL3 at runtime, no hack.
   [OBSOLETE ORIGINAL NOTE kept for context: "MakaOS has no dlopen, so this needs
   a static-symbol-bind patch..." -- superseded by the loader.]
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
