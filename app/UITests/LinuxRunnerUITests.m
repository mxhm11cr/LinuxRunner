#import <XCTest/XCTest.h>
#import <Vision/Vision.h>
@interface LinuxRunnerUITests : XCTestCase
@end
@implementation LinuxRunnerUITests
- (void)testRealLinuxGUIAndCLIRegression {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [XCUIApplication new];
    app.launchArguments = @[@"-LinuxRunnerRuntimeAutoStart", @"NO", @"-Default Root", @"gui-test-fixture"];
    [app launch];
    XCTAssertTrue([app.staticTexts[@"LinuxRunner Device Diagnostics"] waitForExistenceWithTimeout:30]);
    [app.buttons[@"Start Linux Runtime"] tap];
    XCUIElement *details = app.staticTexts[@"Device diagnostic details"];
    NSPredicate *running = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Runtime state: Running"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc] initWithPredicate:running object:details]] timeout:60], XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];
    [app.buttons[@"Linux GUI Test"] tap];
    XCTAssertTrue([app.buttons[@"Install GUI Support"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"Install GUI Support"] tap];
    XCTAssertTrue([self waitForRenderedText:@"GUI_SUPPORT:" app:app]);
    [app.navigationBars.buttons[@"Done"] tap];
    [app.buttons[@"Run X11 Test"] tap];
    XCUIElement *status = app.staticTexts[@"GUI status"];
    NSPredicate *frame = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"touch/keyboard ready"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc] initWithPredicate:frame object:status]] timeout:60], XCTWaiterResultCompleted);
    XCTAttachment *first = [XCTAttachment attachmentWithScreenshot:app.screenshot]; first.name = @"Real Linux Xlib first pixel in iOS viewer"; first.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:first];
    XCUIElement *surface = [[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    if(!surface.exists)NSLog(@"GUI hierarchy: %@",app.debugDescription);
    XCTAssertTrue(surface.exists);
    CGRect bounds = surface.frame;
    CGFloat scale = MIN(bounds.size.width/640.0,bounds.size.height/480.0);
    CGFloat x = (bounds.size.width-640*scale)/2+100*scale;
    CGFloat y = (bounds.size.height-480*scale)/2+230*scale;
    [[surface coordinateWithNormalizedOffset:CGVectorMake(0,0)] coordinateWithOffset:CGVectorMake(x,y)].tap;
    [app.buttons[@"GUI Keyboard"] tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:10]);
    [app typeText:@"gui"];
    [app.buttons[@"GUI Keyboard"] tap];
    [NSThread sleepForTimeInterval:2];
    XCTAttachment *input = [XCTAttachment attachmentWithScreenshot:app.screenshot]; input.name = @"Xlib touch and software keyboard repaint"; input.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:input];
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;
    XCTAssertTrue([status.label containsString:@"touch/keyboard ready"]);
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationPortrait;
    [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome];
    [app activate];
    XCTAssertTrue([status.label containsString:@"touch/keyboard ready"]);
    [app.buttons[@"GUI Esc"] tap];
    NSPredicate *ended = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"CLI"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc] initWithPredicate:ended object:status]] timeout:20], XCTWaiterResultCompleted);
    [app.navigationBars.buttons[@"Done"] tap];
    [[app.buttons matchingIdentifier:@"Terminal"] matchingPredicate:[NSPredicate predicateWithFormat:@"enabled == YES"]].firstMatch.tap;
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);
    [app.webViews.firstMatch tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:20]);
    NSString *cliID = [@"gui-cli-" stringByAppendingString:NSUUID.UUID.UUIDString];
    [app typeText:[NSString stringWithFormat:@"d=$(ls -td /home/mobile/Downloads/.linuxrunner/gui/session-* | head -n 1); grep \'click=1 button=1\' $d/application.log; grep \'keyboard-count=3\' $d/application.log; uname -m; printf \'#include <stdio.h>\\nint main(void){puts(\"cli-after-gui\");return 0;}\\n\' > /tmp/%@.c; gcc /tmp/%@.c -O2 -o /tmp/%@; /tmp/%@ > /home/mobile/Downloads/%@.txt; cat /home/mobile/Downloads/%@.txt; readelf -l /tmp/%@ | grep interpreter\n",cliID,cliID,cliID,cliID,cliID,cliID,cliID]];
    XCTAssertTrue([self waitForRenderedText:@"click=1 button=1" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"keyboard-count=3" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"cli-after-gui" app:app]);
    XCTAttachment *cli = [XCTAttachment attachmentWithScreenshot:app.screenshot];cli.name = @"GCC dynamic ARM64 ELF after GUI exit";cli.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:cli];
}

