#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import "app/LinuxRunner/RFBClient.h"
#import "ProbeOCRDevice.h"
static void key(LRRFBClient *c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void chord(LRRFBClient *c,uint32_t mod,uint32_t k){[c key:mod down:YES];key(c,k);[c key:mod down:NO];}
static void type(LRRFBClient *c,NSString *s){for(const char *p=s.UTF8String;*p;p++)key(c,(unsigned char)*p);}
static void LRProbeWait(double n){NSDate *end=[NSDate dateWithTimeIntervalSinceNow:n];while(end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];}
static void navigate(LRRFBClient *c,NSString *url,BOOL newTab){chord(c,0xffe3,newTab?'t':'l');LRProbeWait(.5);type(c,url);key(c,0xff0d);}
static NSString *ocr(NSData *pixels,unsigned w,unsigned h){if(pixels.length!=(NSUInteger)w*h*4)return @"";CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);CGImageRef image=CGImageCreate(w,h,8,32,w*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,false,kCGRenderingIntentDefault);VNRecognizeTextRequest *request=[VNRecognizeTextRequest new];request.recognitionLevel=VNRequestTextRecognitionLevelFast;request.usesLanguageCorrection=NO;NSMutableArray *text=[NSMutableArray new];if(LRConfigureProbeOCR(request)&&[[[VNImageRequestHandler alloc]initWithCGImage:image options:@{}]performRequests:@[request] error:nil])for(VNRecognizedTextObservation *item in request.results)[text addObject:[item topCandidates:1].firstObject.string?:@""];CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);return [text componentsJoinedByString:@" "];}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]),*statePath=@(getenv("LR_BROWSER_STATE")?:""),*base=@(getenv("LR_BROWSER_URL")?:""),*roundPath=@(getenv("LR_BROWSER_ROUNDS")?:""),*download=@(getenv("LR_BROWSER_DOWNLOAD")?:"");
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];
 NSLock *lock=[NSLock new];__block NSData *frame;__block unsigned width=0,height=0;__block uint64_t frames=0;__block BOOL displayEnded=NO;
 client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t b){(void)b;[lock lock];frame=data;width=w;height=h;frames=f;[lock unlock];};client.statusHandler=^(NSString *message){fprintf(stderr,"%s\n",message.UTF8String);if([message hasPrefix:@"Linux display ended"]||[message hasPrefix:@"Display error:"])dispatch_async(dispatch_get_main_queue(),^{displayEnded=YES;});};[client start];
 NSDate *began=NSDate.date,*step=began,*hold=nil,*confirmation=nil;unsigned round=1,tab=1,phase=0,confirmations=0;BOOL pixels=NO,https=NO,downloaded=NO,passed=NO;NSMutableArray *completed=[NSMutableArray new];double holdSeconds=getenv("LR_FIREFOX_HOLD")?atof(getenv("LR_FIREFOX_HOLD")):90;
 while(-began.timeIntervalSinceNow<holdSeconds*2+210){LRProbeWait(.2);
  NSDictionary *state=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:statePath]?:[NSData data] options:0 error:nil]?:@{};NSArray *loads=state[@"loads"]?:@[];NSString *rounds=[NSString stringWithContentsOfFile:roundPath encoding:NSUTF8StringEncoding error:nil]?:@"";
  [lock lock];NSData *last=frame;unsigned w=width,h=height;[lock unlock];
  if(phase==0&&[loads containsObject:[NSString stringWithFormat:@"%u-1",round]]){
   unsigned count=0;const uint8_t *p=last.bytes;for(NSUInteger i=0;i+3<last.length;i+=4)if(p[i]==60&&p[i+1]==40&&p[i+2]==20)count++;pixels=count>20000;
   if(!pixels)continue;type(client,@"LinuxRunnerTyped");key(client,0xff09);key(client,' ');phase=1;step=NSDate.date;
  }else if(phase==1&&[state[@"typed"] containsString:@"LinuxRunnerTyped"]&&[state[@"clicked"] boolValue]){
   tab++;navigate(client,[NSString stringWithFormat:@"%@?round=%u&tab=%u",base,round,tab],YES);phase=2;
  }else if(phase==2&&[loads containsObject:[NSString stringWithFormat:@"%u-%u",round,tab]]){
   if(tab<6){tab++;navigate(client,[NSString stringWithFormat:@"%@?round=%u&tab=%u",base,round,tab],YES);}
   else {hold=NSDate.date;step=NSDate.date;phase=3;fprintf(stderr,"FIREFOX_TABS six_tabs round=%u\n",round);}
  }else if(phase==3){
   if(-step.timeIntervalSinceNow>3){chord(client,0xffe3,0xff55);for(int i=0;i<4;i++){[client pointerX:450 y:400 buttons:16];[client pointerX:450 y:400 buttons:0];}step=NSDate.date;}
   if(-hold.timeIntervalSinceNow>=holdSeconds&&[state[@"scroll"] integerValue]>0){[last writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"tabs-%u.bgra",round]] atomically:YES];navigate(client,@"https://example.com",YES);phase=4;step=NSDate.date;}
  }else if(phase==4&&-step.timeIntervalSinceNow>3){
   NSString *text=ocr(last,w,h);if(([text containsString:@"Example Domain"]||[text containsString:@"documentation examples"])){https=YES;[last writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"https-%u.bgra",round]] atomically:YES];navigate(client,[base stringByReplacingOccurrencesOfString:@"/browser" withString:@"/download"],NO);phase=5;step=NSDate.date;}
   else if(-step.timeIntervalSinceNow>35)break;
  }else if(phase==5&&[state[@"downloads"] unsignedIntValue]>=round&&[NSFileManager.defaultManager fileExistsAtPath:download]){
   downloaded=YES;[completed addObject:@{@"round":@(round),@"six_tabs":@YES,@"hold_seconds":@(holdSeconds),@"https_pixels":@(https),@"download_present":@YES}];chord(client,0xffe3,'q');phase=6;step=NSDate.date;confirmation=nil;
  }else if(phase==6){
   // A real quit-confirmation dialog was observed in the earlier run. Wait
   // for that dialog before confirming; a fixed one-second Enter raced it.
   if(-step.timeIntervalSinceNow>3&&(!confirmation||-confirmation.timeIntervalSinceNow>5)&&confirmations<4){
    confirmation=NSDate.date;NSString *text=ocr(last,w,h);
    if([text containsString:@"If you exit now"]&&[text containsString:@"Download"]){key(client,0xff0d);confirmations++;}
   }
   if([rounds containsString:[NSString stringWithFormat:@"BROWSER_ROUND_DONE %u status=0",round]]){
    if(round==2){passed=YES;break;}round=2;tab=1;phase=0;https=NO;step=NSDate.date;
   }else if(-step.timeIntervalSinceNow>30)break;
  }
  if(displayEnded&&phase!=6)break;
 }
 [lock lock];NSData *last=frame;[lock unlock];if(last)[last writeToFile:[out stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[client stop];
 NSDictionary *report=@{@"status":passed?@"PASS":@"FAIL",@"scope":@"macOS ARM64 interpreter, standard-user Firefox; controlled six-tab workloads, real page pixels and JS/input, public HTTPS, downloads, graceful browser restart; not physical thermals or unrestricted browsing qualification",@"rounds":completed,@"quit_dialog_confirmations":@(confirmations),@"last_round":@(round),@"last_phase":@(phase),@"page_pixels":@(pixels),@"download_observed":@(downloaded),@"frames":@(frames),@"seconds":@(-began.timeIntervalSinceNow),@"physical_device":@NO};[[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"firefox-tabs.json"] atomically:YES];return passed?0:1;
}}
