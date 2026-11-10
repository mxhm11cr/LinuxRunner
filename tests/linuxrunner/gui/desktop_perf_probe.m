#import <Foundation/Foundation.h>
#include <time.h>
#import "app/LinuxRunner/RFBClient.h"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
// Desktop responsiveness probe (macOS host, the app's native RFB client).
// Measures what a user feels, from pixels: time to first frame and to a
// settled desktop, launcher open/close latency at 50 ms resolution, frame
// rate while the pointer sweeps the desktop, idle frame rate and host CPU.
// Usage: desktop_perf_probe PORT PASSWORD_FILE OUTPUT_DIR [mode]
// LR_PROBE_SECONDS bounds startup (default 600). LR_PERF_LAUNCHER=x,y sets
// the launcher point (default: bottom-left, Plasma/LXQt/Xfce panels).
static NSLock *lock;static NSData *pixels;static uint16_t width,height;static uint64_t frames;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static void pump(double s){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:s]];}
static NSData *copyPixels(void){[lock lock];NSData *d=[pixels copy];[lock unlock];return d;}
static uint64_t frameCount(void){[lock lock];uint64_t f=frames;[lock unlock];return f;}
static unsigned distinctColors(NSData *d){if(!d)return 0;NSMutableSet *s=[NSMutableSet new];const uint32_t *p=d.bytes;size_t n=d.length/4;for(size_t i=0;i<n;i+=97)[s addObject:@(p[i]&0xffffff)];return (unsigned)s.count;}
static double changed(NSData *a,NSData *b,unsigned x0,unsigned y0,unsigned x1,unsigned y1){
 if(!a||!b||a.length!=b.length)return -1;const uint32_t *p=a.bytes,*q=b.bytes;unsigned t=0,d=0;
 for(unsigned y=y0;y<y1&&y<height;y+=2)for(unsigned x=x0;x<x1&&x<width;x+=2){t++;if((p[y*width+x]^q[y*width+x])&0xffffff)d++;}
 return t?(double)d/t:-1;}
// Host CPU of the interpreter processes (ish), summed, sampled by ps.
static double hostCPU(void){
 NSTask *t=[NSTask new];t.launchPath=@"/bin/ps";t.arguments=@[@"-A",@"-o",@"%cpu=,comm="];NSPipe *p=[NSPipe pipe];t.standardOutput=p;[t launch];
 NSData *d=[p.fileHandleForReading readDataToEndOfFile];[t waitUntilExit];double sum=0;
 for(NSString *line in [[[NSString alloc]initWithData:d encoding:NSUTF8StringEncoding] componentsSeparatedByString:@"\n"]){
  NSString *s=[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];NSRange r=[s rangeOfString:@" "];
  if(r.location==NSNotFound)continue;if([[s substringFromIndex:r.location+1].lastPathComponent isEqual:@"ish"])sum+=[s substringToIndex:r.location].doubleValue;}
 return sum;}
static void savePNG(NSString *dir,NSString *name){
 NSData *d=copyPixels();if(!d)return;CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();
 CGContextRef c=CGBitmapContextCreate((void *)d.bytes,width,height,8,width*4,cs,kCGImageAlphaNoneSkipFirst|kCGBitmapByteOrder32Little);
 CGImageRef img=CGBitmapContextCreateImage(c);NSURL *url=[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".png"]]];
 CGImageDestinationRef dst=CGImageDestinationCreateWithURL((__bridge CFURLRef)url,CFSTR("public.png"),1,NULL);
 CGImageDestinationAddImage(dst,img,NULL);CGImageDestinationFinalize(dst);CFRelease(dst);CGImageRelease(img);CGContextRelease(c);CGColorSpaceRelease(cs);}
