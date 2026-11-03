#import <XCTest/XCTest.h>

@interface LinuxRunnerUITests : XCTestCase
- (BOOL)waitForRenderedText:(NSString *)text app:(XCUIApplication *)app timeout:(NSTimeInterval)timeout;
@end

// Explicit physical-device operations on a separately installed reference root.
// Ordinary test runs skip these; they never import over an existing environment.
@implementation LinuxRunnerUITests (ReferenceDistribution)
- (void)testReferenceUnixSocketNamespace {
    NSDictionary *env=NSProcessInfo.processInfo.environment;
    NSString *bundle=env[@"LR_REFERENCE_DEVICE_BUNDLE"],*root=env[@"LR_REFERENCE_ROOT"];
    XCTSkipIf(!bundle.length||!root.length,@"Requires explicit installed reference root and bundle");
    self.continueAfterFailure=NO;
    XCUIApplication *app=[[XCUIApplication alloc]initWithBundleIdentifier:bundle];
    app.launchArguments=@[@"-Default Root",root];[app launch];
    if([app.buttons[@"Later"] waitForExistenceWithTimeout:3])[app.buttons[@"Later"] tap];
    XCUIElement *terminal=app.buttons[@"Terminal"].firstMatch;
    XCTAssertTrue([terminal waitForExistenceWithTimeout:60]);[terminal tap];
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:20]);
    [app typeText:@"gcc /home/mobile/Downloads/unix_socket_namespace_contract.c -O2 -o /home/mobile/Downloads/reference-unix-sockets && /home/mobile/Downloads/reference-unix-sockets | tee /home/mobile/Downloads/unix-socket-reference.txt\n"];
    NSString *marker=[env[@"LR_REFERENCE_EXPECT_SOCKET_FAILURE"] isEqual:@"YES"]?@"UNIX_NAMESPACE_FAIL":@"UNIX_NAMESPACE_PASS";
    XCTAssertTrue([self waitForRenderedText:marker app:app timeout:90]);
    XCTAttachment *shot=[XCTAttachment attachmentWithScreenshot:app.screenshot];shot.name=marker;shot.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:shot];
}

- (void)testInstallArchReference {
    NSDictionary *env=NSProcessInfo.processInfo.environment;
    NSString *bundle=env[@"LR_REFERENCE_DEVICE_BUNDLE"],*original=env[@"LR_REFERENCE_ORIGINAL_ROOT"];
    XCTSkipIf(![env[@"LR_REFERENCE_INSTALL_ARCH"] isEqual:@"YES"]||!bundle.length||!original.length,@"Requires explicit reference-install opt-in, bundle and original root");
    self.continueAfterFailure=NO;self.executionTimeAllowance=2400;
    XCUIDevice.sharedDevice.orientation=UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app=[[XCUIApplication alloc]initWithBundleIdentifier:bundle];
    app.launchArguments=@[@"-Default Root",original,@"-LinuxRunnerDesktop.performanceDiagnostics",@"YES"];
    [app launch];
    // Also qualify the reviewed correction: the installed SteamOS environment can still open before installing Arch.
    if(![env[@"LR_REFERENCE_SKIP_RECOVERY_SMOKE"] isEqual:@"YES"]){
    XCUIElement *open=app.buttons[@"Open Desktop"];
    XCTAssertTrue([open waitForExistenceWithTimeout:60],@"%@",app.debugDescription);[open tap];
    XCUIElement *surface=[[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    NSPredicate *ready=[NSPredicate predicateWithFormat:@"value CONTAINS 'Desktop ready'"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ready object:surface]] timeout:240],XCTWaiterResultCompleted);
    XCTAttachment *before=[XCTAttachment attachmentWithScreenshot:app.screenshot];before.name=@"SteamOS recovery smoke";before.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:before];
    [app terminate];[app launch];
    }
    // iPadOS exposes its top tab strip outside XCUIElementTypeTabBar.
    XCUIElement *distributions=app.buttons[@"Distributions"].firstMatch;
    XCTAssertTrue([distributions waitForExistenceWithTimeout:60],@"%@",app.debugDescription);
    [distributions tap];
    XCUIElement *setup=app.buttons[@"Set Up Linux…"].firstMatch;
    XCTAssertTrue([setup waitForExistenceWithTimeout:15]);[setup tap];
    XCTAssertTrue([app.buttons[@"Continue"] waitForExistenceWithTimeout:15]);[app.buttons[@"Continue"] tap];
    XCUIElement *arch=app.buttons[@"Arch Linux ARM (ARM64) — experimental"];
    XCTAssertTrue([arch waitForExistenceWithTimeout:15]);[arch tap];
    // A retry must not accidentally create a second reference installation.
    XCTAssertFalse(app.buttons[@"Configure Arch Linux ARM"].exists,@"Arch already exists; select its root for the reference boot test");
    [app.buttons[@"Continue"] tap];
    XCUIElement *install=app.buttons[@"Install Arch Linux ARM"];
    XCTAssertTrue([install waitForExistenceWithTimeout:15]);XCTAssertTrue(install.enabled);[install tap];
    NSPredicate *finished=[NSPredicate predicateWithBlock:^BOOL(id object,NSDictionary *bindings){
        return app.navigationBars[@"Restart LinuxRunner"].exists||app.buttons[@"Try Again"].exists;
    }];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:finished object:app]] timeout:1800],XCTWaiterResultCompleted);
    XCTAssertTrue(app.navigationBars[@"Restart LinuxRunner"].exists,@"%@",app.debugDescription);
    XCTAttachment *installed=[XCTAttachment attachmentWithScreenshot:app.screenshot];installed.name=@"Arch Linux ARM installed through verified package installer";installed.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:installed];
}

