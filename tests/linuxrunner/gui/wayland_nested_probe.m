#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
// Known test-client pixels, not a wallpaper heuristic or production readiness.
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]);
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];
 NSLock *lock=[NSLock new];__block NSData *pixels;__block uint16_t width=0,height=0;__block uint64_t frames=0;
 client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){(void)bytes;[lock lock];pixels=data;width=w;height=h;frames=f;[lock unlock];};
 [client start];BOOL sent=NO,found=NO;NSDate *start=NSDate.date;
 NSString *logPath=@(getenv("LR_WAYLAND_PROBE_LOG")?:"");NSString *text=@"";
 while(-[start timeIntervalSinceNow]<45){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  [lock lock];NSData *data=pixels;uint16_t w=width,h=height;[lock unlock];
  if(data&&!sent){const uint8_t *p=data.bytes;unsigned count=0,x0=0,y0=0;
   for(unsigned y=10;y+10<h;y++)for(unsigned x=10;x+10<w;x++){size_t n=(y*w+x)*4;if(p[n]==0x55&&p[n+1]>20&&p[n+2]>20){if(!count){x0=x;y0=y;}count++;}}
   if(count>10000){[data writeToFile:[out stringByAppendingPathComponent:@"wayland-client.bgra"] atomically:YES];
    [client pointerX:x0+12 y:y0+12 buttons:1];[client pointerX:x0+12 y:y0+12 buttons:0];
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.3]];
    [client key:'a' down:YES];[client key:'a' down:NO];sent=YES;}
  }
  text=[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil]?:@"";
  if([text containsString:@"WAYLAND_SHM_PASS"]){found=YES;break;}
  if([text containsString:@"WAYLAND_SHM_FAIL"]||[text containsString:@"WAYLAND_COMPOSITOR_EXITED"])break;
 }
 [client stop];BOOL keys=[text containsString:@"WAYLAND_INPUT key="],buttons=[text containsString:@"WAYLAND_INPUT button="];
 NSDictionary *r=@{@"environment":@"macOS host interpreter + native RFB probe; not physical iOS",@"status":sent&&keys&&buttons&&found?@"PASS":@"FAIL",@"test_surface_pixels":@(sent),@"wayland_client_key_event":@(keys),@"wayland_client_button_event":@(buttons),@"client_frames_and_release":@(found),@"width":@(width),@"height":@(height),@"rfb_frames":@(frames)};
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"wayland-nested.json"] atomically:YES];return sent&&keys&&buttons&&found?0:1;
}}
