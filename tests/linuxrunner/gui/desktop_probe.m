#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static NSData*pixels;static unsigned short width,height;static uint64_t frames,bytes;static BOOL failure;
static void waitSeconds(double n){NSDate*end=[NSDate dateWithTimeIntervalSinceNow:n];while([end timeIntervalSinceNow]>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];}
static void capture(NSString*dir,NSString*name){[pixels writeToFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".bgra"]] atomically:YES];}
static void request(NSString *session, NSString *name, NSString *script) {
    NSString *user=@(getenv("LR_GUI_GUEST_USER") ?: "root");
    if (![user isEqual:@"root"]) {
        NSString *path=[NSString stringWithFormat:@"/home/mobile/Downloads/.linuxrunner/gui/%@/requests/launch-%@.sh",session.lastPathComponent,name];
        NSTask *writer=[NSTask new]; writer.launchPath=@(getenv("LR_GUI_RUNTIME"));
        writer.arguments=@[@"-f",@(getenv("LR_GUI_ROOTFS")),@"/bin/su",@"-l",@"-s",@"/bin/sh",@"-c",
            [NSString stringWithFormat:@"umask 077; cat > '%@.pending' && mv '%@.pending' '%@'",path,path,path],user];
        NSPipe *input=[NSPipe pipe];writer.standardInput=input;
        [writer launch];[input.fileHandleForWriting writeData:[script dataUsingEncoding:NSUTF8StringEncoding]];
        [input.fileHandleForWriting closeFile];[writer waitUntilExit];
        if(writer.terminationStatus)failure=YES;
        return;
    }
    [[NSFileManager defaultManager]createDirectoryAtPath:[session stringByAppendingPathComponent:@"requests"] withIntermediateDirectories:YES attributes:nil error:nil];
    [script writeToFile:[session stringByAppendingPathComponent:[NSString stringWithFormat:@"requests/launch-%@.sh",name]] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

static LRRFBClient* connect(int port,NSString*password){LRRFBClient*c=[[LRRFBClient alloc]initWithPort:port password:password];c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t f,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{pixels=d;width=w;height=h;frames=f;bytes=b;});};c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"])failure=YES;};[c start];return c;}
int main(int argc,char**argv){@autoreleasepool{if(argc!=5)return 2;NSString*dir=@(argv[3]);NSString*session=[@(getenv("LR_GUI_APPLICATION_LOG"))stringByDeletingLastPathComponent];NSString*password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];NSDate*start=NSDate.date;LRRFBClient*c=connect(atoi(argv[1]),password);BOOL ready=NO;while(-[start timeIntervalSinceNow]<100&&!failure){waitSeconds(.2);NSString*s=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil];if([s containsString:@"ready=1"]&&pixels){ready=YES;break;}}if(!ready){[c stop];return 1;}waitSeconds(3);capture(dir,@"desktop");[c pointerX:211 y:137 buttons:0];waitSeconds(.5);
request(session,@"input-probe",[NSString stringWithFormat:@"/home/mobile/Downloads/.linuxrunner/gui/desktop-probe --pointer > /home/mobile/Downloads/.linuxrunner/gui/%@/input-check\n",session.lastPathComponent]);
NSString *inputCheck=nil;for(int i=0;i<60&&![inputCheck containsString:@"pointer="];i++){waitSeconds(.25);inputCheck=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"input-check"] encoding:NSUTF8StringEncoding error:nil];}
BOOL inputOK=[inputCheck containsString:@"pointer=211,137"];fprintf(stderr,"INPUT_ROUNDTRIP %s %s\n",inputOK?"PASS":"FAIL",[inputCheck stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet].UTF8String?:"none");[c pointerX:30 y:height-20 buttons:0];waitSeconds(.3);[c pointerX:30 y:height-20 buttons:1];waitSeconds(.3);[c pointerX:30 y:height-20 buttons:0];waitSeconds(5);capture(dir,@"menu");[c key:0xff1b down:YES];[c key:0xff1b down:NO];
request(session,@"failure",@"exit 23\n");request(session,@"trap",@"exec /home/mobile/Downloads/desktop-fault\n");request(session,@"terminal",@"exec xterm -geometry 55x14+20+60 -title 'LinuxRunner Terminal'\n");request(session,@"files",@"if command -v pcmanfm-qt >/dev/null; then exec pcmanfm-qt --profile LinuxRunner-X11 \"$HOME\"; else exec pcmanfm \"$HOME\"; fi\n");waitSeconds(9);capture(dir,@"applications");
BOOL qtRequired=getenv("LR_REQUIRE_QT_DESKTOP")!=NULL;
BOOL gtkRequired=getenv("LR_REQUIRE_GTK_DESKTOP")!=NULL;
if(qtRequired||gtkRequired){
 [c pointerX:600 y:200 buttons:0];[c pointerX:600 y:200 buttons:1];[c pointerX:600 y:200 buttons:0];waitSeconds(.5);
 [c key:0xffe3 down:YES];[c key:'l' down:YES];[c key:'l' down:NO];[c key:0xffe3 down:NO];waitSeconds(.3);
 for(const char *p="/tmp";*p;p++){[c key:(unsigned char)*p down:YES];[c key:(unsigned char)*p down:NO];}
 [c key:0xff0d down:YES];[c key:0xff0d down:NO];waitSeconds(2);
 request(session,@"qt-contract",gtkRequired?@"QT_DESKTOP_EXPECT_CLASS=Pcmanfm QT_DESKTOP_EXPECT_TITLE=tmp exec /home/mobile/Downloads/qt-desktop-contract\n":@"QT_DESKTOP_EXPECT_TITLE=tmp exec /home/mobile/Downloads/qt-desktop-contract\n");
 for(int i=0;i<120;i++){waitSeconds(.2);if([NSFileManager.defaultManager fileExistsAtPath:[session stringByAppendingPathComponent:@"requests/launch-qt-contract.sh.result"]])break;}
}

