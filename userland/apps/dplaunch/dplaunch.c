/*
 * dplaunch -- launch DarkPlaces headless.  autologin can only exec a bare path
 * (no args, no env), so this tiny wrapper forces SDL's "offscreen" video driver
 * (a surfaceless EGL context via Mesa -- no wayland compositor needed, falls
 * back to swrast without a GPU) and hands DarkPlaces its basedir, then execs it.
 * setenv writes the current environ; execv passes that environ through.
 */
extern int setenv(const char*, const char*, int);
extern int execv(const char*, char* const[]);

int main(void) {
    setenv("SDL_VIDEODRIVER", "offscreen", 1);
    char* argv[] = { "/bin/darkplaces", "-basedir", "/root", (char*)0 };
    execv("/bin/darkplaces", argv);
    return 1;   /* only reached if execv failed */
}
