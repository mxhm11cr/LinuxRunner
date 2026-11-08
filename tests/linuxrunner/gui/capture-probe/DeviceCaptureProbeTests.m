#import "DeviceCaptureProbe.h"
#include <assert.h>

int main(void) { @autoreleasepool {
    NSMutableData *pixels=[NSMutableData dataWithLength:300*60*4];
    assert(!LRDeviceProbeSequence(pixels,300,60));
    unsigned colors[]={0x2457a9,0xb86c31,(7*0x1f2537u)&0xffffffu}, xs[]={20,100,180};
    for(unsigned i=0;i<3;i++){
        unsigned char *p=(unsigned char *)pixels.mutableBytes+(20*300+xs[i])*4;
        p[0]=colors[i]&255;p[1]=(colors[i]>>8)&255;p[2]=(colors[i]>>16)&255;
    }
    assert([LRDeviceProbeSequence(pixels,300,60) unsignedIntValue]==7);
    assert(!LRDeviceProbeSequence(pixels,300,59));
    for(unsigned n=0;n<256;n++){
        unsigned value=(n*0x1f2537u)&0xffffffu;
        unsigned char *p=(unsigned char *)pixels.mutableBytes+(20*300+180)*4;
        p[0]=value&255;p[1]=(value>>8)&255;p[2]=(value>>16)&255;
        assert([LRDeviceProbeSequence(pixels,300,60) unsignedIntValue]==n);
    }
    NSURL *file=[NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
    __block NSUInteger sends=0;
    LRDeviceCaptureProbe *probe=[[LRDeviceCaptureProbe alloc]initWithURL:file send:^(NSUInteger kind){(void)kind;sends++;}];
    for(unsigned i=1;i<=60;i++){
        probe.phase=1;probe.expected=i;probe.sentAt=100+i;probe.decodedAt=0;
        [probe decoded:@(i+1) at:100+i+.01];assert(probe.decodedAt==0);
        [probe decoded:@(i) at:100+i-.01];assert(probe.decodedAt==0);
        [probe submitted:@(i) at:100+i+.01];assert(probe.events.count==i-1);
        [probe decoded:@(i) at:100+i+.02];[probe decoded:@(i) at:100+i+.03];
        [probe submitted:@(i) at:100+i+.04];[probe submitted:@(i) at:100+i+.05];
        assert(probe.events.count==i);
        assert(fabs([probe.events.lastObject[@"decode_ms"] doubleValue]-20)<.0001);
        assert(fabs([probe.events.lastObject[@"submission_ms"] doubleValue]-40)<.0001);
    }
    probe.phase=2;probe.burstSentAt=200;
    for(unsigned i=1;i<=120;i++){
        [probe decoded:@(0x010000+i) at:200+i*.02];[probe decoded:@(0x010000+i) at:201+i*.02];
        if(i%2==0){[probe submitted:@(0x010000+i) at:200+i*.02+.001];[probe submitted:@(0x010000+i) at:200+i*.02+.002];}
    }
    [probe submitted:@0xf00078 at:203];assert(probe.finished&&probe.passed&&probe.submittedBurst.count==60);
    NSDictionary *result=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:file] options:0 error:nil];
    assert([result[@"passed"] boolValue]&&[result[@"latency_events"] count]==60&&[result[@"burst_decoded"] count]==120);
    [probe invalidate];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];assert(sends==0);
    [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    probe=[[LRDeviceCaptureProbe alloc]initWithURL:file send:^(NSUInteger kind){(void)kind;sends++;}];
    [probe submitted:@0 at:100];[probe invalidate];
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.2]];
    assert(sends==0&&probe.finished&&!probe.passed);
    [NSFileManager.defaultManager removeItemAtURL:file error:nil];
    puts("CAPTURE_PROBE_HOST_PASS marker bounds, causal IDs, duplicate rejection, timestamps, serialization, invalidation");
} return 0; }
