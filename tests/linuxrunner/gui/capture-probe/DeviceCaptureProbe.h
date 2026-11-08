// Measurement-only implementation, compiled by the retained instrumentation
// patch. It is not part of an ordinary app build. Same license as LinuxRunner.
#import <Foundation/Foundation.h>
#include <time.h>

static double LRDeviceProbeClock(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static NSNumber *LRDeviceProbeSequence(NSData *pixels, uint16_t width, uint16_t height) {
    if (width < 240 || height < 48 || pixels.length != (NSUInteger)width * height * 4) return nil;
    const unsigned char *p = pixels.bytes;
    unsigned colors[3]; const unsigned x[] = {20, 100, 180};
    for (unsigned i = 0; i < 3; i++) {
        const unsigned char *q = p + ((NSUInteger)20 * width + x[i]) * 4;
        colors[i] = (unsigned)q[2] << 16 | (unsigned)q[1] << 8 | q[0];
    }
    if (colors[0] != 0x2457a9 || colors[1] != 0xb86c31) return nil;
    return @((colors[2] * 0xb7a087u) & 0xffffffu);
}

@interface LRDeviceCaptureProbe : NSObject
@property NSURL *output;
@property(copy) void (^send)(NSUInteger kind); // 0 click, 1 key, 2 start burst
@property BOOL active, finished, passed, scheduled;
@property unsigned phase, expected;
@property NSInteger thermalStart;
@property NSNumber *lastDecoded, *lastSubmitted;
@property double sentAt, decodedAt, burstSentAt, burstFirstAt, burstEndAt;
@property NSMutableArray<NSDictionary *> *events;
@property NSMutableDictionary<NSNumber *,NSNumber *> *decodedBurst;
@property NSMutableArray<NSDictionary *> *submittedBurst;
- (instancetype)initWithURL:(NSURL *)url send:(void (^)(NSUInteger))send;
- (void)decoded:(NSNumber *)sequence at:(double)time;
- (void)submitted:(NSNumber *)sequence at:(double)time;
- (void)invalidate;
@end

@implementation LRDeviceCaptureProbe
- (instancetype)initWithURL:(NSURL *)url send:(void (^)(NSUInteger))send {
    if ((self = [super init])) {
        _output = url; _send = [send copy]; _active = YES;
        _thermalStart = NSProcessInfo.processInfo.thermalState;
        _events = [NSMutableArray new]; _decodedBurst = [NSMutableDictionary new]; _submittedBurst = [NSMutableArray new];
        __weak typeof(self) weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            typeof(self) self = weak; if (self.active) [self finish:NO reason:@"probe timed out"];
        });
    }
    return self;
}
- (void)finish:(BOOL)passed reason:(NSString *)reason {
    if (!_active) return;
    _active = NO; _finished = YES; _passed = passed;
    NSMutableArray *decoded = [NSMutableArray new];
    for (NSNumber *sequence in [_decodedBurst.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [decoded addObject:@{@"id":sequence, @"decoded_s":_decodedBurst[sequence]}];
    NSDictionary *result = @{@"schema":@1, @"passed":@(passed), @"reason":reason,
        @"scope":@"Native RFB input enqueue to matching decoded pixels and main-thread framebuffer submission; excludes UIKit input, GPU completion and panel response",
        @"latency_events":_events, @"burst_generated":@120, @"burst_decoded":decoded, @"burst_submitted":_submittedBurst,
        @"expected_id":@(_expected), @"last_decoded_id":_lastDecoded?:@0, @"last_submitted_id":_lastSubmitted?:@0,
        @"thermal_start":@(_thermalStart), @"thermal_end":@(NSProcessInfo.processInfo.thermalState),
        @"burst_start_enqueued_s":@(_burstSentAt), @"burst_end_submitted_s":@(_burstEndAt)};
    if (![[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil] writeToURL:_output atomically:YES]) _passed = NO;
}
- (void)next {
    if (!_active || _scheduled) return;
    _scheduled = YES;
    __weak typeof(self) weak = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        typeof(self) self = weak; if (!self.active) return;
        self.scheduled = NO;
        if (self.events.count == 60) {
            self.phase = 2; self.burstSentAt = LRDeviceProbeClock(); self.send(2);
        } else {
            self.phase = 1; self.expected = (unsigned)self.events.count + 1;
            self.decodedAt = 0; self.sentAt = LRDeviceProbeClock(); self.send(self.events.count % 2);
            unsigned expected = self.expected;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                typeof(self) self = weak;
                if (self.active && self.phase == 1 && self.expected == expected)
                    [self finish:NO reason:@"matching input repaint timed out"];
            });
        }
    });
}
- (void)decoded:(NSNumber *)sequence at:(double)time {
    if (!_active || !sequence) return;
    _lastDecoded = sequence;
    unsigned n = sequence.unsignedIntValue;
    if (_phase == 1 && n == _expected && time >= _sentAt && !_decodedAt) _decodedAt = time;
    if (_phase == 2 && n > 0x010000 && n <= 0x010078 && !_decodedBurst[sequence]) _decodedBurst[sequence] = @(time);
}
- (void)submitted:(NSNumber *)sequence at:(double)time {
    if (!_active || !sequence) return;
    _lastSubmitted = sequence;
    unsigned n = sequence.unsignedIntValue;
    if (!_phase && n == 0) { [self next]; return; }
    if (_phase == 1 && n == _expected && _decodedAt >= _sentAt && time >= _decodedAt) {
        [_events addObject:@{@"id":@(n), @"kind":(_events.count % 2 ? @"keyboard" : @"click"),
            @"enqueued_s":@(_sentAt), @"decoded_s":@(_decodedAt), @"submitted_s":@(time),
            @"decode_ms":@((_decodedAt - _sentAt) * 1000), @"submission_ms":@((time - _sentAt) * 1000)}];
        _phase = 3; [self next]; return; // duplicates cannot count a second time
    }
    if (_phase == 2 && n > 0x010000 && n <= 0x010078 &&
        (!_submittedBurst.count || n > [_submittedBurst.lastObject[@"id"] unsignedIntValue])) {
        if (!_burstFirstAt) _burstFirstAt = time;
        [_submittedBurst addObject:@{@"id":@(n), @"submitted_s":@(time)}];
    }
    if (_phase == 2 && n == 0xf00078) {
        _burstEndAt = time;
        [self finish:_events.count == 60 && _submittedBurst.count > 1 reason:@"120-frame controlled repaint completed"];
    }
}
- (void)invalidate { if (_active) [self finish:NO reason:@"viewer stopped before probe completion"]; }
@end