static double average(NSArray<NSNumber *> *v){double s=0;for(NSNumber *n in v)s+=n.doubleValue;return v.count?s/v.count:0;}
int main(int argc,char **argv){@autoreleasepool{
 if(argc<4)return 2;NSString *out=@(argv[3]);lock=[NSLock new];
 double limit=getenv("LR_PROBE_SECONDS")?atof(getenv("LR_PROBE_SECONDS")):600;
 NSString *password=[[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
 double start=now();LRRFBClient *client=nil;__block NSString *status=@"";
 while(now()-start<limit&&!frameCount()){
  if(!client||[status hasPrefix:@"Display error"]||[status hasPrefix:@"Linux display ended"]){
   [client stop];client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
   client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t b){(void)b;[lock lock];pixels=data;width=w;height=h;frames=f;[lock unlock];};
   client.statusHandler=^(NSString *s){status=s;};status=@"";[client start];}
  pump(.5);}
 NSMutableDictionary *r=[@{@"environment":@"macOS host ARM64 interpreter + native RFB probe; not physical iPhone"} mutableCopy];
 if(!frameCount()){r[@"status"]=@"FAIL";r[@"reason"]=@"no frame";goto done;}
 r[@"first_frame_s"]=@(round((now()-start)*10)/10);
 {
 // Settled: enough colours and no visible change for 8 s (1 s sampling).
 NSData *last=nil;double stableSince=now(),settled=-1;
 while(now()-start<limit){pump(1);NSData *n=copyPixels();double d=last?changed(last,n,0,0,width,height):1;
  if(d>0.002||distinctColors(n)<16)stableSince=now();else if(now()-stableSince>=8){settled=stableSince-start;break;}last=n;}
 r[@"settled_s"]=@(round(settled*10)/10);savePNG(out,@"settled");r[@"width"]=@(width);r[@"height"]=@(height);
 if(settled<0){r[@"status"]=@"FAIL";r[@"reason"]=@"never settled";goto done;}
 // Idle: frames and host CPU over 8 s with no input.
 NSMutableArray *cpu=[NSMutableArray new];uint64_t f0=frameCount();double t0=now();
 for(int i=0;i<4;i++){pump(2);[cpu addObject:@(hostCPU())];}
 r[@"idle_fps"]=@(round((frameCount()-f0)/(now()-t0)*10)/10);r[@"idle_host_cpu_percent"]=@(round(average(cpu)));
 // Launcher open/close latency, 50 ms polling.
 unsigned lx=22,ly=height-22;const char *lp=getenv("LR_PERF_LAUNCHER");if(lp)sscanf(lp,"%u,%u",&lx,&ly);
 NSMutableArray *opens=[NSMutableArray new],*closes=[NSMutableArray new];
 for(int round_=0;round_<3;round_++){
  [client pointerX:width/2 y:height/2 buttons:0];pump(1.5);NSData *before=copyPixels();
  double t=now();[client pointerX:lx y:ly buttons:0];[client pointerX:lx y:ly buttons:1];[client pointerX:lx y:ly buttons:0];
  double opened=-1;while(now()-t<20){pump(.05);if(changed(before,copyPixels(),0,height/4,width*2/3,height-40)>0.10){opened=now()-t;break;}}
  if(round_==0){pump(1);savePNG(out,@"launcher-open");}
  [opens addObject:@(round(opened*1000))];if(opened<0)break;
  pump(1.5);NSData *open=copyPixels();
  t=now();[client key:0xff1b down:YES];[client key:0xff1b down:NO];
  double closed=-1;while(now()-t<20){pump(.05);NSData *n=copyPixels();if(changed(open,n,0,height/4,width*2/3,height-40)>0.10&&changed(before,n,0,height/4,width*2/3,height-40)<0.05){closed=now()-t;break;}}
  [closes addObject:@(round(closed*1000))];if(closed<0)break;}
 r[@"launcher_open_ms"]=opens;r[@"launcher_close_ms"]=closes;
 // Pointer sweep for 8 s at 60 Hz across the desktop (hover highlights,
 // cursor rendering, tooltips): delivered frame rate and host CPU.
 [cpu removeAllObjects];f0=frameCount();t0=now();double nextCPU=t0+2;
 for(int i=0;now()-t0<8;i++){double x=fabs(fmod(i*8.0,2.0*(width-40))-(width-40))+20;unsigned y=height-22-(i/60)%2*((height-80)/2);
  [client pointerX:(uint16_t)x y:y buttons:0];pump(1.0/60);if(now()>nextCPU){[cpu addObject:@(hostCPU())];nextCPU+=2;}}
 r[@"sweep_fps"]=@(round((frameCount()-f0)/(now()-t0)*10)/10);r[@"sweep_host_cpu_percent"]=@(round(average(cpu)));
 // Menu hover: with the launcher open, sweep the pointer over its entries;
 // each entry highlights under the pointer. Frames per second delivered is
 // what the user sees as smooth or stuttering.
 {[client pointerX:width/2 y:height/2 buttons:0];pump(1.5);
  [client pointerX:lx y:ly buttons:0];[client pointerX:lx y:ly buttons:1];[client pointerX:lx y:ly buttons:0];pump(3);
  [cpu removeAllObjects];f0=frameCount();t0=now();nextCPU=t0+2;unsigned top=height/3,bottom=height-70,span=bottom-top;
  for(int i=0;now()-t0<8;i++){unsigned step=(unsigned)(i*4)%(2*span);unsigned y=step<span?top+step:bottom-(step-span);unsigned x=(i/(2*span/4))%2?width/4:width/8;
   [client pointerX:x y:y buttons:0];pump(1.0/60);if(now()>nextCPU){[cpu addObject:@(hostCPU())];nextCPU+=2;}}
  r[@"menu_hover_fps"]=@(round((frameCount()-f0)/(now()-t0)*10)/10);r[@"menu_hover_host_cpu_percent"]=@(round(average(cpu)));
  [client key:0xff1b down:YES];[client key:0xff1b down:NO];pump(1);}
 BOOL ok=opens.count==3&&closes.count==3;for(NSNumber *v in [opens arrayByAddingObjectsFromArray:closes])if(v.doubleValue<0)ok=NO;
 r[@"status"]=ok?@"PASS":@"FAIL";
 }
done:
 [client stop];
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted|NSJSONWritingSortedKeys error:nil] writeToFile:[out stringByAppendingPathComponent:@"perf.json"] atomically:YES];
 printf("%s\n",[[NSString alloc]initWithData:[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingSortedKeys error:nil] encoding:NSUTF8StringEncoding].UTF8String);
 return [r[@"status"] isEqual:@"PASS"]?0:1;
}}
