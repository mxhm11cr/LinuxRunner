#import <Foundation/Foundation.h>
#include <signal.h>
#include <unistd.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
#ifdef LR_PLASMA_OCR
#import "ProbeOCRDevice.h"
#endif
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
static void typeASCII(LRRFBClient *client,const char *text) {
 size_t n=strlen(text);uint32_t keys[n];
 for(size_t i=0;i<n;i++)keys[i]=text[i]=='\n'?0xff0d:(unsigned char)text[i];
 [client typeKeysyms:keys count:n];
}
static BOOL waitText(NSString *path,NSString *expected,double seconds) {
 NSDate *start=NSDate.date;
 do {if([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] isEqual:expected])return YES;pump(.25);}while(-[start timeIntervalSinceNow]<seconds);
 return NO;
}
#ifdef LR_PLASMA_OCR
static NSString *recognize(CGImageRef image) {
 VNRecognizeTextRequest *request=[VNRecognizeTextRequest new];request.recognitionLevel=VNRequestTextRecognitionLevelAccurate;
 request.revision=VNRecognizeTextRequestRevision2;
 if(!LRConfigureProbeOCR(request))return @"";
 VNImageRequestHandler *handler=[[VNImageRequestHandler alloc]initWithCGImage:image options:@{}];
 NSError *error=nil;if(![handler performRequests:@[request] error:&error]){fprintf(stderr,"OCR_ERROR %s\n",error.localizedDescription.UTF8String);return @"";}
 NSMutableString *text=[NSMutableString new];
 for(VNRecognizedTextObservation *observation in request.results){NSString *line=[[observation topCandidates:1] firstObject].string;if(line)[text appendFormat:@"%@\n",line];}
 return text;
}
static NSString *ocr(void) {
 // Bound host Vision work in a separate process; a stalled OCR service must
 // never hold the desktop's test lease indefinitely or block RFB callbacks.
 NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
 [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:nil];
 snapshot(directory,@"frame");NSString *result=[directory stringByAppendingPathComponent:@"result"];
 [NSFileManager.defaultManager createFileAtPath:result contents:nil attributes:@{NSFilePosixPermissions:@0600}];
 NSTask *task=[NSTask new];task.launchPath=NSProcessInfo.processInfo.arguments[0];task.arguments=@[@"--ocr",[directory stringByAppendingPathComponent:@"frame.png"]];
 task.standardOutput=[NSFileHandle fileHandleForWritingAtPath:result];task.standardError=NSFileHandle.fileHandleWithStandardError;
 NSError *error=nil;BOOL started=[task launchAndReturnError:&error];NSDate *start=NSDate.date;
 while(started&&task.running&&-[start timeIntervalSinceNow]<15)pump(.05);
 if(started&&task.running){[task terminate];pump(.25);if(task.running)kill(task.processIdentifier,SIGKILL);fprintf(stderr,"OCR_TIMEOUT: host measurement unavailable\n");}
 NSString *text=@"";if(started&&!task.running&&task.terminationStatus==0){NSData *data=[NSData dataWithContentsOfFile:result];if(data.length<=65536)text=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding]?:@"";}
 [NSFileManager.defaultManager removeItemAtPath:directory error:nil];return text;
}
// Optional actual KWin window oracle for hosts where Vision cannot run. This
// reads sender-verified snapshots of real guest compositor windows.
static NSArray *kwinWindows(void) {
 const char *name=getenv("LR_PLASMA_WINDOW_LOG");if(!name)return @[];
 NSFileHandle *f=[NSFileHandle fileHandleForReadingAtPath:@(name)];if(!f)return @[];
 NSData *data=[f readDataOfLength:32769];[f closeFile];if(data.length>32768)return @[];
 id value=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
 if([value isKindOfClass:NSArray.class])return value;
 return @[];
}
static BOOL kwinWindowMatches(NSArray<NSString *> *required) {
 NSString *wanted=[[required componentsJoinedByString:@" "] lowercaseString];
 for(id item in kwinWindows()){
  if(![item isKindOfClass:NSDictionary.class])continue;NSDictionary *w=item;
  // A newly mapped window may not own keyboard focus yet. The next step sends
  // shortcuts, so readiness must wait for actual KWin activation as well.
  if(![w[@"active"] boolValue]||[w[@"minimized"] boolValue]||[w[@"hidden"] boolValue]||[w[@"width"] doubleValue]<100||[w[@"height"] doubleValue]<60)continue;
  NSString *app=[w[@"resourceClass"] description].lowercaseString,*caption=[w[@"caption"] description].lowercaseString;
  if([wanted containsString:@"dolphin"]&&[app containsString:@"dolphin"])return YES;
  if([wanted containsString:@"system settings"]&&[app containsString:@"systemsettings"])return YES;
  if([wanted containsString:@"save file"]&&[app containsString:@"kate"]&&[caption containsString:@"save file"])return YES;
  if([wanted containsString:@"kate"]&&[app containsString:@"kate"]&&(![wanted containsString:@"untitled"]||[caption containsString:@"untitled"]))return YES;
 }
 return NO;
}
static NSDictionary *activeKateWindow(void) {
 for(NSDictionary *window in kwinWindows())
  if([window[@"active"] boolValue]&&[[window[@"resourceClass"] description].lowercaseString containsString:@"kate"]&&[window[@"width"] doubleValue]>100&&[window[@"height"] doubleValue]>100)return window;
 return nil;
}
static BOOL waitWindow(NSArray<NSString *> *required,double seconds){
 NSDate *start=NSDate.date;
 do{pump(1);if(getenv("LR_PLASMA_WINDOW_LOG")){if(kwinWindowMatches(required))return YES;continue;}NSString *text=ocr();BOOL ok=YES;for(NSString *value in required)if([text rangeOfString:value options:NSCaseInsensitiveSearch].location==NSNotFound)ok=NO;if(ok)return YES;}while(-[start timeIntervalSinceNow]<seconds);
 return NO;
}
static void chord(LRRFBClient *client,uint32_t modifier,uint32_t key){[client key:modifier down:YES];[client key:key down:YES];[client key:key down:NO];[client key:modifier down:NO];}
static void launchMenu(LRRFBClient *client,const char *name){
 [client pointerX:22 y:height-22 buttons:1];[client pointerX:22 y:height-22 buttons:0];pump(2);
 typeASCII(client,name);pump(3);[client key:0xff0d down:YES];[client key:0xff0d down:NO];
}
static void closeWindow(LRRFBClient *client){chord(client,0xffe9,0xffc1);pump(2);}
#endif
int main(int argc,char **argv){@autoreleasepool{
#ifdef LR_PLASMA_OCR
 if(argc==3&&!strcmp(argv[1],"--ocr")){
  signal(SIGALRM,SIG_DFL);alarm(15);
  CGImageSourceRef source=CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[2])],NULL);
  if(!source)return 2;CGImageRef image=CGImageSourceCreateImageAtIndex(source,0,NULL);CFRelease(source);if(!image)return 2;
  NSString *text=recognize(image);CGImageRelease(image);fputs(text.UTF8String,stdout);return text.length?0:1;
 }
