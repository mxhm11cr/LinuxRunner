#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static volatile BOOL gotFrame = NO;
static void waitSeconds(double seconds) { NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds]; while ([end timeIntervalSinceNow] > 0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]]; }
int main(int argc, char **argv) { @autoreleasepool {
 if (argc != 5) return 2;
 NSString *password = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 NSString *session = [@(getenv("LR_GUI_APPLICATION_LOG")) stringByDeletingLastPathComponent];
 NSString *terminal = [session stringByAppendingPathComponent:@"terminal.log"];
 LRRFBClient *client = [[LRRFBClient alloc] initWithPort:atoi(argv[1]) password:password];
 client.frameHandler = ^(NSData *data, uint16_t w, uint16_t h, uint64_t frames, uint64_t bytes) { if (data.length && w && h && frames) gotFrame = YES; };
 [client start]; BOOL started = NO;
 for (int i=0; i<240; i++) { waitSeconds(.25); NSString *log = [NSString stringWithContentsOfFile:terminal encoding:NSUTF8StringEncoding error:nil]; if ([log containsString:@"I3_BEFORE="] && gotFrame) { started=YES; break; } }
 if (!started) { [client stop]; fprintf(stderr,"I3_INPUT_FAIL initial frame or baseline missing\n"); return 1; }
 [client key:0xffeb down:YES]; [client key:0xff0d down:YES]; [client key:0xff0d down:NO]; [client key:0xffeb down:NO];
 BOOL passed = NO;
 for (int i=0; i<160; i++) { waitSeconds(.25); NSString *log = [NSString stringWithContentsOfFile:terminal encoding:NSUTF8StringEncoding error:nil]; if ([log containsString:@"I3_NEW_WINDOW_PASS"]) { passed=YES; break; } if ([log containsString:@"I3_NEW_WINDOW_FAIL"]) break; }
 [client stop]; fprintf(stderr,"I3_INPUT_%s\n",passed ? "PASS" : "FAIL"); return passed ? 0 : 1;
}}
