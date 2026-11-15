/* window-fit <class>: keep the top-level window whose WM_CLASS contains
 * <class> at 0,0 with the root window's size. Nested compositors (COSMIC's
 * cosmic-comp in winit mode) open a fixed-size window and there is no X window
 * manager to maximize it; they follow the resize. Exits when the window is
 * gone. GPLv3/GPLv2. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static int ignore(Display *d, XErrorEvent *e) { (void) d; (void) e; return 0; }
static Window find(Display *d, Window root, const char *want) {
    Window r, p, *children = NULL, found = 0; unsigned count = 0;
    if (!XQueryTree(d, root, &r, &p, &children, &count)) return 0;
    for (unsigned i = 0; i < count && !found; i++) {
        XClassHint hint = {0}; XWindowAttributes a;
        if (!XGetWindowAttributes(d, children[i], &a) || a.override_redirect) continue;
        if (XGetClassHint(d, children[i], &hint)) {
            if ((hint.res_class && strstr(hint.res_class, want)) || (hint.res_name && strstr(hint.res_name, want))) found = children[i];
            if (hint.res_name) XFree(hint.res_name);
            if (hint.res_class) XFree(hint.res_class);
        }
    }
    if (children) XFree(children);
    return found;
}
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: window-fit <class>\n"); return 2; }
    Display *d = NULL;
    for (int i = 0; i < 100 && !(d = XOpenDisplay(NULL)); i++) usleep(100000);
    if (!d) return 1;
    XSetErrorHandler(ignore);
    Window root = DefaultRootWindow(d), w = 0;
    for (int i = 0; i < 1200 && !(w = find(d, root, argv[1])); i++) usleep(250000);   // up to 5 minutes
    if (!w) { fprintf(stderr, "WINDOW_FIT: no %s window\n", argv[1]); return 1; }
    XSelectInput(d, root, StructureNotifyMask);
    XSelectInput(d, w, StructureNotifyMask);
    for (int fits = 0;;) {
        XWindowAttributes r, a;
        if (!XGetWindowAttributes(d, root, &r) || !XGetWindowAttributes(d, w, &a)) return 0;
        if ((a.x != 0 || a.y != 0 || a.width != r.width || a.height != r.height) && fits++ < 50) {
            XMoveResizeWindow(d, w, 0, 0, (unsigned) r.width, (unsigned) r.height);
            XFlush(d);
            printf("WINDOW_FIT: %s %dx%d -> %dx%d\n", argv[1], a.width, a.height, r.width, r.height); fflush(stdout);
        }
        XEvent e; XNextEvent(d, &e);
        if (e.type == DestroyNotify && e.xdestroywindow.window == w) return 0;
    }
}
