#pragma once
#import <Foundation/Foundation.h>
// The filename must match StikDebug's bundled universal protocol script.
static inline NSURL *LRStikDebugRequest(NSString *bundleID, int processID, BOOL preparedRegion) {
    if (!bundleID.length || processID <= 0) return nil;
    NSURLComponents *components=[NSURLComponents componentsWithString:@"stikdebug://enable-jit"];
    NSMutableArray *items=[NSMutableArray arrayWithArray:@[
        [NSURLQueryItem queryItemWithName:@"bundle-id" value:bundleID],
        [NSURLQueryItem queryItemWithName:@"pid" value:[NSString stringWithFormat:@"%d",processID]]]];
    if(preparedRegion)[items addObject:[NSURLQueryItem queryItemWithName:@"script-name" value:@"universal.js"]];
    components.queryItems=items;
    return components.URL;
}
