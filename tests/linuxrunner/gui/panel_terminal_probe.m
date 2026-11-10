#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
// Test the pinned terminal in the controlled 960x540 Wayfire defaults.
// Require a real shell command executed through RFB, not just a mapped window.
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_PANEL_TEST_USER"))return 2;
 NSString *out=@(argv[3]),*user=@(getenv("LR_PANEL_TEST_USER"));
 NSString *home=[[@(getenv("LR_GUI_ROOTFS")) stringByAppendingPathComponent:@"data/home"] stringByAppendingPathComponent:user];
 NSString *ready=[[@(getenv("LR_GUI_APPLICATION_LOG")) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"desktop-state"],*input=[home stringByAppendingPathComponent:@"lr901-panel-input"];
 if([NSFileManager.defaultManager fileExistsAtPath:input]){fprintf(stderr,"Stale panel proof files\n");return 2;}
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData *pixels;__block uint16_t w=0,h=0;__block uint64_t frames=0;
 c.frameHandler=^(NSData *p,uint16_t width,uint16_t height,uint64_t n,uint64_t bytes){(void)bytes;dispatch_async(dispatch_get_main_queue(),^{pixels=p;w=width;h=height;frames=n;});};
 [c start];NSDate *start=NSDate.date;BOOL sent=NO,received=NO;
 while(-[start timeIntervalSinceNow]<140){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  if(!sent&&frames>1&&[[NSString stringWithContentsOfFile:ready encoding:NSUTF8StringEncoding error:nil] containsString:@"ready=1"]){
   [c pointerX:66 y:h-20 buttons:1];[c pointerX:66 y:h-20 buttons:0];
   [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:3]];
   const char *command="printf PANEL_INPUT_OK > ~/lr901-panel-input\n";
   for(const char *p=command;*p;p++){uint32_t key=*p=='\n'?0xff0d:(unsigned char)*p;[c key:key down:YES];[c key:key down:NO];}
   sent=YES;
  }
  NSString *text=[NSString stringWithContentsOfFile:input encoding:NSUTF8StringEncoding error:nil];
  if([text isEqual:@"PANEL_INPUT_OK"]){received=YES;break;}
 }
 [pixels writeToFile:[out stringByAppendingPathComponent:@"panel.bgra"] atomically:YES];
 NSUInteger nonblack=0;const uint8_t *p=pixels.bytes;for(NSUInteger i=0;i+3<pixels.length;i+=4)if(p[i]||p[i+1]||p[i+2])nonblack++;
 if(received){[c key:0xffe3 down:YES];[c key:'d' down:YES];[c key:'d' down:NO];[c key:0xffe3 down:NO];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];}
 [c stop];BOOL pass=received&&sent&&frames>1&&w>h&&nonblack>100;
 NSDictionary *r=@{@"status":pass?@"PASS":@"FAIL",@"keyboard_command_executed":@(received),@"frames":@(frames),@"width":@(w),@"height":@(h),@"nonblack_pixels":@(nonblack),@"scope":@"Mac interpreter + pinned terminal launcher + real guest shell + RFB; not physical iOS",@"seconds":@(-[start timeIntervalSinceNow])};
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"panel-input.json"] atomically:YES];return pass?0:1;
}}