- (void)tapLinuxX:(CGFloat)x y:(CGFloat)y app:(XCUIApplication *)app {
    XCUIElement *surface=[[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    XCTAssertTrue(surface.exists);CGRect b=surface.frame;CGFloat scale=MIN(b.size.width/640.0,b.size.height/480.0);
    [[[surface coordinateWithNormalizedOffset:CGVectorMake(0,0)] coordinateWithOffset:CGVectorMake((b.size.width-640*scale)/2+x*scale,(b.size.height-480*scale)/2+y*scale)] tap];
}
- (void)testQtAndXtermGenericInput {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO",@"-Default Root",@"qt-test-fixture"];
    [app launch];XCTAssertTrue([app.buttons[@"Start Linux Runtime"] waitForExistenceWithTimeout:30]);[app.buttons[@"Start Linux Runtime"] tap];
    XCUIElement *details=app.staticTexts[@"Device diagnostic details"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS 'Runtime state: Running'"] object:details]] timeout:60],XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];[app.buttons[@"Linux GUI Test"] tap];
    // Fixture was installed/guest-compiled by Install Qt Support acceptance.
    // Input/lifecycle checks reuse it instead of spending minutes recompiling.
    [app.buttons[@"Run Qt Test"] tap];
    XCUIElement *status=app.staticTexts[@"GUI status"];
    NSPredicate *ready=[NSPredicate predicateWithFormat:@"label CONTAINS 'touch/keyboard ready'"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ready object:status]] timeout:60],XCTWaiterResultCompleted);
    XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 QT APPLICATION" app:app timeout:30]);
    [NSThread sleepForTimeInterval:2];
    [self tapLinuxX:65 y:208 app:app];[self tapLinuxX:26 y:232 app:app];
    [self tapLinuxX:100 y:102 app:app];[app.buttons[@"GUI Keyboard"] tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:10]);
    [app typeText:@"Qt iOS27 AaZz09 !?[]"];
    [app.buttons[@"GUI Keyboard"] tap];[NSThread sleepForTimeInterval:1];
    XCTAttachment *qt=[XCTAttachment attachmentWithScreenshot:app.screenshot];qt.name=@"Real Linux Qt widgets and iOS software text";qt.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:qt];
    [self tapLinuxX:166 y:208 app:app];XCTAssertTrue([self waitForRenderedText:@"Linux Qt dialog" app:app timeout:30]);
    [NSThread sleepForTimeInterval:2];[self tapLinuxX:408 y:227 app:app];[NSThread sleepForTimeInterval:2];
    [self tapLinuxX:270 y:208 app:app];XCTAssertTrue([self waitForRenderedText:@"Child focus test" app:app timeout:30]);
    [NSThread sleepForTimeInterval:2];
    XCTAttachment *child=[XCTAttachment attachmentWithScreenshot:app.screenshot];child.name=@"Real Qt child window";child.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:child];
    [self tapLinuxX:300 y:285 app:app];[self tapLinuxX:570 y:208 app:app];
    NSPredicate *ended=[NSPredicate predicateWithFormat:@"label CONTAINS 'CLI'"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ended object:status]] timeout:20],XCTWaiterResultCompleted);
    [app.buttons[@"Run xterm"] tap];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ready object:status]] timeout:60],XCTWaiterResultCompleted);
    [NSThread sleepForTimeInterval:2];[self tapLinuxX:100 y:100 app:app];[app.buttons[@"GUI Keyboard"] tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:10]);
    [app typeText:@"printf 'ios-xterm-keyboard-ok\\n' > /home/mobile/Downloads/qt-xterm-proof.txt\n"];
    [NSThread sleepForTimeInterval:2];
    XCTAttachment *xt=[XCTAttachment attachmentWithScreenshot:app.screenshot];xt.name=@"Real xterm accepts normal iOS keyboard commands";xt.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:xt];
    [app typeText:@"exit\n"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:ended object:status]] timeout:20],XCTWaiterResultCompleted);
    [app.navigationBars.buttons[@"Done"] tap];
    [[app.buttons matchingIdentifier:@"Terminal"] matchingPredicate:[NSPredicate predicateWithFormat:@"enabled == YES"]].firstMatch.tap;
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    [app typeText:@"d=$(ls -td /home/mobile/Downloads/.linuxrunner/gui/session-* | head -n 2 | tail -n 1); grep 'QT_TEST: clicks=1' $d/application.log; cat /home/mobile/Downloads/qt-xterm-proof.txt; echo cli-after-qt > /home/mobile/Downloads/qt-cli-proof.txt\n"];
    [NSThread sleepForTimeInterval:2];[self addAttachment:[XCTAttachment attachmentWithScreenshot:app.screenshot]];
}

- (void)testRealLinuxBrowserAndCLI {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO",@"-Default Root",@"browser-test-fixture"];
    [app launch];XCTAssertTrue([app.buttons[@"Start Linux Runtime"] waitForExistenceWithTimeout:30]);[app.buttons[@"Start Linux Runtime"] tap];
    XCUIElement *details=app.staticTexts[@"Device diagnostic details"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS 'Runtime state: Running'"] object:details]] timeout:60],XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];[app.buttons[@"Linux GUI Test"] tap];[app.buttons[@"Run LinuxRunner Browser"] tap];
    XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 CHROMIUM" app:app timeout:60]);
    XCTAssertTrue([self waitForRenderedText:@"JavaScript 3 storage-ok" app:app timeout:60]);
    [self tapLinuxX:100 y:275 app:app];[app.buttons[@"GUI Keyboard"] tap];[app typeText:@"Browser iOS27 AaZz09 !?"];[app.buttons[@"GUI Keyboard"] tap];[self tapLinuxX:300 y:275 app:app];
    XCTAssertTrue([self waitForRenderedText:@"clicked 1 Browser iOS27 AaZz09 !?" app:app timeout:30]);
    XCTAttachment *local=[XCTAttachment attachmentWithScreenshot:app.screenshot];local.name=@"Real Linux Chromium HTML JavaScript form and native touch/keyboard";local.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:local];
    [self tapLinuxX:390 y:25 app:app];[app.buttons[@"GUI Keyboard"] tap];[app.buttons[@"GUI Ctrl"] tap];[app typeText:@"l"];[app.buttons[@"GUI Ctrl"] tap];[app typeText:@"https://example.com\n"];[app.buttons[@"GUI Keyboard"] tap];
    XCTAssertTrue([self waitForRenderedText:@"Example Domain" app:app timeout:60]);
    XCTAttachment *https=[XCTAttachment attachmentWithScreenshot:app.screenshot];https.name=@"Chromium Linux HTTPS in the native iOS Simulator viewer";https.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:https];
    [app.buttons[@"GUI Close"] tap];[app.navigationBars.buttons[@"Done"] tap];
    [[app.buttons matchingIdentifier:@"Terminal"] matchingPredicate:[NSPredicate predicateWithFormat:@"enabled == YES"]].firstMatch.tap;
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    [app typeText:@"echo cli-after-browser > /home/mobile/Downloads/browser-cli-proof.txt; cat /home/mobile/Downloads/browser-cli-proof.txt\n"];
    XCTAssertTrue([self waitForRenderedText:@"cli-after-browser" app:app timeout:30]);
}

