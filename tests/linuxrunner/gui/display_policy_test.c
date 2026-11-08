#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "app/LinuxRunner/DisplayPolicy.h"
int main(void) {
    for(unsigned v=0;v<2;v++)for(unsigned e=0;e<2;e++)for(unsigned c=0;c<2;c++) {
        assert(lr_external_output(e,c)==(e&&c));
        assert(lr_keep_desktop_transport(v,e,c)==(v||(e&&c)));
    }
    uint16_t w,h;
    lr_desktop_geometry(1920,1080,&w,&h);assert(w==1600&&h==896);
    lr_desktop_geometry(3840,2160,&w,&h);assert(w==1600&&h==896);
    lr_desktop_geometry(390,844,&w,&h);assert(w==1168&&h==536);
    const double viewports[][2]={{390,844},{844,390},{1024,1366},{1366,1024},{800,800},{320,1000},{1080,1920},{256,800},{1,1},{1e308,1}};
    for(unsigned i=0;i<sizeof(viewports)/sizeof(viewports[0]);i++){
        uint16_t rw,rh;
        lr_desktop_geometry(viewports[i][0],viewports[i][1],&w,&h);
        lr_desktop_geometry(viewports[i][1],viewports[i][0],&rw,&rh);
        assert(w>h&&w==rw&&h==rh&&w<=1600&&h<=1200&&w%8==0&&h%8==0);
    }
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
    // Portrait touchpad: iPhone 390x(844-chrome) portrait with the 960x540 default.
    lr_rect d,p;
    assert(lr_portrait_touchpad_layout(390,700,960,540,true,false,false,&d,&p));
    assert(d.x==0&&d.w==390&&fabs(d.h-219.375)<1e-9);                // same aspect-fit size as centred
    assert(fabs(d.y-(700-12-240-12-219.375)/2)<1e-9);                // centred above the pad
    assert(p.x==20&&p.w==350&&p.h==240&&p.y==700-12-240&&p.y>d.h);       // capped, at the bottom
    assert(lr_portrait_touchpad_layout(390,420,960,540,true,false,false,&d,&p)&&fabs(p.h-(420-219.375-24))<1e-9); // smaller space: fills it
    assert(!lr_portrait_touchpad_layout(390,700,960,540,false,false,false,&d,&p)); // setting off
    assert(!lr_portrait_touchpad_layout(390,700,960,540,true,true,false,&d,&p));   // keyboard up
    assert(!lr_portrait_touchpad_layout(390,700,960,540,true,false,true,&d,&p));   // external-display touchpad
    assert(!lr_portrait_touchpad_layout(844,390,960,540,true,false,false,&d,&p));  // landscape
    assert(!lr_portrait_touchpad_layout(390,380,960,540,true,false,false,&d,&p));  // too little space
    assert(!lr_portrait_touchpad_layout(390,700,0,540,true,false,false,&d,&p));
    assert(!lr_portrait_touchpad_layout(NAN,700,960,540,true,false,false,&d,&p));
    // A tall portrait desktop leaves no usable pad.
    assert(!lr_portrait_touchpad_layout(390,700,540,960,true,false,false,&d,&p));
    // Hint fading (OLED): visible for 10 s after being shown, then hidden.
    assert(lr_touchpad_hint_visible(100,100)&&lr_touchpad_hint_visible(100,109.9));
    assert(!lr_touchpad_hint_visible(100,110)&&!lr_touchpad_hint_visible(0,5)&&!lr_touchpad_hint_visible(100,99));
    double at=0,rate=0;uint64_t previous=0;
    lr_display_update_rate(100,1000,&at,&previous,&rate);assert(at==100&&previous==1000&&rate==0);
    lr_display_update_rate(101,1060,&at,&previous,&rate);assert(rate==60);
    // Exact native reconnect regression: elapsed >= .9 and counter decreases.
    uint64_t restarted=3;assert((double)(restarted-previous)>1e10);
    lr_display_update_rate(102,restarted,&at,&previous,&rate);assert(rate==0&&previous==3&&at==102);
    lr_display_update_rate(103,13,&at,&previous,&rate);assert(rate==10);
    lr_display_update_rate(103.5,17,&at,&previous,&rate);assert(rate==10&&previous==13);
    lr_display_update_rate(104,17,&at,&previous,&rate);assert(rate==4);
    lr_display_update_rate(105,17,&at,&previous,&rate);assert(rate==0);
    lr_display_update_rate(50,19,&at,&previous,&rate);assert(rate==0&&at==50);
    lr_display_update_rate(NAN,19,&at,&previous,&rate);assert(rate==0&&at==0);
    lr_display_update_rate(200,UINT64_MAX-3,&at,&previous,&rate);assert(rate==0);
    lr_display_update_rate(201,UINT64_MAX,&at,&previous,&rate);assert(rate==3);
    puts("DISPLAY_POLICY_PASS toggle, connect/disconnect, viewer detach, geometry, reconnect-rate-reset");
}