- (void)testArchReferenceDesktopAndTerminal {
    NSDictionary *env=NSProcessInfo.processInfo.environment;
    NSString *bundle=env[@"LR_REFERENCE_DEVICE_BUNDLE"],*root=env[@"LR_REFERENCE_ROOT"];
    XCTSkipIf(!bundle.length||!root.length,@"Requires explicit installed Arch reference root and bundle");
    self.continueAfterFailure=NO;self.executionTimeAllowance=2400;
    XCUIDevice.sharedDevice.orientation=UIDeviceOrientationLandscapeLeft;
    XCUIApplication *app=[[XCUIApplication alloc]initWithBundleIdentifier:bundle];
    NSMutableArray *args=[@[@"-Default Root",root,@"-LinuxRunnerDesktop.performanceDiagnostics",@"YES"] mutableCopy];
    if(env[@"LR_REFERENCE_SHARED_IMAGES"])[args addObjectsFromArray:@[@"-LinuxRunnerGUISharedImages",env[@"LR_REFERENCE_SHARED_IMAGES"]]];
    app.launchArguments=args;
    if([env[@"LR_REFERENCE_RESTART_AFTER_TEST"] isEqual:@"YES"]){
        // A forced x11vnc exit can retain unmarked SysV capture buffers. End
        // the entire owning app process even when an assertion fails, then
        // relaunch without the temporary capture override.
        [self addTeardownBlock:^{[app terminate];app.launchArguments=@[@"-Default Root",root];[app launch];}];
    }
    [app launch];
    // Account creation is left to the operator; this isolated regression root
    // uses its administrative shell and creates no test password/credential.
    XCUIElement *later=app.buttons[@"Later"];
    if([later waitForExistenceWithTimeout:8])[later tap];
    if(![env[@"LR_REFERENCE_SKIP_TERMINAL"] isEqual:@"YES"]){
    XCUIElement *terminal=app.buttons[@"Terminal"].firstMatch;
    XCTAssertTrue([terminal waitForExistenceWithTimeout:120],@"%@",app.debugDescription);[terminal tap];
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:20]);
    [app typeText:@"{ uname -m; whoami; cat /etc/os-release; pacman -Q glibc bash pacman; } > /home/mobile/Downloads/arch-reference-cli.txt 2>&1; cat /home/mobile/Downloads/arch-reference-cli.txt; printf 'LR ARCH %s\\n' \"$(uname -m)\"\n"];
    XCTAssertTrue([self waitForRenderedText:@"LR ARCH aarch64" app:app timeout:120]);
    XCTAttachment *cli=[XCTAttachment attachmentWithScreenshot:app.screenshot];cli.name=@"Arch reference terminal identity through native input";cli.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:cli];
    [app terminate];[app launch];
    if([later waitForExistenceWithTimeout:3])[later tap];
    }
    XCUIElement *open=app.buttons[@"Open Desktop"];
    XCTAssertTrue([open waitForExistenceWithTimeout:60]);[open tap];
    XCUIElement *surface=[[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    NSPredicate *ready=[NSPredicate predicateWithFormat:@"value CONTAINS 'Desktop ready'"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ready object:surface]] timeout:1800],XCTWaiterResultCompleted);
    XCTAttachment *desktop=[XCTAttachment attachmentWithScreenshot:app.screenshot];desktop.name=@"Arch supported default desktop on physical iPad";desktop.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:desktop];
    NSLog(@"ARCH_REFERENCE_DESKTOP %@ surface=%@",surface.value,NSStringFromCGRect(surface.frame));
    // Paired storage-policy runs hold interaction until the 30–60 second
    // native counter window is complete. The broker uses the matching delay.
    if([env[@"LR_REFERENCE_QUIET_WINDOW"] isEqual:@"YES"])[NSThread sleepForTimeInterval:70];
    if([env[@"LR_REFERENCE_GUI_INPUT"] isEqual:@"YES"]){
        // The host's explicit reference-test broker submits an xterm launch
        // through this desktop's existing managed application request queue.
        BOOL terminalReady=[self waitForRenderedText:@"LINUXRUNNER GUI READY" app:app timeout:90];
        XCTAttachment *window=[XCTAttachment attachmentWithScreenshot:app.screenshot];window.name=@"Arch reference xterm readiness";window.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:window];
        XCTAssertTrue(terminalReady);
        unsigned w=0,h=0;NSString *state=surface.value;
        NSRange range=[state rangeOfString:@"framebuffer "];
        XCTAssertNotEqual(range.location,NSNotFound);
        XCTAssertEqual(sscanf([state substringFromIndex:NSMaxRange(range)].UTF8String,"%ux%u",&w,&h),2);
        CGRect b=surface.frame;CGFloat scale=MIN(b.size.width/w,b.size.height/h);
        [[[surface coordinateWithNormalizedOffset:CGVectorMake(0,0)] coordinateWithOffset:CGVectorMake((b.size.width-w*scale)/2+160*scale,(b.size.height-h*scale)/2+130*scale)] tap];
        [app typeText:@"printf 'LR REF INPUT %s\\n' \"$(uname -m)\" | tee /home/mobile/Downloads/arch-reference-gui.txt\n"];
        XCTAssertTrue([self waitForRenderedText:@"LR REF INPUT aarch64" app:app timeout:60]);
        XCTAttachment *input=[XCTAttachment attachmentWithScreenshot:app.screenshot];input.name=@"Arch reference xterm through native RFB touch and text input";input.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:input];
        if([env[@"LR_REFERENCE_REPAINT"] isEqual:@"YES"]){
            [app typeText:@"i=0; while [ $i -lt 30 ]; do printf 'Rendering sample %02d: ABCDEFG abcdefg 0123456789\\n' \"$i\"; i=$((i+1)); sleep .1; done; printf 'LR REFERENCE REPAINT %s\\n' DONE\n"];
            XCTAssertTrue([self waitForRenderedText:@"LR REFERENCE REPAINT DONE" app:app timeout:60]);
            XCTAttachment *repaint=[XCTAttachment attachmentWithScreenshot:app.screenshot];repaint.name=@"Arch reference bounded repaint workload";repaint.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:repaint];
        }
    }
    XCTAssertEqual(app.state,XCUIApplicationStateRunningForeground);
}
@end
