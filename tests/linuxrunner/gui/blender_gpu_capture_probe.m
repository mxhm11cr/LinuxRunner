// Native viewer evidence for blender_gpu_benchmark.py; no OCR service required.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
static void savePNG(NSData *data, uint16_t width, uint16_t height, NSString *path) {
    if (!data || !width || !height) return;
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGImageRef image=CGImageCreate(width,height,8,32,width*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,NO,kCGRenderingIntentDefault);
    CGImageDestinationRef output=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],CFSTR("public.png"),1,NULL);
    if(output){CGImageDestinationAddImage(output,image,NULL);CGImageDestinationFinalize(output);CFRelease(output);}
    CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);
}
int main(int argc,char **argv){@autoreleasepool{
    if(argc!=5 || !getenv("LR_BLENDER_HOST_RESULTS"))return 2;
    NSString *out=@(argv[3]),*resultPath=@(getenv("LR_BLENDER_HOST_RESULTS"));
    double limit=getenv("LR_BLENDER_SECONDS")?atof(getenv("LR_BLENDER_SECONDS")):180;
    if(limit<10||limit>600)return 2;
    LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
    NSLock *lock=[NSLock new];__block NSData *pixels;__block uint16_t width=0,height=0;__block uint64_t frames=0;__block BOOL ended=NO;
    client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t count,uint64_t bytes){(void)bytes;[lock lock];pixels=data;width=w;height=h;frames=count;[lock unlock];};
    client.statusHandler=^(NSString *status){fprintf(stderr,"%s\n",status.UTF8String);if([status hasPrefix:@"Linux display ended"]||[status hasPrefix:@"Display error:"]){[lock lock];ended=YES;[lock unlock];}};
    [client start];NSDate *start=NSDate.date;NSDictionary *benchmark=nil;NSDate *firstPixels=nil;BOOL opened=NO;
    while(-[start timeIntervalSinceNow]<limit){
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.25]];
        NSData *data=[NSData dataWithContentsOfFile:resultPath];
        if(data.length)benchmark=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        if(getenv("LR_BLENDER_HOST_UI_READY")&&!opened){
            [lock lock];BOOL haveFrame=pixels!=nil;uint16_t w=width,h=height;[lock unlock];
            if(haveFrame&&!firstPixels)firstPixels=NSDate.date;
            if(firstPixels&&-[firstPixels timeIntervalSinceNow]>12){
                // Controlled 960x540 first-run profile: accept Blender's
                // Quick Setup, then dismiss its splash using real RFB input.
                [client pointerX:w/2 y:h-64 buttons:1];[client pointerX:w/2 y:h-64 buttons:0];
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
                [client key:0xff1b down:YES];[client key:0xff1b down:NO];
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
                [client pointerX:100 y:100 buttons:1];[client pointerX:100 y:100 buttons:0];
                [@"viewer-input-sent\n" writeToFile:@(getenv("LR_BLENDER_HOST_UI_READY")) atomically:YES encoding:NSUTF8StringEncoding error:nil];
                opened=YES;
            }
        }
        if(benchmark){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2]];break;}
        [lock lock];BOOL disconnected=ended;[lock unlock];if(disconnected)break;
    }
    BOOL editSaved=getenv("LR_BLENDER_EDIT_SCENE")==NULL;
    if([benchmark[@"status"] isEqual:@"PASS"]&&getenv("LR_BLENDER_EDIT_SCENE")){
        NSString *scenePath=[[resultPath stringByDeletingPathExtension] stringByAppendingPathExtension:@"blend"];
        NSDate *before=[NSFileManager.defaultManager attributesOfItemAtPath:scenePath error:nil].fileModificationDate;
        [lock lock];uint16_t w=width,h=height;[lock unlock];
        // Real viewport input after the scripted benchmark: select all meshes,
        // translate by +1 on Z, then save the existing .blend file. The runner
        // must reopen it in Blender and assert positions; sending is not PASS.
        [client pointerX:w/3 y:h/2 buttons:1];[client pointerX:w/3 y:h/2 buttons:0];
        uint32_t keys[]={'a','g','z','1',0xff0d};
        for(unsigned i=0;i<5;i++){[client key:keys[i] down:YES];[client key:keys[i] down:NO];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.4]];}
        [client key:0xffe3 down:YES];[client key:'s' down:YES];[client key:'s' down:NO];[client key:0xffe3 down:NO];
        NSDate *waitStart=NSDate.date;
        while(-[waitStart timeIntervalSinceNow]<30){
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.25]];
            NSDate *after=[NSFileManager.defaultManager attributesOfItemAtPath:scenePath error:nil].fileModificationDate;
            if(before&&[after compare:before]==NSOrderedDescending){editSaved=YES;break;}
        }
    }
    [lock lock];NSData *captured=pixels;uint16_t w=width,h=height;uint64_t n=frames;[lock unlock];
    savePNG(captured,w,h,[out stringByAppendingPathComponent:@"blender-viewport.png"]);
    NSMutableSet *colors=[NSMutableSet new];const uint32_t *p=captured.bytes;
    for(NSUInteger i=0;i<captured.length/4;i+=97)[colors addObject:@(p[i]&0xffffff)];
    BOOL pass=[benchmark[@"status"] isEqual:@"PASS"]&&n>0&&colors.count>16&&editSaved;
    NSDictionary *report=@{@"status":pass?@"PASS":@"FAIL",@"benchmark_completed":@(benchmark!=nil),@"scene_file_changed_after_input":@(editSaved),@"frames":@(n),@"distinct_colors":@(colors.count),@"width":@(w),@"height":@(h),@"elapsed_seconds":@(-[start timeIntervalSinceNow]),@"scope":@"Mac interpreter and real native RFB viewer; not physical iOS"};
    [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"blender-display.json"] atomically:YES];
    [client stop];return pass?0:1;
}}
