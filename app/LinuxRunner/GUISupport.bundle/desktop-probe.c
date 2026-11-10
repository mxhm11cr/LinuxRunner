#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>
static int ignored(Display*d,XErrorEvent*e){(void)d;(void)e;return 0;}
static int has(Display*d,Window w,Atom prop,Atom value){Atom type;int format;unsigned long n,left;unsigned char*p=0;int ok=0;if(XGetWindowProperty(d,w,prop,0,1024,False,AnyPropertyType,&type,&format,&n,&left,&p)==Success&&format==32){for(unsigned long i=0;i<n;i++)if(((unsigned long*)p)[i]==value)ok=1;}if(p)XFree(p);return ok;}
struct state { Window root; int width,height,panel,surface; unsigned visited; Atom wt,dock,desk; };
static void inspect(Display*d,Window w,unsigned depth,struct state*s){
 if(depth>8||s->visited++>4096)return;
 XWindowAttributes a;int x,y;Window child;
 if(XGetWindowAttributes(d,w,&a)&&a.map_state==IsViewable&&XTranslateCoordinates(d,w,s->root,0,0,&x,&y,&child)&&x<s->width&&y<s->height&&x+a.width>0&&y+a.height>0){
  // A selection owner/1x1 helper is not a visible panel or desktop.
  if(a.width>=48&&a.height>=12&&has(d,w,s->wt,s->dock)){s->panel++;printf("panel_geometry=%dx%d%+d%+d\n",a.width,a.height,x,y);}
  if(a.width>=s->width*3/4&&a.height>=s->height*3/4&&has(d,w,s->wt,s->desk)){s->surface++;printf("surface_geometry=%dx%d%+d%+d\n",a.width,a.height,x,y);}
 }
 Window r,parent,*children=0;unsigned n=0;if(XQueryTree(d,w,&r,&parent,&children,&n))for(unsigned i=0;i<n;i++)inspect(d,children[i],depth+1,s);if(children)XFree(children);
}
int main(int argc,char**argv){
 Display*d=XOpenDisplay(0);if(!d){fputs("DESKTOP_PROBE: authenticated display connection failed\n",stderr);return 2;}
 // --pointer: where the X server itself has the pointer. The app compares it
 // with the position it sent over RFB: proof that input reaches the desktop.
 if(argc>1&&!strcmp(argv[1],"--pointer")){Window r,c;int rx,ry,wx,wy;unsigned m;
  if(!XQueryPointer(d,DefaultRootWindow(d),&r,&c,&rx,&ry,&wx,&wy,&m)){XCloseDisplay(d);return 3;}
  printf("pointer=%d,%d\n",rx,ry);XCloseDisplay(d);return 0;}
 XSetErrorHandler(ignored);struct state s={.root=DefaultRootWindow(d),.width=DisplayWidth(d,DefaultScreen(d)),.height=DisplayHeight(d,DefaultScreen(d)),.wt=XInternAtom(d,"_NET_WM_WINDOW_TYPE",False),.dock=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DOCK",False),.desk=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DESKTOP",False)};
 Window owner=XGetSelectionOwner(d,XInternAtom(d,"WM_S0",False));inspect(d,s.root,0,&s);int ready=owner!=None&&s.panel>0&&s.surface>0;
 printf("root_geometry=%dx%d\nwm=%lu\npanel=%d\nsurface=%d\nready=%d\n",s.width,s.height,owner,s.panel,s.surface,ready);XCloseDisplay(d);return argc>1&&!strcmp(argv[1],"--ready")&&!ready?1:0;
}
