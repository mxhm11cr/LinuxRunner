#import <Foundation/Foundation.h>
#import "RFBClient.h"
int main(int argc,char**argv){@autoreleasepool{
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:@"testpass"];
 __block NSString *last=@"";c.statusHandler=^(NSString*s){@synchronized(c){last=s;}};
 __block uint64_t coalesced=0,writes=0;__block double qsum=0,qmax=0;
 c.transportMetricsHandler=^(NSDictionary*e){@synchronized(c){writes++;double q=[e[@"input_queue_ms"] doubleValue];qsum+=q;qmax=MAX(qmax,q);coalesced=[e[@"coalesced_motion"] unsignedLongLongValue];}};
 [c start];usleep(500000);
 for(int r=0;r<5;r++){
  for(int i=0;i<1000;i++)[c pointerX:i y:r buttons:0];
  [c pointerX:1000 y:5 buttons:1];for(int i=0;i<500;i++)[c pointerX:1000+i y:6 buttons:1];[c pointerX:1500 y:6 buttons:0];
  [c key:'a' down:YES];[c key:'a' down:NO];
  for(int i=0;i<300;i++)[c pointerX:2000+i y:7 buttons:0];
  for(int k=0;k<3;k++){[c pointerX:2300 y:7 buttons:8];[c pointerX:2300 y:7 buttons:0];}
 }
 [c pointerX:4000 y:9 buttons:0];usleep(1500000);
 @synchronized(c){printf("status=%s writes=%llu coalesced=%llu queue_ms_avg=%.3f max=%.3f\n",last.UTF8String,writes,coalesced,writes?qsum/writes:0,qmax);}
 [c stop];usleep(200000);}return 0;}