#endif
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
  // Blank output cannot qualify; require stability before testing the menu.
  if(delta>0.002||colors<16)stableSince=t;
  else if(t-stableSince>=20){settled=t;break;}
  last=now;}
 NSMutableDictionary *r=[@{@"environment":@"macOS host interpreter + native RFB probe; not physical iOS",@"desktop_environment":@(getenv("LR_PROBE_DESKTOP_NAME")?:"plasma"),@"first_frame_s":frames?@((int)firstFrame):@(-1),
  @"settled_s":@((int)settled),@"width":@(width),@"height":@(height),@"distinct_colors":@(colors),@"timeline":timeline} mutableCopy];
 if(getenv("LR_PLASMA_WINDOW_LOG"))r[@"application_window_oracle"]=@"Actual KWin managed-window snapshots; sender verified by guest oracle; filesystem checks verify GUI actions";
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
 if(getenv("LR_PLASMA_INPUT_DIRECTORY")&&[r[@"launcher_closed_after_escape_s"] doubleValue]>0){
  NSString *directory=@(getenv("LR_PLASMA_INPUT_DIRECTORY"));
  unsigned lx=22,ly=height-22;
  [client pointerX:lx y:ly buttons:1];[client pointerX:lx y:ly buttons:0];pump(2);
  typeASCII(client,"LinuxRunner Input Test");pump(3);
  snapshot(out,@"terminal-launch-search");
  [client key:0xff0d down:YES];[client key:0xff0d down:NO];
  BOOL ready=waitText([directory stringByAppendingPathComponent:@"terminal.ready"],@"TERMINAL_READY",60);
  r[@"konsole_launched_from_menu"]=@(ready);
  BOOL typed=NO,reconnected=NO;
  if(ready){
   pump(2);typeASCII(client,"printf PLASMA_EIS_INPUT_OK > /home/mobile/Downloads/logs/eis-input-proof\n");
   typed=waitText([directory stringByAppendingPathComponent:@"eis-input-proof"],@"PLASMA_EIS_INPUT_OK",40);
   snapshot(out,@"konsole-typed");
   if(typed){
    // Abrupt viewer loss must release a held modifier in the EIS backend.
    [client key:0xffe3 down:YES];pump(.25);r[@"disconnect_with_control_held"]=@YES;
    [client stop];pump(1);
    [lock lock];frames=0;pixels=nil;[lock unlock];
    client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
    client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){(void)bytes;[lock lock];pixels=data;width=w;height=h;frames=f;[lock unlock];};
    client.statusHandler=^(NSString *s){status=s;};[client start];
    NSDate *reconnectStart=NSDate.date;
    while(!frames&&-[reconnectStart timeIntervalSinceNow]<30)pump(.25);
    if(frames){pump(2);typeASCII(client,"printf RECONNECT_EIS_OK > /home/mobile/Downloads/logs/eis-reconnect-proof\n");
     reconnected=waitText([directory stringByAppendingPathComponent:@"eis-reconnect-proof"],@"RECONNECT_EIS_OK",40);}
    snapshot(out,@"konsole-reconnected");
   }
  }
  r[@"typed_shell_command_executed"]=@(typed);r[@"reconnected_typed_command_executed"]=@(reconnected);
 }