- (void)testRealLinuxFirefoxAndCLI {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO",@"-Default Root",@"browser-test-fixture"];
    [app launch];XCTAssertTrue([app.buttons[@"Start Linux Runtime"] waitForExistenceWithTimeout:30]);[app.buttons[@"Start Linux Runtime"] tap];
    XCUIElement *details=app.staticTexts[@"Device diagnostic details"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS 'Runtime state: Running'"] object:details]] timeout:60],XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];[app.buttons[@"Linux GUI Test"] tap];[app.buttons[@"Run Firefox"] tap];
    XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 FIREFOX" app:app timeout:120]);
    XCTAssertTrue([self waitForRenderedText:@"JavaScript 3 storage-ok" app:app timeout:60]);
    [self tapLinuxX:100 y:294 app:app];[app.buttons[@"GUI Keyboard"] tap];[app typeText:@"Firefox iOS27 AaZz09 !?"];[app.buttons[@"GUI Keyboard"] tap];[self tapLinuxX:310 y:294 app:app];
    XCTAssertTrue([self waitForRenderedText:@"clicked 1 Firefox iOS27 AaZz09 !?" app:app timeout:60]);
    XCTAttachment *local=[XCTAttachment attachmentWithScreenshot:app.screenshot];local.name=@"Real Linux Firefox HTML JavaScript native touch and text";local.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:local];
    [self tapLinuxX:85 y:410 app:app];
    XCTAssertTrue([self waitForRenderedText:@"This domain is for use" app:app timeout:90]);
    XCTAttachment *https=[XCTAttachment attachmentWithScreenshot:app.screenshot];https.name=@"Real Linux Firefox HTTPS in native Simulator viewer";https.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:https];
    [app.buttons[@"GUI Close"] tap];[app.navigationBars.buttons[@"Done"] tap];
    [[app.buttons matchingIdentifier:@"Terminal"] matchingPredicate:[NSPredicate predicateWithFormat:@"enabled == YES"]].firstMatch.tap;
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);[app.webViews.firstMatch tap];
    [app typeText:@"echo cli-after-firefox > /home/mobile/Downloads/firefox-cli-proof.txt; cat /home/mobile/Downloads/firefox-cli-proof.txt\n"];
    XCTAssertTrue([self waitForRenderedText:@"cli-after-firefox" app:app timeout:30]);
}

// Run on a newly created Simulator with no imported/preinstalled test fixtures.
- (void)openFreshGUI:(XCUIApplication *)app {
    XCTAssertTrue([app.buttons[@"Start Linux Runtime"] waitForExistenceWithTimeout:30]);[app.buttons[@"Start Linux Runtime"] tap];
    XCUIElement *details=app.staticTexts[@"Device diagnostic details"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc]initWithPredicate:[NSPredicate predicateWithFormat:@"label CONTAINS 'Runtime state: Running'"] object:details]] timeout:60],XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];[app.buttons[@"Linux GUI Test"] tap];
}
- (void)firefoxURL:(NSString *)url app:(XCUIApplication *)app {
    [app.buttons[@"GUI Ctrl"] tap];[app.buttons[@"GUI Keyboard"] tap];[app typeText:@"l"];[app.buttons[@"GUI Ctrl"] tap];
    [app typeText:[url stringByAppendingString:@"\n"]];[app.buttons[@"GUI Keyboard"] tap];
}
- (void)testFreshInstallBrowsersAndPersistence {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO"];
    [app launch];[self openFreshGUI:app];
    [app.buttons[@"Install GUI Support"] tap];
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app timeout:600]);[app.navigationBars.buttons[@"Done"] tap];
    [app.buttons[@"Install Browser Support"] tap];
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app timeout:1800]);[app.navigationBars.buttons[@"Done"] tap];
    [app.buttons[@"Run Qt Test"] tap];XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 QT APPLICATION" app:app timeout:60]);[app.buttons[@"GUI Close"] tap];
    [self exerciseInstalledBrowsers:app];
}

