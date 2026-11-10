#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#include <math.h>
#import "app/LinuxRunner/RFBClient.h"
#import "ProbeOCRDevice.h"
static void key(LRRFBClient *c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void chord(LRRFBClient *c,uint32_t mod,uint32_t k){[c key:mod down:YES];key(c,k);[c key:mod down:NO];}
static void LRProbeWait(double n);
static void type(LRRFBClient *c,NSString *s){
 NSMutableData *data=[[s dataUsingEncoding:NSUTF32LittleEndianStringEncoding]mutableCopy];uint32_t *symbols=data.mutableBytes;
 for(NSUInteger i=0;i<data.length/4;i++){uint32_t ch=symbols[i];symbols[i]=ch=='\n'?0xff0d:ch<256?ch:0x01000000|ch;}
 // Drive the application at a bounded typing rate. Wire-order correctness
 // alone does not mean Firefox's interpreted UI has processed a focus change.
 for(NSUInteger i=0;i<data.length/4;i+=8){[c typeKeysyms:symbols+i count:MIN((NSUInteger)8,data.length/4-i)];LRProbeWait(.05);}
}
static void LRProbeWait(double n){NSDate *end=[NSDate dateWithTimeIntervalSinceNow:n];while(end.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];}
static void navigate(LRRFBClient *c,NSString *url,BOOL newTab){chord(c,0xffe3,newTab?'t':'l');LRProbeWait(1);type(c,url);LRProbeWait(.2);key(c,0xff0d);}
static NSSet<NSString *> *downloadFiles(NSString *path){
 NSString *prefix=path.lastPathComponent.stringByDeletingPathExtension;NSMutableSet *files=[NSMutableSet new];
 for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:path.stringByDeletingLastPathComponent error:nil]){
  if(![name hasPrefix:prefix]||![name.pathExtension isEqual:@"bin"])continue;
  NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:[path.stringByDeletingLastPathComponent stringByAppendingPathComponent:name] error:nil];
  if([attributes[NSFileType] isEqual:NSFileTypeRegular]&&[attributes[NSFileSize] unsignedLongLongValue]==65536)[files addObject:name];
 }
 return files;
}
static NSString *ocr(NSData *pixels,unsigned w,unsigned h,CGPoint *quitButton){
 if(pixels.length!=(NSUInteger)w*h*4)return @"";
 CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
 CGImageRef image=CGImageCreate(w,h,8,32,w*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,false,kCGRenderingIntentDefault);
 VNRecognizeTextRequest *request=[VNRecognizeTextRequest new];request.recognitionLevel=VNRequestTextRecognitionLevelFast;request.usesLanguageCorrection=NO;NSMutableArray *text=[NSMutableArray new];
 if(LRConfigureProbeOCR(request)&&[[[VNImageRequestHandler alloc]initWithCGImage:image options:@{}]performRequests:@[request] error:nil])for(VNRecognizedTextObservation *item in request.results){
  VNRecognizedText *candidate=[item topCandidates:1].firstObject;NSString *label=candidate.string?:@"";[text addObject:label];
  if(quitButton){
   NSRegularExpression *pattern=[NSRegularExpression regularExpressionWithPattern:@"quit\\s*firefox" options:NSRegularExpressionCaseInsensitive error:nil];
   NSTextCheckingResult *match=[pattern firstMatchInString:label options:0 range:NSMakeRange(0,label.length)];
   VNRectangleObservation *target=match?[candidate boundingBoxForRange:match.range error:nil]:nil;
   if(target){CGRect box=target.boundingBox;*quitButton=CGPointMake(CGRectGetMidX(box)*w,(1-CGRectGetMidY(box))*h);}
  }
 }
 CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);return [text componentsJoinedByString:@" "];
}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]),*statePath=@(getenv("LR_BROWSER_STATE")?:""),*base=@(getenv("LR_BROWSER_URL")?:""),*roundPath=@(getenv("LR_BROWSER_ROUNDS")?:""),*download=@(getenv("LR_BROWSER_DOWNLOAD")?:"");
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];
 NSLock *lock=[NSLock new];__block NSData *frame;__block unsigned width=0,height=0;__block uint64_t frames=0;__block BOOL displayEnded=NO;
 client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t b){(void)b;[lock lock];frame=data;width=w;height=h;frames=f;[lock unlock];};client.statusHandler=^(NSString *message){fprintf(stderr,"%s\n",message.UTF8String);if([message hasPrefix:@"Linux display ended"]||[message hasPrefix:@"Display error:"])dispatch_async(dispatch_get_main_queue(),^{displayEnded=YES;});};[client start];
 NSDate *began=NSDate.date,*step=began,*hold=nil,*confirmation=nil;unsigned round=1,tab=1,phase=0,confirmations=0,roundConfirmations=0,quitClicks=0;BOOL pixels=NO,https=NO,downloaded=NO,passed=NO;NSMutableArray *completed=[NSMutableArray new],*quitTexts=[NSMutableArray new];NSSet *downloadsBefore=NSSet.set;double holdSeconds=getenv("LR_FIREFOX_HOLD")?atof(getenv("LR_FIREFOX_HOLD")):90;
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
   NSString *text=ocr(last,w,h,NULL);if(([text containsString:@"Example Domain"]||[text containsString:@"documentation examples"])){https=YES;[last writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"https-%u.bgra",round]] atomically:YES];downloadsBefore=downloadFiles(download);navigate(client,[base stringByReplacingOccurrencesOfString:@"/browser" withString:@"/download"],NO);phase=5;step=NSDate.date;}
   else if(-step.timeIntervalSinceNow>35)break;
  }else if(phase==5&&[state[@"downloads"] unsignedIntValue]>=round&&![downloadFiles(download) isSubsetOfSet:downloadsBefore]){
   downloaded=YES;[completed addObject:@{@"round":@(round),@"six_tabs":@YES,@"hold_seconds":@(holdSeconds),@"https_pixels":@(https),@"new_download_present":@YES}];
   // Firefox may focus its download popover. Dismiss that transient UI before
   // requesting normal quit; do not disable its download or quit warnings.
   key(client,0xff1b);LRProbeWait(.5);chord(client,0xffe3,'q');phase=6;step=NSDate.date;confirmation=nil;roundConfirmations=0;
  }else if(phase==6){
   // A real quit-confirmation dialog was observed in the earlier run. Wait
   // for that dialog before confirming; a fixed one-second Enter raced it.
   if(-step.timeIntervalSinceNow>3&&(!confirmation||-confirmation.timeIntervalSinceNow>5)&&roundConfirmations<4){
    confirmation=NSDate.date;CGPoint quitButton=CGPointMake(NAN,NAN);NSString *text=ocr(last,w,h,&quitButton);
    // OCR may merge spaces or read the capital I as l. Match the download
    // confirmation's actual wording without relying on that fragile prefix.
    NSString *words=[[[text lowercaseString] componentsSeparatedByCharactersInSet:NSCharacterSet.alphanumericCharacterSet.invertedSet] componentsJoinedByString:@""];
    if(quitTexts.count<8)[quitTexts addObject:[text substringToIndex:MIN(text.length,1000)]];
    if([words containsString:@"download"]&&([words containsString:@"exitnow"]||[words containsString:@"areyousure"]||[words containsString:@"cancel1download"])){
     key(client,0xff0d);confirmations++;roundConfirmations++;fprintf(stderr,"FIREFOX_TABS download_quit_confirmation round=%u\n",round);
    }else if([words containsString:@"closecurrenttab"]&&isfinite(quitButton.x)&&isfinite(quitButton.y)){
     // Firefox's second, separate Ctrl+Q confirmation. Click its actual OCR
     // text bounds, rather than guessing a fixed button position or disabling
     // either confirmation preference. A download popover may also be open.
     [client pointerX:quitButton.x y:quitButton.y buttons:0];
     [client pointerX:quitButton.x y:quitButton.y buttons:1];
     [client pointerX:quitButton.x y:quitButton.y buttons:0];
     confirmations++;roundConfirmations++;quitClicks++;fprintf(stderr,"FIREFOX_TABS quit_button_clicked round=%u\n",round);
    }
   }
   if([rounds containsString:[NSString stringWithFormat:@"BROWSER_ROUND_DONE %u status=0",round]]){
    if(round==2){passed=YES;break;}round=2;tab=1;phase=0;https=NO;step=NSDate.date;
   }else if(-step.timeIntervalSinceNow>30)break;
  }
  if(displayEnded&&phase!=6)break;
 }
 [lock lock];NSData *last=frame;[lock unlock];if(last)[last writeToFile:[out stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[client stop];
 NSDictionary *report=@{@"status":passed?@"PASS":@"FAIL",@"scope":@"macOS ARM64 interpreter, standard-user Firefox; controlled six-tab workloads, real page pixels and JS/input, public HTTPS, downloads, graceful browser restart; not physical thermals or unrestricted browsing qualification",@"rounds":completed,@"quit_dialog_confirmations":@(confirmations),@"quit_button_clicks":@(quitClicks),@"quit_dialog_ocr":quitTexts,@"last_round":@(round),@"last_phase":@(phase),@"page_pixels":@(pixels),@"download_observed":@(downloaded),@"frames":@(frames),@"seconds":@(-began.timeIntervalSinceNow),@"physical_device":@NO};[[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"firefox-tabs.json"] atomically:YES];return passed?0:1;
}}
