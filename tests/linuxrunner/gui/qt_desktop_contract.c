#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static int ignored(Display*d,XErrorEvent*e){(void)d;(void)e;return 0;}
static int files(Display*d,Window w,int depth){
 if(depth>8)return 0;
 XWindowAttributes a;XClassHint c={0};int found=0;
 if(XGetWindowAttributes(d,w,&a)&&a.map_state==IsViewable&&a.width>=200&&a.height>=150&&XGetClassHint(d,w,&c)){
  const char *expected_class=getenv("QT_DESKTOP_EXPECT_CLASS");
  if(!expected_class)expected_class="pcmanfm-qt";
  if((c.res_class&&strstr(c.res_class,expected_class))||(c.res_name&&strstr(c.res_name,expected_class))){
   Atom type;int fmt;unsigned long n,left;unsigned char*p=0;
   Atom desk=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DESKTOP",False);
   int desktop=0;XGetWindowProperty(d,w,XInternAtom(d,"_NET_WM_WINDOW_TYPE",False),0,16,False,XA_ATOM,&type,&fmt,&n,&left,&p);
   if(p&&fmt==32)for(unsigned long i=0;i<n;i++)if(((unsigned long*)p)[i]==desk)desktop=1;
   if(p)XFree(p);
   char *title=0;XFetchName(d,w,&title);const char *expected=getenv("QT_DESKTOP_EXPECT_TITLE");
   int matches=!expected||(title&&strstr(title,expected));if(title)XFree(title);
   if(!desktop&&matches){printf("QT_FILE_WINDOW geometry=%dx%d class=%s\n",a.width,a.height,c.res_class);found=1;}
  }
 }
 if(c.res_class)XFree(c.res_class);if(c.res_name)XFree(c.res_name);
 Window root,parent,*kids=0;unsigned n=0;if(XQueryTree(d,w,&root,&parent,&kids,&n))for(unsigned i=0;i<n;i++)found|=files(d,kids[i],depth+1);if(kids)XFree(kids);return found;
}
int main(void){Display*d=XOpenDisplay(0);if(!d)return 2;XSetErrorHandler(ignored);struct timespec pause={0,200000000};int found=0;for(int i=0;i<100;i++){if(files(d,DefaultRootWindow(d),0)){found=1;break;}nanosleep(&pause,0);}XCloseDisplay(d);puts(found?"QT_DESKTOP_FILE_MANAGER PASS":"QT_DESKTOP_FILE_MANAGER FAIL");return !found;}
