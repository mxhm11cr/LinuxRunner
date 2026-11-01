#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <ImageIO/ImageIO.h>
#import "ProbeOCRDevice.h"
#import "app/LinuxRunner/RFBClient.h"
// Test-only: operate the original application's visible document, then inspect
// its saved DOCX independently. Never write the expected result into the file.
static void key(LRRFBClient *c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void control(LRRFBClient *c,uint32_t k){[c key:0xffe3 down:YES];key(c,k);[c key:0xffe3 down:NO];}
static NSString *text(NSData *pixels,uint16_t w,uint16_t h,NSString *png){
 if(!pixels||!w||!h)return @"";CGColorSpaceRef s=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);CGImageRef im=CGImageCreate(w,h,8,32,w*4,s,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);
 if(png&&im){CGImageDestinationRef d=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:png],CFSTR("public.png"),1,NULL);if(d){CGImageDestinationAddImage(d,im,NULL);CGImageDestinationFinalize(d);CFRelease(d);}}
 VNRecognizeTextRequest *r=[VNRecognizeTextRequest new];r.recognitionLevel=VNRequestTextRecognitionLevelAccurate;r.usesLanguageCorrection=NO;NSMutableArray *lines=[NSMutableArray new];NSError *error=nil;
 if(im&&LRConfigureProbeOCR(r)&&[[[VNImageRequestHandler alloc]initWithCGImage:im options:@{}]performRequests:@[r] error:&error])for(VNRecognizedTextObservation *o in r.results)[lines addObject:[o topCandidates:1].firstObject.string?:@""];
 if(im)CGImageRelease(im);CGDataProviderRelease(p);CGColorSpaceRelease(s);return [lines componentsJoinedByString:@" "];
}
static BOOL savedMarker(NSString *path){NSTask *t=[NSTask new];t.executableURL=[NSURL fileURLWithPath:@"/usr/bin/unzip"];t.arguments=@[@"-p",path,@"word/document.xml"];NSPipe *out=[NSPipe pipe];t.standardOutput=out;t.standardError=[NSFileHandle fileHandleWithNullDevice];NSError *error=nil;if(![t launchAndReturnError:&error])return NO;NSData *d=[out.fileHandleForReading readDataToEndOfFile];[t waitUntilExit];if(t.terminationStatus||d.length>1024*1024)return NO;NSString *xml=[[NSString alloc]initWithData:d encoding:NSUTF8StringEncoding]?:@"";NSString *plain=[xml stringByReplacingOccurrencesOfString:@"<[^>]*>" withString:@"" options:NSRegularExpressionSearch range:NSMakeRange(0,xml.length)];return [plain containsString:@"LINUXRUNNER EDIT VERIFIED 20261028"];}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_ONLYOFFICE_DOCUMENT"))return 2;NSString *dir=@(argv[3]),*file=@(getenv("LR_ONLYOFFICE_DOCUMENT"));BOOL reopen=getenv("LR_ONLYOFFICE_REOPEN")!=NULL;NSString *needle=reopen?@"LINUXRUNNER EDIT VERIFIED 20261028":@"LINUXRUNNER ORIGINAL DOCUMENT";
 if(!reopen&&savedMarker(file)){fprintf(stderr,"Refusing edit test with preexisting expected marker\n");return 2;}
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil]];__block NSData *pixels;__block uint16_t width=0,height=0;__block uint64_t frames=0;__block BOOL ended=NO;
 c.frameHandler=^(NSData *d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){(void)b;dispatch_async(dispatch_get_main_queue(),^{pixels=d;width=w;height=h;frames=n;});};
 c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};[c start];NSDate *start=NSDate.date,*nextOCR=NSDate.distantPast;BOOL opened=NO,typed=NO,saved=NO,quitSent=NO,closeSent=NO;NSDate *quitAt=nil;NSString *ocr=@"";
 while(!ended&&-[start timeIntervalSinceNow]<120){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
  if(pixels&&[nextOCR timeIntervalSinceNow]<=0){ocr=text(pixels,width,height,[dir stringByAppendingPathComponent:@"latest.png"]);nextOCR=[NSDate dateWithTimeIntervalSinceNow:3];[ocr writeToFile:[dir stringByAppendingPathComponent:@"latest-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];}
  if(!opened&&[ocr.uppercaseString containsString:needle]){opened=YES;fprintf(stderr,"ONLYOFFICE_DOCUMENT_OPENED reopen=%d\n",reopen);if(!reopen){[c pointerX:width/2 y:height/2 buttons:1];[c pointerX:width/2 y:height/2 buttons:0];control(c,0xff57);key(c,0xff0d);const char *v="LINUXRUNNER EDIT VERIFIED 20261028";for(const char *p=v;*p;p++)key(c,(unsigned char)*p);control(c,'s');typed=YES;}}
  if(quitSent&&!closeSent&&-[quitAt timeIntervalSinceNow]>5){[c pointerX:width-14 y:26 buttons:1];[c pointerX:width-14 y:26 buttons:0];closeSent=YES;}
  if(opened&&(reopen||typed)&&savedMarker(file)){saved=YES;if(!quitSent){[pixels writeToFile:[dir stringByAppendingPathComponent:@"saved.bgra"] atomically:YES];control(c,'q');quitSent=YES;quitAt=NSDate.date;nextOCR=NSDate.distantPast;}}
 }
 text(pixels,width,height,[dir stringByAppendingPathComponent:@"final.png"]);[c stop];NSDictionary *r=@{@"environment":@"Mac ARM64 interpreter fixture; no physical device",@"opened_original_document":@(opened),@"typed_through_rfb":@(typed),@"saved_docx_independently_verified":@(saved),@"reopened_saved_document":@(reopen&&opened&&saved),@"clean_session_end":@(ended&&quitSent),@"frames":@(frames),@"last_ocr":ocr};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"onlyoffice-document.json"] atomically:YES];return opened&&saved&&ended?0:1;
}}