- (void)exerciseInstalledBrowsers:(XCUIApplication *)app {
    for(int cycle=0;cycle<2;cycle++){
        if(cycle){[app terminate];[app launch];[self openFreshGUI:app];}
        [app.buttons[@"Run LinuxRunner Browser"] tap];
        XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 CHROMIUM" app:app timeout:120]);
        XCTAssertTrue([self waitForRenderedText:@"JavaScript 3 storage-ok" app:app timeout:60]);
        [self tapLinuxX:100 y:275 app:app];[app.buttons[@"GUI Keyboard"] tap];[app typeText:@"Browser iOS27 AaZz09 !?"];[app.buttons[@"GUI Keyboard"] tap];[self tapLinuxX:300 y:275 app:app];
        XCTAssertTrue([self waitForRenderedText:@"clicked 1 Browser iOS27 AaZz09 !?" app:app timeout:60]);
        [app.buttons[@"GUI Close"] tap];[app.buttons[@"Run Firefox"] tap];
        XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 FIREFOX" app:app timeout:600]);
        XCTAssertTrue([self waitForRenderedText:@"JavaScript 3 storage-ok" app:app timeout:60]);
        [self tapLinuxX:100 y:294 app:app];[app.buttons[@"GUI Keyboard"] tap];[app typeText:@"Firefox iOS27 AaZz09 !?"];[app.buttons[@"GUI Keyboard"] tap];[self tapLinuxX:310 y:294 app:app];
        XCTAssertTrue([self waitForRenderedText:@"clicked 1 Firefox iOS27 AaZz09 !?" app:app timeout:60]);
        [self firefoxURL:@"https://etsbi.edu.ba/" app:app];
        XCTAssertTrue([self waitForRenderedText:@"ELEKTROTEHNI" app:app timeout:300]);
        XCTAttachment *web=[XCTAttachment attachmentWithScreenshot:app.screenshot];web.name=[NSString stringWithFormat:@"Actual Linux Firefox etsbi.edu.ba cycle %d",cycle];web.lifetime=XCTAttachmentLifetimeKeepAlways;[self addAttachment:web];
        [self firefoxURL:@"https://example.com/" app:app];XCTAssertTrue([self waitForRenderedText:@"This domain is for use" app:app timeout:180]);
        [app.buttons[@"GUI Close"] tap];
    }
}
- (void)testInstalledBrowsersAndUpdatePersistence {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO"];
    [app launch];[self openFreshGUI:app];
    [app.buttons[@"Run Qt Test"] tap];XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 QT APPLICATION" app:app timeout:90]);[app.buttons[@"GUI Close"] tap];
    [self exerciseInstalledBrowsers:app];
}

// Reuses only the environment built by the fresh native UI installers.
- (void)testCleanInstalledFixturePersistence {
    self.continueAfterFailure=NO;XCUIApplication *app=[XCUIApplication new];
    app.launchArguments=@[@"-LinuxRunnerRuntimeAutoStart",@"NO",@"-Default Root",@"clean-833-native-resume"];
    [app launch];[self openFreshGUI:app];
    [app.buttons[@"Run Qt Test"] tap];XCTAssertTrue([self waitForRenderedText:@"REAL LINUX ARM64 QT APPLICATION" app:app timeout:90]);[app.buttons[@"GUI Close"] tap];
    [self exerciseInstalledBrowsers:app];
}

- (void)testNativeDiagnosticsAndManualRuntimeStart {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [XCUIApplication new];
    app.launchArguments = @[@"-LinuxRunnerRuntimeAutoStart", @"NO"];
    [app launch];
    XCTAssertTrue([app.staticTexts[@"LinuxRunner Device Diagnostics"] waitForExistenceWithTimeout:30]);
    XCUIElement *details = app.staticTexts[@"Device diagnostic details"];
    XCTAssertTrue([details.label containsString:@"Runtime state: Not started"]);
    XCTAssertTrue([details.label containsString:@"Bundled rootfs exists: yes"]);
    [self addAttachment:[XCTAttachment attachmentWithScreenshot:app.screenshot]];
    [app.buttons[@"Export Logs"] tap];
    XCTAssertTrue([app.collectionViews[@"activityCollectionView"] waitForExistenceWithTimeout:10]);
    [app.otherElements[@"PopoverDismissRegion"] tap];
    [app.buttons[@"Start Linux Runtime"] tap];
    NSPredicate *running = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Runtime state: Running"];
    XCTNSPredicateExpectation *started = [[XCTNSPredicateExpectation alloc] initWithPredicate:running object:details];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[started] timeout:60], XCTWaiterResultCompleted);
    [app.buttons[@"Open Environment Manager"] tap];
    XCTAssertTrue([app.buttons[@"Terminal"].firstMatch waitForExistenceWithTimeout:30]);
    [app.buttons[@"Terminal"].firstMatch tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    // Tap the terminal surface as a user would after switching native roots.
    XCTAssertTrue([app.webViews.firstMatch waitForExistenceWithTimeout:30]);
    [app.webViews.firstMatch tap];
    XCTAssertTrue([app.keyboards.firstMatch waitForExistenceWithTimeout:30]);
    NSString *name = [NSString stringWithFormat:@"diagnostic-%@.txt", NSUUID.UUID.UUIDString];
    [app typeText:[NSString stringWithFormat:@"uname -m; echo device-debug > /home/mobile/Downloads/%@\n", name]];
    [NSThread sleepForTimeInterval:3];
    [self addAttachment:[XCTAttachment attachmentWithScreenshot:app.screenshot]];
    [app.buttons[@"Done"] tap];
    [app.buttons[@"Files"].firstMatch tap];
    XCTAssertTrue([app.buttons[name] waitForExistenceWithTimeout:10]);
    [app terminate];
    [app launch];
    XCTAssertTrue([app.staticTexts[@"LinuxRunner Device Diagnostics"] waitForExistenceWithTimeout:30]);
    XCTAssertTrue([app.staticTexts[@"Device diagnostic details"].label containsString:@"Runtime state: Not started"]);
}

