#import "app/LinuxRunner/CapturePolicy.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    @autoreleasepool {
        NSString *suite = [@"LinuxRunnerCapturePolicy." stringByAppendingString:NSUUID.UUID.UUIDString];
        NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
        NSString *key = @"LinuxRunnerGUISharedImages";
        NSArray *catalog = @[@{@"rootfsName":@"a", @"distributionID":@"arch"},
            @{@"rootfsName":@"s", @"distributionID":@"steamos"},
            @{@"rootfsName":@"d", @"distributionID":@"debian"},
            @{@"rootfsName":@"unknown"}];
        for (NSDictionary *environment in catalog) {
            NSString *root = environment[@"rootfsName"];
            BOOL supported = [root isEqual:@"a"] || [root isEqual:@"s"];
            assert(LRDesktopSharedCaptureEnabled(defaults,root,catalog,@{}) == supported);
            assert([defaults objectForKey:key] == nil); // no implicit persistence
            [defaults setBool:NO forKey:key];
            assert(!LRDesktopSharedCaptureEnabled(defaults,root,catalog,@{}));
            [defaults setBool:YES forKey:key];
            assert(LRDesktopSharedCaptureEnabled(defaults,root,catalog,@{}));
            [defaults removeObjectForKey:key];
        }
        // Catalog entry (even one without a distribution) wins over stale
        // install metadata, and another root's identity cannot select a default.
        assert(!LRDesktopSharedCaptureEnabled(defaults,@"d",catalog,@{@"distributionID":@"arch"}));
        assert(!LRDesktopSharedCaptureEnabled(defaults,@"unknown",catalog,@{@"distributionID":@"steamos"}));
        assert(!LRDesktopSharedCaptureEnabled(defaults,@"absent",catalog,@{}));
        assert(!LRDesktopSharedCaptureEnabled(defaults,@"absent",catalog,@{@"distributionID":@"alpine"}));
        assert(!LRDesktopSharedCaptureEnabled(defaults,@"absent",catalog,@{@"distributionID":@"archarm"}));
        assert(LRDesktopSharedCaptureEnabled(defaults,@"absent",catalog,@{@"distributionID":@"arch"}));
        assert(LRDesktopSharedCaptureEnabled(defaults,@"absent",catalog,@{@"distributionID":@"steamos"}));
        [defaults removePersistentDomainForName:suite];
        puts("CAPTURE_POLICY_PASS explicit choices, catalog identity, metadata fallback, no migration");
    }
}
