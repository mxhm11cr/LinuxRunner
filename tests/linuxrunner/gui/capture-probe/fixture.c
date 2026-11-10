/* Controlled X11 repaint fixture; same license as LinuxRunner. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

static Display *display;
static Window window;
static GC gc;
static unsigned width, height, clicks;
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void rectangle(int x, int y, unsigned w, unsigned h, unsigned long color) {
    XSetForeground(display, gc, color); XFillRectangle(display, window, gc, x, y, w, h);
}
static void marker(unsigned sequence) {
    rectangle(16, 16, 64, 32, 0x2457a9);
    rectangle(96, 16, 64, 32, 0xb86c31);
    // An invertible 24-bit code gives consecutive IDs visibly different RGB
    // values, rather than one dark-blue bit changing in a tiny tile.
    rectangle(176, 16, 64, 32, (sequence * 0x1f2537u) & 0xffffffu);
    XSetForeground(display, gc, 0xffffff);
    const char *title = "LINUXRUNNER CAPTURE PROBE";
    XDrawString(display, window, gc, 20, 64, title, 25);
}
static void response(unsigned sequence) {
    rectangle(100, 100, 256, 128, sequence % 2 ? 0x247a93 : 0x835629);
    marker(sequence); XSync(display, False);
}
static void burst(void) {
    const unsigned frames = 120;
    double begin = now();
    for (unsigned i = 1; i <= frames; i++) {
        double target = begin + (i - 1) / 60.0;
        while (now() < target) {
            double remaining = target - now();
            if (remaining <= 0) break;
            struct timespec sleep = {.tv_sec = 0, .tv_nsec = (long)(remaining * 1e9)};
            nanosleep(&sleep, NULL);
        }
        // Sixteen large changing strips: controlled capture workload, not a
        // desktop/game FPS benchmark. The guest waits for each X11 draw.
        for (unsigned j = 0; j < 16; j++) {
            unsigned color = ((i * 17 + j * 31) & 255) << 16 |
                             ((i * 29 + j * 11) & 255) << 8 |
                             ((i * 7 + j * 23) & 255);
            unsigned y = j * height / 16, next = (j + 1) * height / 16;
            rectangle(0, y, width, next - y, color);
        }
        marker(0x010000 + i); XSync(display, False);
    }
    double elapsed = now() - begin;
    // Keep the last generated image briefly before publishing the end ID.
    struct timespec settle = {.tv_sec = 0, .tv_nsec = 100000000}; nanosleep(&settle, NULL);
    marker(0xf00000 + frames);
    XSetForeground(display, gc, 0xffffff);
    XDrawString(display, window, gc, 20, 90, "CAPTURE PROBE DONE", 18);
    XSync(display, False);
    printf("CAPTURE_FIXTURE generated=%u elapsed_s=%.9f requested_hz=60\n", frames, elapsed);
    fflush(stdout);
}
int main(void) {
    alarm(180);
    display = XOpenDisplay(NULL); if (!display) return 1;
    int screen = DefaultScreen(display);
    width = DisplayWidth(display, screen); height = DisplayHeight(display, screen);
    XSetWindowAttributes attributes = {.override_redirect = True};
    window = XCreateWindow(display, RootWindow(display, screen), 0, 0, width, height,
        0, CopyFromParent, InputOutput, CopyFromParent, CWOverrideRedirect, &attributes);
    XStoreName(display, window, "LinuxRunner capture performance fixture");
    XSelectInput(display, window, ExposureMask | ButtonPressMask | KeyPressMask | StructureNotifyMask);
    gc = XCreateGC(display, window, 0, NULL);
    XMapRaised(display, window); XSync(display, False);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    rectangle(0, 0, width, height, 0x142536); marker(0); XSync(display, False);
    int finished = 0;
    for (;;) {
        XEvent event; XNextEvent(display, &event);
        if (event.type == Expose && !finished) { marker(clicks); XFlush(display); }
        if (finished) continue;
        if (event.type == ButtonPress) { response(++clicks); printf("CAPTURE_FIXTURE response=%u kind=click\n", clicks); fflush(stdout); }
        if (event.type == KeyPress) {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_q) { response(++clicks); printf("CAPTURE_FIXTURE response=%u kind=keyboard\n", clicks); fflush(stdout); }
            if (key == XK_b) { finished = 1; burst(); }
        }
    }
}