- (BOOL)waitForRenderedText:(NSString *)expected app:(XCUIApplication *)app {return [self waitForRenderedText:expected app:app timeout:30];}
- (BOOL)waitForRenderedText:(NSString *)expected app:(XCUIApplication *)app timeout:(NSTimeInterval)timeout {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    NSString *last = @"";
    NSString *needle = [[expected componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] componentsJoinedByString:@""].lowercaseString;
    BOOL dismissedFirefoxNotice=NO;
    do {
        VNRecognizeTextRequest *request = [VNRecognizeTextRequest new];
        request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
        request.recognitionLanguages = @[@"en-US"];
        request.usesLanguageCorrection = NO;
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:app.screenshot.image.CGImage options:@{}];
        NSError *error = nil;
        if (![handler performRequests:@[request] error:&error]) { NSLog(@"Terminal OCR: %@", error); return NO; }
        NSMutableString *text = [NSMutableString new];
        for (VNRecognizedTextObservation *line in request.results)
            [text appendFormat:@"%@\n", [line topCandidates:1].firstObject.string ?: @""];
        last = text;
        if (!dismissedFirefoxNotice && [expected containsString:@"FIREFOX"] &&
            ([text containsString:@"Firefox automatically sends"] || [text containsString:@"Choose What I Share"])) {
            [self tapLinuxX:552 y:108 app:app];dismissedFirefoxNotice=YES;
            [NSThread sleepForTimeInterval:2];continue;
        }
        NSString *normalized = [[text componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] componentsJoinedByString:@""].lowercaseString;
        // Monospace zero is occasionally recognized as O by Vision. Limit
        // this normalization to the known numeric exit-status field.
        if ([needle isEqualToString:@"exitstatus:0"])
            normalized = [normalized stringByReplacingOccurrencesOfString:@"exitstatus:o" withString:@"exitstatus:0"];
        // Vision reads the leading Q as O at this fitted display size. Only
        // normalize the exact Qt fixture heading; version/QPA are also shown.
        if ([needle isEqualToString:@"reallinuxarm64qtapplication"])
            normalized = [normalized stringByReplacingOccurrencesOfString:@"reallinuxarm64otapplication" withString:needle];
        if ([normalized containsString:needle]) return YES;
        [NSThread sleepForTimeInterval:1];
    } while ([deadline timeIntervalSinceNow] > 0);
    NSLog(@"Last rendered terminal text: %@", last);
    return NO;
}
- (void)testTerminalFilesLauncherAndLifecycle {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    [app launch];
    XCTAssertTrue([app.navigationBars[@"LinuxRunner"] waitForExistenceWithTimeout:30]);
    [app.buttons[@"Terminal"].firstMatch tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    NSString *file = [NSString stringWithFormat:@"terminal-%@.txt", NSUUID.UUID.UUIDString];
    // Exercise the existing UITextInput -> PTY -> shell path.
    [app typeText:[NSString stringWithFormat:@"uname -m; echo terminal-smoke; ls /; pwd; mkdir -p /tmp/lr-ui; touch /tmp/lr-ui/a; echo verified > /tmp/lr-ui/a; cp /tmp/lr-ui/a /tmp/lr-ui/b; mv /tmp/lr-ui/b /tmp/lr-ui/c; cat /tmp/lr-ui/c > /home/mobile/Downloads/%@; rm /tmp/lr-ui/a /tmp/lr-ui/c; sh -c 'echo shell-ok'\n", file]];
    [NSThread sleepForTimeInterval:3];
    XCTAttachment *terminal = [XCTAttachment attachmentWithScreenshot:app.screenshot];
    terminal.name = @"Interactive ARM64 terminal"; terminal.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:terminal];
    [app.buttons[@"Done"] tap];
    [app.buttons[@"Files"].firstMatch tap];
    XCTAssertTrue([app.buttons[file] waitForExistenceWithTimeout:10]);
    [app.buttons[file] tap];
    XCTAssertTrue([app.collectionViews[@"activityCollectionView"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue(app.cells[@"Copy"].exists);
    // Close the share controller without sending data anywhere.
    [app.buttons[@"header.closeButton"] tap];
    XCTAssertTrue([app.buttons[@"Import File"] waitForExistenceWithTimeout:10]);
    [[app.navigationBars.buttons elementBoundByIndex:0] tap];
    [app.buttons[@"Open"].firstMatch tap];
    [app.buttons[@"run-System information"] tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app]);
    [app.buttons[@"Done"] tap];
    [app.buttons[@"Manage Alpine Packages"] tap];
    [app.buttons[@"Installed Packages"] tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self waitForRenderedText:@"alpine-baselayout" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"libcrypto3" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app]);
    XCTAttachment *packages = [XCTAttachment attachmentWithScreenshot:app.screenshot];
    packages.name = @"Guest apk info"; packages.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:packages];
    [app.buttons[@"Done"] tap];
    [app.navigationBars.buttons.firstMatch tap];
    [app.navigationBars.buttons.firstMatch tap];
    [app.buttons[@"Diagnostics"] tap];
    XCTAssertTrue([app.staticTexts[@"Guest: ARM64 · Interpreter Active"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"Export Diagnostics"] tap];
    XCTAssertTrue([app.collectionViews[@"activityCollectionView"] waitForExistenceWithTimeout:10]);
    [app.buttons[@"header.closeButton"] tap];
    [XCUIDevice.sharedDevice pressButton:XCUIDeviceButtonHome];
    [app activate];
    XCTAssertTrue([app.navigationBars[@"LinuxRunner"] waitForExistenceWithTimeout:10]);
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationLandscapeLeft;
    [app.buttons[@"Terminal"].firstMatch tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    [self addAttachment:[XCTAttachment attachmentWithScreenshot:app.screenshot]];
    XCUIDevice.sharedDevice.orientation = UIDeviceOrientationPortrait;
    [app.buttons[@"Done"] tap];
    [app terminate];
    [app launch];
    [app.buttons[@"Files"].firstMatch tap];
    XCTAssertTrue([app.buttons[file] waitForExistenceWithTimeout:10]);
}
- (void)testVerifiedEnvironmentCreationAndSelection {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    [app launch];
    XCTAssertTrue([app.buttons[@"New Environment"] waitForExistenceWithTimeout:30]);
    [app.buttons[@"New Environment"] tap];
    XCUIElement *name = app.textFields[@"Environment name"];
    [name tap];
    NSMutableString *erase = [NSMutableString new];
    for (NSUInteger i = 0; i < [name.value length]; i++) [erase appendString:XCUIKeyboardKeyDelete];
    [name typeText:erase];
    NSString *title = [@"Validation Alpine " stringByAppendingString:NSUUID.UUID.UUIDString];
    [name typeText:title];
    [app.buttons[@"Create Environment"] tap];
    XCTAssertTrue([app.staticTexts[title] waitForExistenceWithTimeout:60]);
    [[app.buttons matchingIdentifier:@"Files"].allElementsBoundByIndex.lastObject tap];
    XCTAssertTrue([app.staticTexts[@"No files in the shared folder."] waitForExistenceWithTimeout:10]);
    [app.navigationBars.buttons.firstMatch tap];
    [[app.buttons matchingIdentifier:@"Open"].allElementsBoundByIndex.lastObject tap];
    [app.buttons[@"Use on Next Launch"] tap];
    [app.alerts.buttons[@"OK"] tap];
    [app terminate];
    [app launch];
    XCTAssertTrue([app.staticTexts[title] waitForExistenceWithTimeout:30]);
    [[app.buttons matchingIdentifier:@"Open"].allElementsBoundByIndex.lastObject tap];
    XCTAssertFalse(app.buttons[@"Use on Next Launch"].exists);
    [app.buttons[@"run-System information"] tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app]);
    [self addAttachment:[XCTAttachment attachmentWithScreenshot:app.screenshot]];
    [app.buttons[@"Done"] tap];
    [app.navigationBars.buttons.firstMatch tap];
    // Restore the original root for repeatable subsequent test runs.
    [app.buttons[@"Open"].firstMatch tap];
    [app.buttons[@"Use on Next Launch"] tap];
    [app.alerts.buttons[@"OK"] tap];
    [app terminate];
    [app launch];
    XCTAssertTrue([app.navigationBars[@"LinuxRunner"] waitForExistenceWithTimeout:30]);
}
- (void)testNativeDocumentImport {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    [app launch];
    XCTAssertTrue([app.buttons[@"Terminal"].firstMatch waitForExistenceWithTimeout:30]);
    [app.buttons[@"Terminal"].firstMatch tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    [app typeText:@"rm -f /home/mobile/Downloads/native-import.txt\n"];
    [NSThread sleepForTimeInterval:1];
    [app.buttons[@"Done"] tap];
    [app.buttons[@"Import File"] tap];
    XCTAssertTrue([app.navigationBars[@"FullDocumentManagerViewControllerNavigationBar"] waitForExistenceWithTimeout:60]);
    [[app.tabBars[@"DOC.browsingModeTabBar"].buttons elementBoundByIndex:2] tap];
    XCUIElement *local = [app.cells matchingPredicate:[NSPredicate predicateWithFormat:
        @"identifier BEGINSWITH 'DOC.sidebar.item.' AND (label CONTAINS 'iPhone' OR label CONTAINS 'iPad')"]].firstMatch;
    if ([local waitForExistenceWithTimeout:3]) [local tap];
    XCUIElement *container = app.cells[@"LinuxRunner, Container"];
    if ([container waitForExistenceWithTimeout:10]) [container tap];
    XCUIElement *fixture = [app.cells matchingPredicate:[NSPredicate predicateWithFormat:
        @"identifier BEGINSWITH %@ OR label BEGINSWITH %@", @"native-import", @"native-import"]].firstMatch;
    // Seed this known fixture in the app's Documents using the documented
    // simctl setup. Provider navigation is tested separately on each OS/device.
    XCTAssertTrue([fixture waitForExistenceWithTimeout:20], @"Seed native-import.txt in the app Documents before this test.");
    [fixture tap];
    XCTAssertTrue([app.navigationBars[@"LinuxRunner"] waitForExistenceWithTimeout:20]);
    [app.buttons[@"Files"].firstMatch tap];
    XCTAssertTrue([app.buttons[@"native-import.txt"] waitForExistenceWithTimeout:20]);
    XCTAttachment *imported = [XCTAttachment attachmentWithScreenshot:app.screenshot];
    imported.name = @"Imported document";
    imported.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:imported];
}
- (void)testLauncherRenderedOutput {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [[XCUIApplication alloc] init];
    [app launch];
    XCTAssertTrue([app.buttons[@"Open"].firstMatch waitForExistenceWithTimeout:30]);
    [app.buttons[@"Open"].firstMatch tap];
    [app.buttons[@"run-System information"] tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app]);
    [app.buttons[@"Done"] tap];
    [app.buttons[@"Manage Alpine Packages"] tap];
    [app.buttons[@"Installed Packages"] tap];
    XCTAssertTrue([app.buttons[@"Done"] waitForExistenceWithTimeout:10]);
    XCTAssertTrue([self waitForRenderedText:@"alpine-baselayout" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"libcrypto3" app:app]);
    XCTAssertTrue([self waitForRenderedText:@"Exit status: 0" app:app]);
    XCTAttachment *image = [XCTAttachment attachmentWithScreenshot:app.screenshot];
    image.name = @"Rendered guest apk output and exit status"; image.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:image];
}

