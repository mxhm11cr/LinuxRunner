#import <XCTest/XCTest.h>

@interface LinuxRunnerUITests : XCTestCase
- (BOOL)waitForRenderedText:(NSString *)text app:(XCUIApplication *)app timeout:(NSTimeInterval)timeout;
@end

// Explicit physical-device operations on a separately installed reference root.
// Ordinary test runs skip these; they never import over an existing environment.
@implementation LinuxRunnerUITests (CaptureCleanup)
- (void)dismissCaptureCleanupExitNotice:(XCUIApplication *)app {
    XCUIElement *notice=app.navigationBars[@"Crash Details"];
    if([notice waitForExistenceWithTimeout:2]){
        // Forced session teardown can produce background guest exit reports
        // (e.g. kded6). Retain the notice; it is not a capture resource result.
        XCTAttachment *shot=[XCTAttachment attachmentWithScreenshot:app.screenshot];
        shot.name=@"Guest exit notice during capture teardown";
        shot.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:shot];
        NSLog(@"CAPTURE_CLEANUP_EXIT_NOTICE %@",app.debugDescription);
        [notice.buttons[@"Done"] tap];
    }
}

- (void)testReferenceCaptureForcedTermination {
    NSDictionary *env=NSProcessInfo.processInfo.environment;
    NSString *bundle=env[@"LR_REFERENCE_DEVICE_BUNDLE"],*root=env[@"LR_REFERENCE_ROOT"];
    XCTSkipIf(![env[@"LR_REFERENCE_CAPTURE_CLEANUP"] isEqual:@"YES"]||!bundle.length||!root.length,
              @"Requires explicit capture-cleanup opt-in, bundle, installed root and transferred monitor");
    self.continueAfterFailure=NO;self.executionTimeAllowance=1800;
    XCUIDevice.sharedDevice.orientation=UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app=[[XCUIApplication alloc]initWithBundleIdentifier:bundle];
    app.launchArguments=@[@"-Default Root",root,@"-LinuxRunnerGUISharedImages",@"YES",
        @"-LinuxRunnerDesktop.autoFullscreen",@"NO"];
    [self addTeardownBlock:^{[app terminate];app.launchArguments=@[@"-Default Root",root];[app launch];}];
    [app launch];
    if([app.buttons[@"Later"] waitForExistenceWithTimeout:3])[app.buttons[@"Later"] tap];
    XCUIElement *terminal=app.buttons[@"Terminal"].firstMatch;
    XCTAssertTrue([terminal waitForExistenceWithTimeout:60]);[terminal tap];
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    // A single monitor owns all five cycles, outside desktop process groups.
    // Keep the terminal closed during recovery to avoid focus/sheet races.
    [app typeText:@"rm -f /home/mobile/Downloads/capture-cleanup-all.log; nohup setsid /bin/sh /home/mobile/Downloads/capture-cleanup-device.sh all > /home/mobile/Downloads/capture-cleanup-all.log 2>&1 < /dev/null & while ! grep -q ARMED /home/mobile/Downloads/capture-cleanup-all.log; do sleep .2; done; cat /home/mobile/Downloads/capture-cleanup-all.log\n"];
    XCTAssertTrue([self waitForRenderedText:@"CAPTURE_MONITOR_ARMED_1" app:app timeout:40]);
    [app.buttons[@"Done"] tap];
    for(NSUInteger cycle=1;cycle<=5;cycle++){
        [self dismissCaptureCleanupExitNotice:app];
        XCUIElement *open=app.buttons[@"Open Desktop"];
        XCTAssertTrue([open waitForExistenceWithTimeout:30]);[open tap];
        XCUIElement *surface=[[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
        XCTAssertTrue([surface waitForExistenceWithTimeout:60]);
        NSPredicate *ready=[NSPredicate predicateWithFormat:@"value CONTAINS 'Desktop ready'"];
        XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ready object:surface]] timeout:300],XCTWaiterResultCompleted);
        // Bounded settle for the monitor's deliberate 10-second delay,
        // termination and five stable samples. The host still must require
        // the independent ALL_PASS log; elapsed time is never acceptance.
        [NSThread sleepForTimeInterval:25];
        XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:30]);[app.buttons[@"Done"] tap];
        // The independent monitor waits for desktop readiness, kills the
        // identified capture and checks counters.
        // The public Home state confirms the owned session actually ended.
        XCTAssertTrue([app.staticTexts[@"Stopped"] waitForExistenceWithTimeout:360]);
        [self dismissCaptureCleanupExitNotice:app];
        NSLog(@"CAPTURE_CLEANUP_UI_SESSION_STOPPED cycle=%lu",(unsigned long)cycle);
        XCTAssertEqual(app.state,XCUIApplicationStateRunningForeground);
    }
    // The device's capture-cleanup-all.log is the resource result. The host
    // must independently retrieve and require ALL_PASS; UI shutdown alone
    // does not prove buffer reclamation or a stable runtime identity.

}

- (void)testReferenceCapturePerformance {
    NSDictionary *env=NSProcessInfo.processInfo.environment;
    NSString *bundle=env[@"LR_REFERENCE_DEVICE_BUNDLE"],*root=env[@"LR_REFERENCE_ROOT"];
    XCTSkipIf(![env[@"LR_REFERENCE_CAPTURE_PROBE"] isEqual:@"YES"]||!bundle.length||!root.length,@"Requires explicit capture probe, bundle and installed root");
    self.continueAfterFailure=NO;
    XCUIDevice.sharedDevice.orientation=UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app=[[XCUIApplication alloc]initWithBundleIdentifier:bundle];
    app.launchArguments=@[@"-Default Root",root,@"-LinuxRunnerDesktop.captureProbe",@"YES",@"-LinuxRunnerDesktop.performanceDiagnostics",@"YES",
        @"-LinuxRunnerGUISharedImages",env[@"LR_REFERENCE_SHARED_IMAGES"]?:@"NO"];
    [self addTeardownBlock:^{[app terminate];app.launchArguments=@[@"-Default Root",root];[app launch];}];
    [app launch];
    if([app.buttons[@"Later"] waitForExistenceWithTimeout:3])[app.buttons[@"Later"] tap];
    XCUIElement *open=app.buttons[@"Open Desktop"];
    XCTAssertTrue([open waitForExistenceWithTimeout:60]);[open tap];
    XCUIElement *surface=[[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    NSPredicate *done=[NSPredicate predicateWithFormat:@"value CONTAINS 'Capture probe complete' OR value CONTAINS 'Capture probe failed'"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:done object:surface]] timeout:240],XCTWaiterResultCompleted);
    XCTAssertTrue([surface.value containsString:@"Capture probe complete"],@"%@",surface.value);
    XCTAttachment *shot=[XCTAttachment attachmentWithScreenshot:app.screenshot];shot.name=@"Controlled input and repaint capture probe";shot.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:shot];
}

@end