uint64_t oldFrames=frames;[c stop];waitSeconds(2);pixels=nil;frames=0;bytes=0;c=connect(atoi(argv[1]),password);waitSeconds(5);BOOL reconnect=pixels&&frames>0;capture(dir,@"reconnected");
request(session,@"windows",[NSString stringWithFormat:@"exec /home/mobile/Downloads/desktop-windows '%@/desktop-test.state'\n",[@"/home/mobile/Downloads/.linuxrunner/gui" stringByAppendingPathComponent:session.lastPathComponent]]);NSString*state=nil;for(int i=0;i<160;i++){waitSeconds(.2);state=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"desktop-test.state"] encoding:NSUTF8StringEncoding error:nil];if([state hasPrefix:@"ready"])break;}int x=0,y=0;if(state)sscanf(state.UTF8String,"ready x=%d y=%d",&x,&y);[c pointerX:x+30 y:y+50 buttons:0];waitSeconds(.3);[c pointerX:x+30 y:y+50 buttons:1];waitSeconds(.3);[c pointerX:x+30 y:y+50 buttons:0];waitSeconds(.5);[c pointerX:x+30 y:y+50 buttons:1];waitSeconds(.3);[c pointerX:x+30 y:y+50 buttons:0];waitSeconds(.5);[c key:'X' down:YES];[c key:'X' down:NO];for(int i=0;i<160;i++){waitSeconds(.2);state=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"desktop-test.state"] encoding:NSUTF8StringEncoding error:nil];if([state hasPrefix:@"PASS"])break;}capture(dir,@"final");BOOL windows=[state hasPrefix:@"PASS"];NSString *childResult=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"requests/launch-failure.sh.result"] encoding:NSUTF8StringEncoding error:nil];BOOL isolated=[childResult hasPrefix:@"23"];NSString *trapResult=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"requests/launch-trap.sh.result"] encoding:NSUTF8StringEncoding error:nil];BOOL trapIsolated=[trapResult hasPrefix:@"133"];NSString *qtResult=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"requests/launch-qt-contract.sh.result"] encoding:NSUTF8StringEncoding error:nil];BOOL qtOK=!(qtRequired||gtkRequired)||[qtResult hasPrefix:@"0"];
NSDictionary*report=@{@"qt_file_manager_window":@(qtRequired&&qtOK),@"gtk_file_manager_window":@(gtkRequired&&qtOK),@"ready":@(ready),@"input_roundtrip":@(inputOK),@"child_failure_isolated":@(isolated),@"guest_brk_isolated":@(trapIsolated),@"guest_user":@(getenv("LR_GUI_GUEST_USER") ?: "root"),@"reconnect":@(reconnect),@"windows":state?:@"missing",@"width":@(width),@"height":@(height),@"frames_before_detach":@(oldFrames),@"frames_after_reconnect":@(frames),@"wire_bytes_after_reconnect":@(bytes),@"elapsed_seconds":@(-[start timeIntervalSinceNow]),@"environment":@"macOS host ARM64 interpreter",@"physical_device":@"NOT TESTED"};[[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"desktop-result.json"] atomically:YES];[c stop];return windows&&reconnect&&isolated&&trapIsolated&&qtOK&&inputOK&&!failure?0:1;}}