// Real UIKit touches on the Linux desktop surface (iOS Simulator): the Open Desktop
// button, then click, double-click, press-and-drag and hold in the selected mouse
// mode (LR_TEST_MOUSE_MODE: pointer | touch). The app exports its Open Desktop
// report 100 s after launch (input sources, pointer events, latencies).
- (CGPoint)surfacePoint:(XCUIElement *)surface x:(CGFloat)px y:(CGFloat)py {
    CGRect b = surface.frame; CGFloat scale = MIN(b.size.width / 960.0, b.size.height / 540.0);
    return CGPointMake((b.size.width - 960 * scale) / 2 + px * scale, (b.size.height - 540 * scale) / 2 + py * scale);
}
- (XCUICoordinate *)at:(XCUIElement *)surface x:(CGFloat)px y:(CGFloat)py {
    CGPoint p = [self surfacePoint:surface x:px y:py];
    return [[surface coordinateWithNormalizedOffset:CGVectorMake(0, 0)] coordinateWithOffset:CGVectorMake(p.x, p.y)];
}
- (void)shot:(XCUIApplication *)app name:(NSString *)name {
    XCTAttachment *a = [XCTAttachment attachmentWithScreenshot:app.screenshot]; a.name = name; a.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:a];
}
- (void)testDesktopPointerInput {
    self.continueAfterFailure = NO;
    NSString *mode = NSProcessInfo.processInfo.environment[@"LR_TEST_MOUSE_MODE"] ?: @"pointer";
    XCUIApplication *app = [XCUIApplication new];
    app.launchArguments = @[@"-LinuxRunnerInput.mouseMode", mode];
    app.launchEnvironment = @{@"LR_SIM_SCRIPT": @"wait:100;report"};
    [app launch];
    XCUIElement *open = app.buttons[@"Open Desktop"];
    XCTAssertTrue([open waitForExistenceWithTimeout:60]);
    [open tap];
    XCUIElement *status = app.staticTexts[@"GUI status"];
    NSPredicate *ready = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Desktop ready"];
    XCTAssertEqual([XCTWaiter waitForExpectations:@[[[XCTNSPredicateExpectation alloc] initWithPredicate:ready object:status]] timeout:180], XCTWaiterResultCompleted);
    XCUIElement *surface = [[app descendantsMatchingType:XCUIElementTypeAny] matchingIdentifier:@"Linux GUI surface"].firstMatch;
    XCTAssertTrue(surface.exists);
    [NSThread sleepForTimeInterval:4];
    [self shot:app name:[mode stringByAppendingString:@" 1 desktop"]];
    [[self at:surface x:55 y:528] tap];                       // Applications menu (panel, bottom left)
    [NSThread sleepForTimeInterval:3];
    [self shot:app name:[mode stringByAppendingString:@" 2 applications menu after click"]];
    [[self at:surface x:700 y:150] tap];                      // close the menu
    [NSThread sleepForTimeInterval:2];
    [[self at:surface x:480 y:250] doubleTap];                // double-click on the desktop
    [NSThread sleepForTimeInterval:2];
    [[self at:surface x:300 y:150] pressForDuration:0.4 thenDragToCoordinate:[self at:surface x:620 y:380]];   // rubber-band drag
    [NSThread sleepForTimeInterval:2];
    [self shot:app name:[mode stringByAppendingString:@" 3 after drag"]];
    [[self at:surface x:480 y:250] pressForDuration:1.2];    // hold (touch mode: right-click menu)
    [NSThread sleepForTimeInterval:3];
    [self shot:app name:[mode stringByAppendingString:@" 4 after hold"]];
    for (int i = 0; i < 6; i++) { [[self at:surface x:420 + i * 10 y:200] tap]; [NSThread sleepForTimeInterval:0.6]; }
    [NSThread sleepForTimeInterval:60];                       // the app writes its report at ~100 s
}

