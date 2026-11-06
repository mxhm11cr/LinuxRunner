/* LinuxRunner Xlib proof application. Same GPLv3/GPLv2 terms as LinuxRunner. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double trace_clock_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000.0+t.tv_nsec/1e6; }
static Display *display; static Window window; static GC gc;
static unsigned clicks, keys; static char text[65];
static void box(int x,int y,int w,int h,unsigned long color) {
    XSetForeground(display,gc,color); XFillRectangle(display,window,gc,x,y,w,h);
}
static void label(int x,int y,const char *s) {
    XSetForeground(display,gc,0xffffff); XDrawString(display,window,gc,x,y,s,(int)strlen(s));
}
static void report_geometry(void) {
    XWindowAttributes attributes; Window child; int x=0,y=0;
    if(XGetWindowAttributes(display,window,&attributes)&&XTranslateCoordinates(display,window,DefaultRootWindow(display),0,0,&x,&y,&child)){
        printf("GUI_TEST: geometry=%d,%d,%d,%d\n",x,y,attributes.width,attributes.height);fflush(stdout);
    }
}
static void paint(void) {
    box(0,0,640,480,0x182430);
    label(24,35,"REAL LINUX ARM64 X11 APPLICATION");
    label(24,58,"Xlib -> X11 display server -> LinuxRunner -> iOS");
    box(24,80,190,80,0xd84040); box(224,80,190,80,0x40b060); box(424,80,190,80,0x4080d8);
    box(24,190,380,80, clicks%2 ? 0x236c56 : 0x304858);
    char line[128]; snprintf(line,sizeof line,"Click counter: %u",clicks); label(44,235,line);
    box(424,190,190,80,0x803d40); label(475,235,"Quit");
    snprintf(line,sizeof line,"Keyboard events: %u",keys); label(24,315,line);
    label(24,342,text); label(24,400,"Tap counter, type text, or press Esc to exit.");
    label(24,425,"This window is drawn by a dynamically linked Linux ELF."); XFlush(display);
}
int main(int argc, char **argv) {
    display=XOpenDisplay(NULL); if(!display){fputs("GUI_TEST: cannot open DISPLAY\n",stderr);return 1;}
    if(argc>1 && !strcmp(argv[1],"--self-test")){
        Pixmap pixmap=XCreatePixmap(display,DefaultRootWindow(display),16,16,DefaultDepth(display,DefaultScreen(display)));
        GC testgc=XCreateGC(display,pixmap,0,NULL);XSetForeground(display,testgc,0x4080d8);XFillRectangle(display,pixmap,testgc,0,0,16,16);XSync(display,False);
        XFreeGC(display,testgc);XFreePixmap(display,pixmap);XCloseDisplay(display);puts("GUI_SUPPORT: real Linux Xlib draw/sync probe=PASS");return 0;
    }
    window=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,640,480,0,0,0x182430);
    XStoreName(display,window,"LinuxRunner real Xlib test");
    XSelectInput(display,window,ExposureMask|ButtonPressMask|KeyPressMask|KeyReleaseMask|StructureNotifyMask);
    Atom wmDelete=XInternAtom(display,"WM_DELETE_WINDOW",False); XSetWMProtocols(display,window,&wmDelete,1);
    gc=XCreateGC(display,window,0,NULL); XMapRaised(display,window); XFlush(display);

    puts("GUI_TEST: mapped real ARM64 Xlib window"); fflush(stdout);
    int trace=getenv("LR_GUI_INPUT_TRACE") && !strcmp(getenv("LR_GUI_INPUT_TRACE"),"1");
    int quit=0;
    while(!quit){
        XEvent event; XNextEvent(display,&event);
        if(event.type==MapNotify)XSetInputFocus(display,window,RevertToParent,CurrentTime);
        else if(event.type==Expose){paint();report_geometry();}
        else if(event.type==ConfigureNotify)report_geometry();
        else if(event.type==ButtonPress){
            if(event.xbutton.x>=424 && event.xbutton.y>=190 && event.xbutton.y<270)quit=1;
            else if(event.xbutton.x>=24 && event.xbutton.x<404 && event.xbutton.y>=190 && event.xbutton.y<270){double event_ms=trace ? trace_clock_ms() : 0; clicks++;printf("GUI_TEST: click=%u button=%u\n",clicks,event.xbutton.button);fflush(stdout);paint(); if(trace){printf("GUI_INPUT_TRACE: click=%u event_monotonic_ms=%.3f redraw_submit_monotonic_ms=%.3f\n",clicks,event_ms,trace_clock_ms());fflush(stdout);}}
        }else if(event.type==KeyPress || event.type==KeyRelease){
            KeySym symbol;char buffer[32];int n=XLookupString(&event.xkey,buffer,sizeof buffer,&symbol,NULL);if(event.type==KeyPress)keys++;
            printf("GUI_TEST: keysym=0x%08lx down=%d state=0x%x bytes_hex=",symbol,event.type==KeyPress,event.xkey.state);
            for(int i=0;i<n;i++){printf("%02x",(unsigned char)buffer[i]);}puts("");fflush(stdout);
            if(event.type==KeyRelease)continue;
            if(symbol==XK_Escape)quit=1;
            else if(symbol==XK_BackSpace){size_t len=strlen(text);if(len)text[len-1]=0;}
            else if(n>0){size_t len=strlen(text);if((size_t)n<sizeof(text)-len){memcpy(text+len,buffer,n);text[len+n]=0;}}
            printf("GUI_TEST: keyboard-count=%u\n",keys);fflush(stdout);paint();
        }else if(event.type==ClientMessage && (Atom)event.xclient.data.l[0]==wmDelete)quit=1;
    }
    printf("GUI_TEST: clean exit clicks=%u keys=%u\n",clicks,keys);fflush(stdout);
    XFreeGC(display,gc);XDestroyWindow(display,window);XCloseDisplay(display);return 0;
}
