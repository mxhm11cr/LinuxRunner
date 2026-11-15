// Regression: a cursor shape whose hotspot lies outside its image must not end
// the RFB connection ("Display error: Invalid cursor shape" froze GNOME/Zorin
// desktops when typing hid the pointer). With "grow": a rectangle beyond the
// framebuffer before NewFBSize must not end it either ("Unsupported encoding or
// invalid rectangle" when GNOME resized the X screen). Runs against
// rfb_cursor_hotspot_server.py [grow]; this binary takes PORT [grow].
//   clang -fobjc-arc -I../../../app/LinuxRunner rfb_cursor_hotspot_regress.m ../../../app/LinuxRunner/RFBClient.m ../../../app/LinuxRunner/RFBProtocol.c -framework Foundation -o rfb-cursor
#import <Foundation/Foundation.h>
#import "RFBClient.h"
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        LRRFBClient *client = [[LRRFBClient alloc] initWithPort:(uint16_t) atoi(argv[1]) password:@"testtest"];
        BOOL grow = argc > 2 && !strcmp(argv[2], "grow");   // out-of-bounds rectangle before NewFBSize
        BOOL copyrect = argc > 2 && !strcmp(argv[2], "copyrect");   // window move as CopyRect
        __block int frames = 0; __block BOOL copied = NO;
        __block BOOL frame = NO, cursor = NO; __block NSString *failure = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        client.frameHandler = ^(NSData *bgra, uint16_t w, uint16_t h, uint64_t n, uint64_t bytes) { if (copyrect) { if (++frames == 2) { const uint8_t *p = bgra.bytes; copied = bgra.length >= 4 && p[0] == 0x33 && p[1] == 0x22 && p[2] == 0x11; frame = YES; dispatch_semaphore_signal(done); } return; }
            if (w == (grow ? 32 : 16) && h == 16) { frame = YES; dispatch_semaphore_signal(done); } };
        client.cursorHandler = ^(NSData *bgra, uint16_t w, uint16_t h, uint16_t hx, uint16_t hy) { cursor = YES; if (hx >= w || hy >= h) failure = @"hotspot not clamped"; };
        client.statusHandler = ^(NSString *status) { if ([status hasPrefix:@"Display error:"]) { failure = status; dispatch_semaphore_signal(done); } };
        [client start];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));
        [client stop];
        BOOL ok = frame && (cursor || grow || (copyrect && copied)) && !failure;
        printf("%s: frame=%d cursor=%d failure=%s result=%s\n", copyrect ? (copied ? "RFB_COPYRECT copied=1" : "RFB_COPYRECT copied=0") : grow ? "RFB_RECT_GROW" : "RFB_CURSOR_HOTSPOT", frame, cursor, failure.UTF8String ?: "none", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