#ifdef LR_PLASMA_OCR
 if(getenv("LR_PLASMA_APPLICATIONS")&&[r[@"reconnected_typed_command_executed"] boolValue]){
  NSString *home=@(getenv("LR_PLASMA_GUEST_HOME")?:"");
  NSString *identifier=@(getenv("LR_PLASMA_TEST_ID")?:"");
  NSCharacterSet *valid=[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-"];
  BOOL safe=home.length&&identifier.length&&[identifier rangeOfCharacterFromSet:valid.invertedSet].location==NSNotFound;
  NSString *folder=[@"plasma901-folder-" stringByAppendingString:identifier],*file=[[@"plasma901-kate-" stringByAppendingString:identifier] stringByAppendingString:@".txt"];
  BOOL unused=safe&&![NSFileManager.defaultManager fileExistsAtPath:[home stringByAppendingPathComponent:folder]]&&![NSFileManager.defaultManager fileExistsAtPath:[home stringByAppendingPathComponent:file]];
  BOOL dolphin=NO,created=NO,kate=NO,saved=NO,settings=NO;
  if(unused){
   launchMenu(client,"Dolphin");
   dolphin=waitWindow(@[@"Dolphin",@"Places"],45);snapshot(out,@"dolphin");
   if(dolphin){
    [client key:0xffe3 down:YES];[client key:0xffe1 down:YES];[client key:'n' down:YES];[client key:'n' down:NO];[client key:0xffe1 down:NO];[client key:0xffe3 down:NO];pump(2);
    typeASCII(client,folder.UTF8String);[client key:0xff0d down:YES];[client key:0xff0d down:NO];
    NSDate *start=NSDate.date;do{pump(.25);BOOL directory=NO;created=[NSFileManager.defaultManager fileExistsAtPath:[home stringByAppendingPathComponent:folder] isDirectory:&directory]&&directory;}while(!created&&-[start timeIntervalSinceNow]<15);
    snapshot(out,@"dolphin-folder-created");closeWindow(client);
    launchMenu(client,"Kate");kate=waitWindow(@[@"Kate"],45);
    if(kate){
     if(getenv("LR_PLASMA_WINDOW_LOG")){
      // Exercise the visible New action, including real pointer focus, rather
      // than racing a shortcut against initial Wayland keyboard activation.
      // The managed fixture uses Kate's default toolbar; no user config reset.
      NSDictionary *window=activeKateWindow();int x=[window[@"x"] intValue]+40,y=[window[@"y"] intValue]+82;
      if(window&&x>=0&&y>=0&&x<width&&y<height){[client pointerX:x y:y buttons:1];[client pointerX:x y:y buttons:0];r[@"kate_new_file_action"]=@"visible-toolbar";}
      else kate=NO;
     }else{chord(client,0xffe3,'n');r[@"kate_new_file_action"]=@"keyboard-shortcut";}
     if(kate)kate=waitWindow(@[@"Kate",@"Untitled"],20);
    }
    snapshot(out,@"kate");
    if(kate){
     typeASCII(client,"LinuxRunner Plasma 901\nEdited through EIS\n");chord(client,0xffe3,'s');
     BOOL dialog=waitWindow(@[@"Save File",@"Name"],15);
     if(!dialog&&waitWindow(@[@"specified folder does not exist"],2)){
      snapshot(out,@"kate-stale-location-notice");
      [client key:0xff0d down:YES];[client key:0xff0d down:NO];
      dialog=waitWindow(@[@"Save File",@"Name"],20);
     }
     r[@"kate_save_dialog"]=@(dialog);
     if(dialog){chord(client,0xffe9,0xff50);pump(2);}
     snapshot(out,@"kate-save-dialog");
     // Wait for the asynchronous KDE file dialog before targeting its Name
     // field. An early batch can otherwise go to the editor or URL completer.
     if(dialog){chord(client,0xffe9,'n');chord(client,0xffe3,'a');typeASCII(client,file.UTF8String);
      pump(1);[client key:0xff0d down:YES];[client key:0xff0d down:NO];
      saved=waitText([home stringByAppendingPathComponent:file],@"LinuxRunner Plasma 901\nEdited through EIS\n",25);}

     snapshot(out,@"kate-saved");if(saved)closeWindow(client);
    }
    if(saved){launchMenu(client,"System Settings");settings=waitWindow(@[@"System Settings",@"Appearance"],45);snapshot(out,@"system-settings");if(settings)closeWindow(client);}
   }
  }
  r[@"dolphin_window"]=@(dolphin);r[@"dolphin_created_folder"]=@(created);r[@"kate_window"]=@(kate);r[@"kate_edited_and_saved"]=@(saved);r[@"systemsettings_window"]=@(settings);r[@"application_test_paths_unused"]=@(unused);
 }
#endif
 if(getenv("LR_PROBE_WAIT_TERMINAL")&&getenv("LR_GUI_APPLICATION_LOG")){
  NSString *done=[@(getenv("LR_GUI_APPLICATION_LOG")).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"terminal.done"];
  while(-[start timeIntervalSinceNow]<limit&&![NSFileManager.defaultManager fileExistsAtPath:done])pump(5);
  snapshot(out,@"terminal-done");r[@"terminal_done"]=@([NSFileManager.defaultManager fileExistsAtPath:done]);}
 BOOL pass=settled>0&&[r[@"launcher_opened_after_click_s"] doubleValue]>0&&[r[@"launcher_closed_after_escape_s"] doubleValue]>0;
 if(getenv("LR_PLASMA_INPUT_DIRECTORY"))pass=pass&&[r[@"konsole_launched_from_menu"] boolValue]&&[r[@"typed_shell_command_executed"] boolValue]&&[r[@"reconnected_typed_command_executed"] boolValue];
#ifdef LR_PLASMA_OCR
 if(getenv("LR_PLASMA_APPLICATIONS"))pass=pass&&[r[@"dolphin_created_folder"] boolValue]&&[r[@"kate_edited_and_saved"] boolValue]&&[r[@"systemsettings_window"] boolValue];
#endif
 r[@"status"]=pass?@"PASS":@"FAIL";[client stop];
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"plasma-rfb.json"] atomically:YES];
 return pass?0:1;
}}
