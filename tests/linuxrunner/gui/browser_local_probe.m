#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void chord(LRRFBClient*c,uint32_t mod,uint32_t k){[c key:mod down:YES];key(c,k);[c key:mod down:NO];}
static void type(LRRFBClient*c,NSString*s){for(const char*p=s.UTF8String;*p;p++)key(c,(unsigned char)*p);}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;NSString*out=@(argv[3]),*statePath=@(getenv("LR_BROWSER_STATE")?:""),*url=@(getenv("LR_BROWSER_URL")?:"");
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];
 NSLock*lock=[NSLock new];__block NSData*frame;__block uint16_t width=0,height=0;__block uint64_t frames=0;
 c.frameHandler=^(NSData*p,uint16_t w,uint16_t h,uint64_t f,uint64_t b){(void)b;[lock lock];frame=p;width=w;height=h;frames=f;[lock unlock];};[c start];
 NSDate*started=NSDate.date,*step=started;unsigned phase=0;BOOL painted=NO,passed=NO;
 while(-[started timeIntervalSinceNow]<65){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
  NSDictionary*state=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:statePath]?:[NSData data] options:0 error:nil]?:@{};
  NSArray*loads=state[@"loads"]?:@[];NSString*typed=state[@"typed"]?:@"";BOOL clicked=[state[@"clicked"] boolValue];
  [lock lock];NSData*p=frame;[lock unlock];if(p&&!painted){const uint8_t*b=p.bytes;unsigned count=0;for(NSUInteger i=0;i+3<p.length;i+=4)if(b[i]==60&&b[i+1]==40&&b[i+2]==20)count++;painted=count>20000;}
  if(phase==0&&[loads containsObject:@1]&&painted&&-[step timeIntervalSinceNow]>2){type(c,@"LinuxRunnerTyped");key(c,0xff09);key(c,' ');phase=1;step=NSDate.date;}
  else if(phase==1&&[typed containsString:@"LinuxRunnerTyped"]&&clicked){chord(c,0xffe3,'t');[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.6]];type(c,[url stringByAppendingString:@"?tab=2"]);key(c,0xff0d);phase=2;step=NSDate.date;}
  else if(phase==2&&[loads containsObject:@2]){chord(c,0xffe3,'t');[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.6]];type(c,[url stringByAppendingString:@"?tab=3"]);key(c,0xff0d);phase=3;step=NSDate.date;}
  else if(phase==3&&[loads containsObject:@3]&&-[step timeIntervalSinceNow]>3){passed=YES;break;}
 }
 [lock lock];NSData*last=frame;[lock unlock];if(last)[last writeToFile:[out stringByAppendingPathComponent:@"browser-local.bgra"] atomically:YES];
 NSDictionary*r=@{@"status":passed?@"PASS":@"FAIL",@"scope":@"Real ARM64 browser: controlled HTTP/JavaScript, typed DOM input, button event, three tabs, actual page pixels; not HTTPS or long-duration qualification",@"phase":@(phase),@"page_pixels":@(painted),@"frames":@(frames),@"width":@(width),@"height":@(height),@"physical_device":@NO};
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"browser-local.json"] atomically:YES];[c stop];return passed?0:1;
}}