// iOS Files → document picker → security-scoped bookmark → /mnt/<name> in Linux.
// Needs a folder "LRShare" (with at least one file) in On My iPhone. Runs the
// app's Check Access (Linux list/read/create/modify/rename/mkdir/delete as the
// default user, then the Linux-made file must be visible to iOS), then relaunches
// the app and checks again (bookmark restore).
- (BOOL)tapFirst:(NSArray<XCUIElement *> *)candidates timeout:(NSTimeInterval)timeout {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while ([end timeIntervalSinceNow] > 0) {
        for (XCUIElement *e in candidates) if (e.exists && e.isHittable) { [e tap]; return YES; }
        [NSThread sleepForTimeInterval:0.5];
    }
    return NO;
}
- (void)openSharedFolders:(XCUIApplication *)app {
    XCTAssertTrue([app.tabBars.buttons[@"Settings"] waitForExistenceWithTimeout:60]);
    [app.tabBars.buttons[@"Settings"] tap];
    XCUIElement *row = app.buttons[@"Shared iPhone Folders"];
    for (int i = 0; i < 8 && !(row.exists && row.isHittable); i++) [app swipeUp];
    XCTAssertTrue(row.exists); [row tap];
    XCTAssertTrue([app.navigationBars[@"Shared iPhone Folders"] waitForExistenceWithTimeout:10]);
}
- (NSString *)checkAccess:(XCUIApplication *)app label:(NSString *)label {
    XCUIElement *check = app.buttons[@"Check Access"].firstMatch;
    XCTAssertTrue([check waitForExistenceWithTimeout:20]); [check tap];
    NSPredicate *done = [NSPredicate predicateWithFormat:@"label CONTAINS %@", @"Linux:"];
    XCUIElement *result = [app.staticTexts matchingPredicate:done].firstMatch;
    XCTAssertTrue([result waitForExistenceWithTimeout:120]);
    [NSThread sleepForTimeInterval:1];
    NSString *text = [app.staticTexts matchingPredicate:done].firstMatch.label;
    NSLog(@"SHARE_CHECK %@: %@", label, text);
    XCTAttachment *a = [XCTAttachment attachmentWithString:text]; a.name = [@"check access " stringByAppendingString:label]; a.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:a];
    [self shot:app name:[@"shared folder " stringByAppendingString:label]];
    return text;
}
- (void)testSharedFolderFromFiles {
    self.continueAfterFailure = NO;
    XCUIApplication *app = [XCUIApplication new];
    app.launchArguments = @[@"-AppleLanguages", @"(en)", @"-AppleLocale", @"en_US"];
    [app launch];
    [self openSharedFolders:app];
    if (![app.staticTexts[@"/mnt/LRShare"] exists]) {
        [app.buttons[@"Add Folder…"] tap];
        [NSThread sleepForTimeInterval:3];
        [self shot:app name:@"picker opened"];
        // The picker runs in the system's language (this simulator: Croatian).
        [self tapFirst:@[app.buttons[@"Browse"], app.buttons[@"Pretraga"]] timeout:8];
        [NSThread sleepForTimeInterval:2];
        if (![self tapFirst:@[app.cells[@"On My iPhone"], app.buttons[@"On My iPhone"], app.staticTexts[@"On My iPhone"], app.cells[@"Na mom iPhoneu"], app.buttons[@"Na mom iPhoneu"], app.staticTexts[@"Na mom iPhoneu"]] timeout:10]) NSLog(@"PICKER_TREE %@", app.debugDescription);
        [NSThread sleepForTimeInterval:2];
        if (![self tapFirst:@[app.cells[@"LRShare"], app.buttons[@"LRShare"], app.staticTexts[@"LRShare"], app.images[@"LRShare"]] timeout:10]) NSLog(@"PICKER_TREE %@", app.debugDescription);
        [NSThread sleepForTimeInterval:2];
        [self shot:app name:@"picker in LRShare"];
        if (![self tapFirst:@[app.buttons[@"Open"], app.buttons[@"Otvori"], app.buttons[@"Done"]] timeout:10]) NSLog(@"PICKER_TREE %@", app.debugDescription);
        NSPredicate *shared = [NSPredicate predicateWithFormat:@"label BEGINSWITH %@", @"Shared at"];
        XCTAssertTrue([[app.staticTexts matchingPredicate:shared].firstMatch waitForExistenceWithTimeout:20]);
    }
    NSString *first = [self checkAccess:app label:@"after adding"];
    XCTAssertTrue([first containsString:@"rename=ok"] && [first containsString:@"visible in iOS=yes"], @"%@", first);
    [app terminate];
    [app launch];
    [self openSharedFolders:app];
    NSString *again = [self checkAccess:app label:@"after relaunch"];
    XCTAssertTrue([again containsString:@"rename=ok"] && [again containsString:@"visible in iOS=yes"], @"%@", again);
}
// Release screenshots (real app UI in the iOS Simulator): tabs, Settings,
// Desktop Environment, a terminal running real commands, Open Desktop.
- (void)releaseShot:(XCUIApplication *)app name:(NSString *)name {
    XCTAttachment *a = [XCTAttachment attachmentWithScreenshot:app.screenshot]; a.name = name; a.lifetime = XCTAttachmentLifetimeKeepAlways; [self addAttachment:a];
}
- (void)testReleaseScreenshots {
    self.continueAfterFailure = YES;
    XCUIApplication *app = [XCUIApplication new];
    NSString *root = NSProcessInfo.processInfo.environment[@"LR_SCREENSHOT_ROOT"] ?: @"sim-debian-1";
    app.launchArguments = @[@"-Default Root", root];   // argument domain: overrides the stored environment choice
    [app launch];
    XCTAssertTrue([app.tabBars.buttons[@"Home"] waitForExistenceWithTimeout:30]);
    [NSThread sleepForTimeInterval:8];
    [self releaseShot:app name:@"linuxrunner-home"];
    [app.tabBars.buttons[@"Distributions"] tap]; [NSThread sleepForTimeInterval:2]; [self releaseShot:app name:@"linuxrunner-distributions"];
    [app.tabBars.buttons[@"Settings"] tap]; [NSThread sleepForTimeInterval:2]; [self releaseShot:app name:@"linuxrunner-settings-1"];
    [app swipeUp]; [NSThread sleepForTimeInterval:1]; [self releaseShot:app name:@"linuxrunner-settings-2"];
    [app swipeUp]; [NSThread sleepForTimeInterval:1]; [self releaseShot:app name:@"linuxrunner-settings-3"];
    [app swipeDown]; [app swipeDown]; [NSThread sleepForTimeInterval:1];
    XCUIElement *desktopEnvironment = app.buttons[@"Desktop Environment"];
    if ([desktopEnvironment waitForExistenceWithTimeout:5]) { [desktopEnvironment tap]; [NSThread sleepForTimeInterval:2]; [self releaseShot:app name:@"linuxrunner-desktop-environment"]; [app.navigationBars.buttons.firstMatch tap]; }
    [app.tabBars.buttons[@"Sessions"] tap]; [NSThread sleepForTimeInterval:2];
    XCUIElement *terminal = app.buttons[@"Open Terminal"];
    if ([terminal waitForExistenceWithTimeout:10]) {
        [terminal tap]; [NSThread sleepForTimeInterval:10];
        [app typeText:@"uname -sm; . /etc/os-release; echo $PRETTY_NAME; whoami\n"]; [NSThread sleepForTimeInterval:8];
        [self releaseShot:app name:@"linuxrunner-terminal"];
        XCUIElement *done = app.buttons[@"Done"]; if ([done waitForExistenceWithTimeout:3]) [done tap]; else [app.navigationBars.buttons.firstMatch tap];
        [NSThread sleepForTimeInterval:2];
    }
    [app.tabBars.buttons[@"Home"] tap]; [NSThread sleepForTimeInterval:1];
    XCUIElement *open = app.buttons[@"Open Desktop"];
    if ([open waitForExistenceWithTimeout:10]) { [open tap]; [NSThread sleepForTimeInterval:90]; [self releaseShot:app name:@"linuxrunner-desktop"]; }
}
@end
