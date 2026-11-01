#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static void stroke(LRRFBClient *c,uint32_t key){[c key:key down:YES];[c key:key down:NO];}
static void chord(LRRFBClient *c,uint32_t key){[c key:0xffe3 down:YES];stroke(c,key);[c key:0xffe3 down:NO];}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_GEANY_FILE"))return 2;NSString *out=@(argv[3]),*file=@(getenv("LR_GEANY_FILE"));
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];
 NSLock *lock=[NSLock new];__block NSData *pixels;__block uint16_t width=0,height=0;__block uint64_t frames=0;
 c.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){(void)bytes;[lock lock];pixels=data;width=w;height=h;frames=f;[lock unlock];};
 [c start];NSDate *started=NSDate.date;double last=0;BOOL saved=NO;unsigned attempts=0;
 NSString *expected=@"LinuxRunner editor regression 20261027\n";
 while(-[started timeIntervalSinceNow]<45){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
  double elapsed=-[started timeIntervalSinceNow];[lock lock];uint64_t f=frames;[lock unlock];
  NSString *text=[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];if([text isEqual:expected]){saved=YES;break;}
  if(f>=3&&elapsed>5&&elapsed-last>7){last=elapsed;attempts++;
   [c pointerX:400 y:220 buttons:1];[c pointerX:400 y:220 buttons:0];
   [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.3]];chord(c,'a');
   [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
   for(const char *p=expected.UTF8String;*p;p++)stroke(c,*p=='\n'?0xff0d:(unsigned char)*p);
   chord(c,'s');
  }
 }
 [lock lock];NSData *lastPixels=pixels;[lock unlock];if(lastPixels)[lastPixels writeToFile:[out stringByAppendingPathComponent:@"geany-edited.bgra"] atomically:YES];
 NSDictionary *result=@{@"status":saved?@"PASS":@"FAIL",@"scope":@"Real Geany GUI open/edit/save through RFB pointer and keyboard; host fixture, not iOS",@"saved_file_matches":@(saved),@"input_attempts":@(attempts),@"width":@(width),@"height":@(height),@"frames":@(frames)};
 [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"geany-result.json"] atomically:YES];if(saved)chord(c,'q');[c stop];return saved?0:1;
}}
