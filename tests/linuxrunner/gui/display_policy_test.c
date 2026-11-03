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
    // iPad viewport points, not Retina pixels or a 16:9 preset.
    lr_desktop_geometry(1566,975,&w,&h);assert(w==1560&&h==968);
    lr_desktop_geometry(900,1150,&w,&h);assert(w==896&&h==1144);
    assert(lr_desktop_resolution(-1,false,false)==3);
    assert(lr_desktop_resolution(-1,true,false)==4);
    assert(lr_desktop_resolution(3,true,false)==4);
    assert(lr_desktop_resolution(3,true,true)==3);
    for(int i=0;i<=4;i++) {
        assert(lr_desktop_resolution(i,false,false)==(unsigned)i);
        assert(lr_desktop_resolution(i,true,true)==(unsigned)i);
        if(i!=3)assert(lr_desktop_resolution(i,true,false)==(unsigned)i);
    }
    double x=640,y=360;
    assert(lr_touchpad_move(&x,&y,100,40,1280,720)&&x==790&&y==420);
    assert(lr_touchpad_move(&x,&y,10000,10000,1280,720)&&x==1279&&y==719);
    assert(lr_touchpad_move(&x,&y,-10000,-10000,1280,720)&&x==0&&y==0);
    assert(!lr_touchpad_move(&x,&y,1,1,0,0));
    assert(!lr_touchpad_move(&x,&y,NAN,1,1280,720));
    assert(lr_input_gain(0,1.5)==1.5&&lr_input_gain(NAN,1)==1&&lr_input_gain(4,1)==4&&lr_input_gain(.25,1)==.25);
    puts("DISPLAY_POLICY_PASS toggle, connect/disconnect, viewer detach, geometry");
}
