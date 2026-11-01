#include <assert.h>
#include <stdio.h>
#include "app/LinuxRunner/DisplayPolicy.h"
int main(void) {
    for(unsigned v=0;v<2;v++)for(unsigned e=0;e<2;e++)for(unsigned c=0;c<2;c++) {
        assert(lr_external_output(e,c)==(e&&c));
        assert(lr_keep_desktop_transport(v,e,c)==(v||(e&&c)));
    }
    uint16_t w,h;
    lr_desktop_geometry(1920,1080,&w,&h);assert(w==1600&&h==896);
    lr_desktop_geometry(3840,2160,&w,&h);assert(w==1600&&h==896);
    lr_desktop_geometry(390,844,&w,&h);assert(w==536&&h==1168);
    lr_desktop_geometry(0,100,&w,&h);assert(w==960&&h==540);
    lr_desktop_geometry(NAN,100,&w,&h);assert(w==960&&h==540);
    lr_desktop_geometry(INFINITY,100,&w,&h);assert(w==960&&h==540);
    double x=640,y=360;
    assert(lr_touchpad_move(&x,&y,100,40,1280,720)&&x==790&&y==420);
    assert(lr_touchpad_move(&x,&y,10000,10000,1280,720)&&x==1279&&y==719);
    assert(lr_touchpad_move(&x,&y,-10000,-10000,1280,720)&&x==0&&y==0);
    assert(!lr_touchpad_move(&x,&y,1,1,0,0));
    assert(!lr_touchpad_move(&x,&y,NAN,1,1280,720));
    assert(lr_input_gain(0,1.5)==1.5&&lr_input_gain(NAN,1)==1&&lr_input_gain(4,1)==4&&lr_input_gain(.25,1)==.25);
    puts("DISPLAY_POLICY_PASS toggle, connect/disconnect, viewer detach, geometry");
}
