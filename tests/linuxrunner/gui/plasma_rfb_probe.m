#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
// Plasma-over-RFB acceptance probe (macOS host, native RFB client used by the
// app). Saves PNG snapshots of what the viewer would show and checks input by
// its visible effect: clicking the application launcher must change the
// launcher region, Escape must close it again. Pixels, not process liveness.
// Usage: plasma_rfb_probe PORT PASSWORD_FILE OUTPUT_DIR
// LR_PROBE_SECONDS bounds the whole run (default 900). LR_PROBE_WAIT_TERMINAL=1
// keeps the session (and a terminal test running in it) alive until the
// session's terminal.done appears, within the same bound.
static NSLock *lock;static NSData *pixels;static uint16_t width,height;static uint64_t frames;
static void snapshot(NSString *dir,NSString *name){
 [lock lock];NSData *d=pixels;uint16_t w=width,h=height;[lock unlock];if(!d)return;
 CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();
 CGContextRef c=CGBitmapContextCreate((void *)d.bytes,w,h,8,w*4,cs,kCGImageAlphaNoneSkipFirst|kCGBitmapByteOrder32Little);
 CGImageRef img=CGBitmapContextCreateImage(c);
 NSURL *url=[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".png"]]];
 CGImageDestinationRef dst=CGImageDestinationCreateWithURL((__bridge CFURLRef)url,CFSTR("public.png"),1,NULL);
 CGImageDestinationAddImage(dst,img,NULL);CGImageDestinationFinalize(dst);
 CFRelease(dst);CGImageRelease(img);CGContextRelease(c);CGColorSpaceRelease(cs);
}
static NSData *copyPixels(void){[lock lock];NSData *d=[pixels copy];[lock unlock];return d;}
static unsigned distinctColors(NSData *d){
 if(!d)return 0;NSMutableSet *set=[NSMutableSet new];const uint32_t *p=d.bytes;size_t n=d.length/4;
 for(size_t i=0;i<n;i+=97)[set addObject:@(p[i]&0xffffff)];return (unsigned)set.count;
}
// Fraction of sampled pixels that differ inside a rectangle.
static double changed(NSData *a,NSData *b,unsigned x0,unsigned y0,unsigned x1,unsigned y1){
 if(!a||!b||a.length!=b.length)return -1;const uint32_t *p=a.bytes,*q=b.bytes;unsigned total=0,diff=0;
 for(unsigned y=y0;y<y1&&y<height;y+=2)for(unsigned x=x0;x<x1&&x<width;x+=2){total++;if((p[y*width+x]^q[y*width+x])&0xffffff)diff++;}
 return total?(double)diff/total:-1;
}
static void pump(double seconds){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];}
int main(int argc,char **argv){@autoreleasepool{
 if(argc<4)return 2;NSString *out=@(argv[3]);lock=[NSLock new];
 double limit=getenv("LR_PROBE_SECONDS")?atof(getenv("LR_PROBE_SECONDS")):900;
 NSString *password=[[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
 NSMutableArray *timeline=[NSMutableArray new];NSDate *start=NSDate.date;LRRFBClient *client=nil;__block NSString *status=@"";
 // The server appears only after KWin, PipeWire and KRFB have started.
 while(-[start timeIntervalSinceNow]<limit&&!frames){
  if(!client||[status hasPrefix:@"Display error"]||[status hasPrefix:@"Linux display ended"]){
   [client stop];client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
   client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){(void)bytes;[lock lock];pixels=data;width=w;height=h;frames=f;[lock unlock];};
   client.statusHandler=^(NSString *s){status=s;};status=@"";[client start];}
  pump(2);}
 double firstFrame=-[start timeIntervalSinceNow];
 // Wait for the desktop to settle: enough colours and stable for 20 s.
 NSData *last=nil;double stableSince=0,settled=-1;unsigned colors=0;int shot=0;
 while(frames&&-[start timeIntervalSinceNow]<limit){
  pump(5);NSData *now=copyPixels();colors=distinctColors(now);double t=-[start timeIntervalSinceNow];
  double delta=last?changed(last,now,0,0,width,height):1;
  [timeline addObject:@{@"t":@((int)t),@"colors":@(colors),@"changed":@(delta),@"frames":@(frames)}];
  if(((int)t/60)>=shot){snapshot(out,[NSString stringWithFormat:@"t%03d",(int)t]);
   // Raw BGRX bytes as received (the fourth byte is the server framebuffer's).
   [now writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"t%03d.bgra",(int)t]] atomically:YES];
   shot=(int)t/60+1;}
  if(delta>0.002||colors<16)stableSince=t;   // a blank screen has 1-2 colours; a plain desktop with its panel ~60else if(t-stableSince>=20){settled=t;break;}
  last=now;}
 NSMutableDictionary *r=[@{@"environment":@"macOS host interpreter + native RFB probe; not physical iOS",@"first_frame_s":frames?@((int)firstFrame):@(-1),
  @"settled_s":@((int)settled),@"width":@(width),@"height":@(height),@"distinct_colors":@(colors),@"timeline":timeline} mutableCopy];
 if(settled>0){
  snapshot(out,@"desktop");NSData *before=copyPixels();
  // Kickoff sits at the left end of the default bottom panel.
  unsigned lx=22,ly=height-22;
  [client pointerX:width/2 y:height/2 buttons:0];pump(.5);
  [client pointerX:lx y:ly buttons:0];pump(.5);[client pointerX:lx y:ly buttons:1];pump(.15);[client pointerX:lx y:ly buttons:0];
  double opened=-1;for(int i=0;i<24;i++){pump(5);double c=changed(before,copyPixels(),0,height/3,width/2,height-48);if(c>0.15){opened=5*(i+1);break;}}
  snapshot(out,@"launcher-open");NSData *open=copyPixels();
  [client key:0xff1b down:YES];[client key:0xff1b down:NO];
  double closed=-1;for(int i=0;i<24;i++){pump(5);NSData *now=copyPixels();if(changed(open,now,0,height/3,width/2,height-48)>0.15&&changed(before,now,0,height/3,width/2,height-48)<0.05){closed=5*(i+1);break;}}
  snapshot(out,@"launcher-closed");
  r[@"launcher_opened_after_click_s"]=@(opened);r[@"launcher_closed_after_escape_s"]=@(closed);}
 if(getenv("LR_PROBE_WAIT_TERMINAL")&&getenv("LR_GUI_APPLICATION_LOG")){
  NSString *done=[@(getenv("LR_GUI_APPLICATION_LOG")).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"terminal.done"];
  while(-[start timeIntervalSinceNow]<limit&&![NSFileManager.defaultManager fileExistsAtPath:done])pump(5);
  snapshot(out,@"terminal-done");r[@"terminal_done"]=@([NSFileManager.defaultManager fileExistsAtPath:done]);}
 BOOL pass=settled>0&&[r[@"launcher_opened_after_click_s"] doubleValue]>0&&[r[@"launcher_closed_after_escape_s"] doubleValue]>0;
 r[@"status"]=pass?@"PASS":@"FAIL";[client stop];
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"plasma-rfb.json"] atomically:YES];
 return pass?0:1;
}}
