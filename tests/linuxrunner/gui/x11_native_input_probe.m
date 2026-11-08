#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"

// Connect only after dismissing the app's viewer (RFB ClientInit is exclusive).
// Uses the production RFB client against the Simulator's real guest desktop.
// Proof files are written by commands typed into an actual, non-root xterm;
// this probe only reads them, never mounts or mutates the running guest root.
static NSData *pixels;
static uint16_t width, height;
static uint64_t frames;
static void spin(double seconds) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (end.timeIntervalSinceNow > 0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
}
static BOOL until(double seconds, BOOL (^predicate)(void)) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while (end.timeIntervalSinceNow > 0) { if (predicate()) return YES; spin(.1); }
    return predicate();
}
static LRRFBClient *connectDisplay(int port, NSString *password) {
    pixels = nil; frames = 0;
    LRRFBClient *client = [[LRRFBClient alloc] initWithPort:port password:password];
    client.statusHandler = ^(NSString *status) { fprintf(stderr,"%s\n",status.UTF8String); };
    client.frameHandler = ^(NSData *data, uint16_t w, uint16_t h, uint64_t count, uint64_t bytes) {
        (void)bytes;
        dispatch_async(dispatch_get_main_queue(), ^{ pixels = data; width = w; height = h; frames = count; });
    };
    [client start]; return client;
}
static void type(LRRFBClient *client, NSString *text) {
    // Use the app's bounded bulk-paste API, rather than flooding the ordinary
    // key-event queue with hundreds of separately scheduled jobs.
    uint32_t *keys = calloc(text.length, sizeof(*keys));
    for (NSUInteger i = 0; i < text.length; i++) {
        keys[i] = [text characterAtIndex:i]; if (keys[i] == '\n') keys[i] = 0xff0d;
    }
    [client typeKeysyms:keys count:text.length]; free(keys);
}
static BOOL geometry(NSString *file, int *x, int *y, int *w, int *h) {
    NSString *text = [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil];
    NSRegularExpression *pattern = [NSRegularExpression regularExpressionWithPattern:@"(\\d+)x(\\d+)([-+]\\d+)([-+]\\d+)\\s+([-+]\\d+)([-+]\\d+)" options:0 error:nil];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        if (![line containsString:@"\"XTerm\""]) continue;
        NSTextCheckingResult *m = [pattern firstMatchInString:line options:0 range:NSMakeRange(0,line.length)];
        if (!m) continue;
        *w = [[line substringWithRange:[m rangeAtIndex:1]] intValue];
        *h = [[line substringWithRange:[m rangeAtIndex:2]] intValue];
        *x = [[line substringWithRange:[m rangeAtIndex:5]] intValue];
        *y = [[line substringWithRange:[m rangeAtIndex:6]] intValue];
        return *w > 100 && *h > 100;
    }
    return NO;
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 6) return 2;
    NSString *home = @(argv[3]), *out = @(argv[4]), *token = @(argv[5]);
    if (!token.length || [token rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyz0123456789-"] invertedSet]].location != NSNotFound) return 2;
    NSString *(^proof)(NSString *) = ^NSString *(NSString *suffix) { return [home stringByAppendingPathComponent:[token stringByAppendingString:suffix]]; };
    for (NSString *suffix in @[@"-input", @"-before", @"-after", @"-reconnect"])
        if ([NSFileManager.defaultManager fileExistsAtPath:proof(suffix)]) return 2;
    NSString *password = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
    LRRFBClient *client = connectDisplay(atoi(argv[1]), password);
    BOOL streamed = until(30, ^BOOL{ return pixels.length > 0 && width > height; });
    BOOL executed = NO, moved = NO, reconnected = NO;unsigned attempts=0;
    if (streamed) {
        // The managed Openbox configuration binds Super+Return to xterm.
        [client key:0xffeb down:YES]; [client key:0xff0d down:YES];
        [client key:0xff0d down:NO]; [client key:0xffeb down:NO]; spin(8);
        // Cold xterm/font initialization can outlast the first key sequence.
        // Retry only this idempotent probe command in its owned test terminal;
        // the proof still has to be produced by the real non-root guest shell.
        while(!executed&&attempts++<4){
            [client key:0xffe3 down:YES];[client key:'c' down:YES];
            [client key:'c' down:NO];[client key:0xffe3 down:NO];spin(.2);
            type(client, [NSString stringWithFormat:@"xwininfo -root -tree >~/%@-before; printf 'NATIVE_INPUT_OK uid=%%s\\n' \"$(id -u)\" >~/%@-input\n", token, token]);
            executed = until(12, ^BOOL{
                NSString *s = [NSString stringWithContentsOfFile:proof(@"-input") encoding:NSUTF8StringEncoding error:nil];
                return [s hasPrefix:@"NATIVE_INPUT_OK uid="] && [[s componentsSeparatedByString:@"="].lastObject intValue] >= 1000;
            });
        }
        int x=0,y=0,w=0,h=0;
        if (executed && geometry(proof(@"-before"), &x,&y,&w,&h) && y >= 10) {
            int px = MAX(0,MIN(width-80,x+50)), py = y-10;
            [client pointerX:px y:py buttons:0]; spin(.2);
            [client pointerX:px y:py buttons:1];
            for (int i=1;i<=8;i++) { [client pointerX:px+i*6 y:py+i*3 buttons:1]; spin(.05); }
            [client pointerX:px+48 y:py+24 buttons:0]; spin(1);
            type(client, [NSString stringWithFormat:@"xwininfo -root -tree >~/%@-after\n",token]);
            int nx=0,ny=0,nw=0,nh=0;
            moved = until(15, ^BOOL{ int a,b,c,d; return geometry(proof(@"-after"),&a,&b,&c,&d); }) &&
                geometry(proof(@"-after"),&nx,&ny,&nw,&nh) && abs(nx-x)>=20 && abs(ny-y)>=8;
        }
        [pixels writeToFile:[out stringByAppendingPathComponent:@"desktop.bgra"] atomically:YES];
    }
    [client stop]; spin(.5);
    if (executed) {
        client = connectDisplay(atoi(argv[1]),password);
        if (until(20, ^BOOL{ return pixels.length > 0; })) {
            type(client,[NSString stringWithFormat:@"printf 'RECONNECTED_OK\\n' >~/%@-reconnect\n",token]);
            reconnected = until(20, ^BOOL{ return [[NSString stringWithContentsOfFile:proof(@"-reconnect") encoding:NSUTF8StringEncoding error:nil] isEqual:@"RECONNECTED_OK\n"]; });
        }
        // Leave the real xterm visible for the app viewer reconnect/shutdown test.
        [client stop];
    }
    NSDictionary *result = @{@"status":streamed&&executed&&moved&&reconnected?@"PASS":@"FAIL", @"landscape_frames":@(streamed), @"normal_user_keyboard_command":@(executed), @"command_attempts":@(attempts), @"pointer_window_drag":@(moved), @"reconnected_command":@(reconnected), @"width":@(width), @"height":@(height), @"frames":@(frames), @"physical_device":@NO, @"scope":@"Production host RFB client controlling the iOS Simulator's guest X11 desktop; physical touch/monitor untested"};
    [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[out stringByAppendingPathComponent:@"input-result.json"] atomically:YES];
    return streamed&&executed&&moved&&reconnected?0:1;
} }
