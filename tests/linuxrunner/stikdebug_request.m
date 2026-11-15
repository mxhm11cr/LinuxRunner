#import <Foundation/Foundation.h>
#include <assert.h>
#import "app/LinuxRunner/StikDebugRequest.h"
int main(void) { @autoreleasepool {
    for(int prepared=0;prepared<2;prepared++) {
        NSURL *url=LRStikDebugRequest(@"dev.linuxrunner.app",1234,prepared);
        NSURLComponents *parts=[NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
        assert([parts.scheme isEqual:@"stikdebug"] && [parts.host isEqual:@"enable-jit"]);
        NSMutableDictionary *items=[NSMutableDictionary dictionary];
        for(NSURLQueryItem *item in parts.queryItems)items[item.name]=item.value;
        assert([items[@"bundle-id"] isEqual:@"dev.linuxrunner.app"]);
        assert([items[@"pid"] isEqual:@"1234"]);
        assert(prepared ? [items[@"script-name"] isEqual:@"universal.js"] : items[@"script-name"]==nil);
        assert(items.count==(prepared?3:2));
    }
    assert(!LRStikDebugRequest(@"",1234,YES));
    assert(!LRStikDebugRequest(@"dev.linuxrunner.app",0,YES));
    return 0;
} }
