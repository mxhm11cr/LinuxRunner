// Included only in Simulator builds, next to the production controller. This
// tests UIKit/CG rendering and queued-update cancellation, not monitor hardware.
@interface LRGUIViewController (LRNotificationThreadContract)
- (void)externalDisplayChanged:(NSNotification *)notification;
- (void)detachViewer;
- (BOOL)externalOutputActive;
- (BOOL)externalDisplayAvailable;
- (BOOL)trackpadReady;
- (void)resumeRequestedTrackpad;
- (UIAlertController *)desktopOptionsMenu;
@end
@interface LRThreadContractDesktop : LRGUIViewController
@property BOOL detached;
@property BOOL detachedOnMainThread;
@end
@implementation LRThreadContractDesktop
- (void)detachViewer{self.detached=YES;self.detachedOnMainThread=NSThread.isMainThread;}
@end
static NSData *LRTestImageBytes(LRExternalDisplayController *view) {
    CGImageRef image=view.image.image.CGImage;if(!image)return nil;
    return CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(image)));
}
// Reproduce the old scheduling policy on the same controller and CG renderer.
// These measurements exclude AirPlay transport, scanout, and physical monitors.
@interface LRFixedDelayDisplayContract : LRExternalDisplayController
@end
@implementation LRFixedDelayDisplayContract
- (double)presentationDelayAt:(double)now{(void)now;return 1.0/30.0;}
@end
static void LRWaitForPresentation(LRExternalDisplayController *view,double deadline,void (^done)(BOOL)) {
    if(view.image.image||LRGUIClock()>=deadline){done(view.image.image!=nil);return;}
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC),dispatch_get_main_queue(),^{LRWaitForPresentation(view,deadline,done);});
}
static void LRMeasureExternalPacing(unsigned sample,NSMutableArray *before,NSMutableArray *after,void (^done)(NSDictionary *)) {
    if(sample==10){done(@{@"environment":@"Simulator CG component, controlled old-delay comparison; no hardware latency claim",@"fixed_delay_first_frame_ms":before,@"paced_first_frame_ms":after});return;}
    LRExternalDisplayController *view=sample%2?[LRExternalDisplayController new]:[LRFixedDelayDisplayContract new];
    [view loadViewIfNeeded];[view.metal removeFromSuperview];view.metal=nil;
    NSData *pixels=[NSData dataWithBytes:(uint32_t[1]){0xff123456} length:4];
    double start=LRGUIClock();[view presentFrame:pixels width:1 height:1 damage:(LRFrameDamage){0,0,1,1}];
    LRWaitForPresentation(view,start+1,^(BOOL shown){
        [(sample%2?after:before) addObject:shown?@((LRGUIClock()-start)*1000):@(-1)];
        LRMeasureExternalPacing(sample+1,before,after,done);
    });
}
static NSDictionary *LRMeasureHiddenFramePresentation(void) {
    LRFramebufferView *surface=[LRFramebufferView new];[surface.metal removeFromSuperview];surface.metal=nil;
    NSMutableData *pixels=[NSMutableData dataWithLength:960*540*4];
    NSMutableArray *enabled=[NSMutableArray new],*suppressed=[NSMutableArray new];
    for(unsigned sample=0;sample<5;sample++)for(unsigned mode=0;mode<2;mode++){
        surface.presentationSuppressed=mode;surface.image.image=nil;
        double start=NSProcessInfo.processInfo.systemUptime;
        for(unsigned frame=0;frame<64;frame++){@autoreleasepool{
            ((uint8_t *)pixels.mutableBytes)[0]=frame;
            [surface presentPixels:pixels width:960 height:540];
        }}
        [(mode?suppressed:enabled) addObject:@((NSProcessInfo.processInfo.systemUptime-start)*1000)];
    }
    BOOL noImage=surface.image.image==nil;surface.presentationSuppressed=NO;
    [surface presentPixels:surface.latestPixels width:960 height:540];
    NSData *rendered=CFBridgingRelease(CGDataProviderCopyData(CGImageGetDataProvider(surface.image.image.CGImage)));
    return @{@"environment":@"iOS Simulator component CPU, forced Core Graphics fallback; no device FPS claim",@"frames_per_sample":@64,@"geometry":@"960x540",@"presentation_enabled_cpu_ms":enabled,@"hidden_presentation_suppressed_cpu_ms":suppressed,@"no_hidden_image":@(noImage),@"resume_latest_pixels":@([rendered isEqual:pixels])};
}
@interface LRInputRecordingClient : LRRFBClient
@property NSMutableArray *events;
@end
@implementation LRInputRecordingClient
- (void)pointerX:(uint16_t)x y:(uint16_t)y buttons:(uint8_t)b { [self.events addObject:@[@(x),@(y),@(b)]]; }
- (void)key:(uint32_t)symbol down:(BOOL)down { [self.events addObject:@[@(symbol),@(down)]]; }
- (void)typeKeysyms:(const uint32_t *)symbols count:(size_t)count { for(size_t i=0;i<count;i++){[self key:symbols[i] down:YES];[self key:symbols[i] down:NO];} }
@end
// Synthetic UIKit press objects exercise the production HID/layout mapper;
// they do not substitute for Magic Keyboard/Bluetooth/USB hardware testing.
@interface LRKeyboardContractKey : UIKey
@property UIKeyboardHIDUsage usage;
@property UIKeyModifierFlags flags;
@property NSString *text;
@end
@implementation LRKeyboardContractKey
- (UIKeyboardHIDUsage)keyCode{return _usage;}
- (UIKeyModifierFlags)modifierFlags{return _flags;}
- (NSString *)characters{return _text;}
- (NSString *)charactersIgnoringModifiers{return _text;}
@end
@interface LRKeyboardContractPress : UIPress
@property LRKeyboardContractKey *testKey;
@end
@implementation LRKeyboardContractPress
- (UIKey *)key{return _testKey;}
@end
static LRKeyboardContractPress *LRKeyboardPress(UIKeyboardHIDUsage usage,NSString *text,UIKeyModifierFlags flags){
    LRKeyboardContractPress *press=[LRKeyboardContractPress new];press.testKey=[LRKeyboardContractKey new];
    press.testKey.usage=usage;press.testKey.text=text;press.testKey.flags=flags;return press;
}
static NSDictionary *LRHardwareKeyboardContract(void){
    LRFramebufferView *surface=[LRFramebufferView new];
    LRInputRecordingClient *client=[[LRInputRecordingClient alloc]initWithPort:0 password:@""];client.events=[NSMutableArray new];surface.client=client;
    BOOL noAutomaticSoftwareKeyboard=surface.inputView!=nil&&!surface.softwareKeyboardRequested;
    surface.softwareKeyboardRequested=YES;BOOL explicitSoftwareKeyboard=surface.inputView==nil;
    surface.softwareKeyboardRequested=NO;explicitSoftwareKeyboard&=surface.inputView!=nil;
    BOOL special=YES;
    const UIKeyboardHIDUsage codes[]={UIKeyboardHIDUsageKeyboardEscape,UIKeyboardHIDUsageKeyboardReturnOrEnter,UIKeyboardHIDUsageKeyboardTab,UIKeyboardHIDUsageKeyboardDeleteOrBackspace,UIKeyboardHIDUsageKeyboardUpArrow,UIKeyboardHIDUsageKeyboardDownArrow,UIKeyboardHIDUsageKeyboardLeftArrow,UIKeyboardHIDUsageKeyboardRightArrow,UIKeyboardHIDUsageKeyboardF1,UIKeyboardHIDUsageKeyboardF12,UIKeyboardHIDUsageKeyboardF13,UIKeyboardHIDUsageKeyboardF24};
    const uint32_t symbols[]={0xff1b,0xff0d,0xff09,0xff08,0xff52,0xff54,0xff51,0xff53,0xffbe,0xffc9,0xffca,0xffd5};
    for(unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++){
        [client.events removeAllObjects];NSSet *presses=[NSSet setWithObject:LRKeyboardPress(codes[i],@"",0)];
        [surface pressesBegan:presses withEvent:nil];[surface pressesEnded:presses withEvent:nil];
        special&=[client.events isEqual:@[@[@(symbols[i]),@YES],@[@(symbols[i]),@NO]]];
    }
    [client.events removeAllObjects];
    NSSet *unicode=[NSSet setWithObject:LRKeyboardPress(UIKeyboardHIDUsageKeyboardE,@"é",0)];
    [surface pressesBegan:unicode withEvent:nil];[surface pressesEnded:unicode withEvent:nil];
    BOOL layout=[client.events isEqual:@[@[@0xe9,@YES],@[@0xe9,@NO]]];
    [client.events removeAllObjects];
    [surface pressesBegan:[NSSet setWithObject:LRKeyboardPress(UIKeyboardHIDUsageKeyboardA,@"A",UIKeyModifierShift)] withEvent:nil];
    NSSet *repeat=[NSSet setWithObject:LRKeyboardPress(UIKeyboardHIDUsageKeyboardA,@"a",0)];
    [surface pressesBegan:repeat withEvent:nil];[surface pressesEnded:repeat withEvent:nil];
    BOOL repeats=[client.events isEqual:@[@[@0xffe1,@YES],@[@65,@YES],@[@0xffe1,@NO],@[@65,@NO],@[@97,@YES],@[@97,@NO]]];
    BOOL modifiers=YES;UIKeyModifierFlags flags[]={UIKeyModifierControl,UIKeyModifierAlternate,UIKeyModifierShift,UIKeyModifierCommand};uint32_t mods[]={0xffe3,0xffe9,0xffe1,0xffeb};
    for(unsigned i=0;i<4;i++){
        [client.events removeAllObjects];
        [surface pressesBegan:[NSSet setWithObject:LRKeyboardPress(UIKeyboardHIDUsageKeyboardC,@"c",flags[i])] withEvent:nil];
        [surface pressesCancelled:NSSet.set withEvent:nil];
        modifiers&=[client.events containsObject:@[@(mods[i]),@YES]]&&[client.events containsObject:@[@(mods[i]),@NO]]&&[client.events containsObject:@[@99,@NO]]&&surface.pressedKeys.count==0&&surface.sentModifiers==0;
    }
    BOOL pass=noAutomaticSoftwareKeyboard&&explicitSoftwareKeyboard&&special&&layout&&repeats&&modifiers;
    return @{@"status":pass?@"PASS":@"FAIL",@"software_keyboard_opt_in":@(noAutomaticSoftwareKeyboard&&explicitSoftwareKeyboard),@"arrows_function_and_editing_keys":@(special),@"layout_characters":@(layout),@"repeat_releases_previous_keysym":@(repeats),@"modifier_and_cancel_release":@(modifiers),@"scope":@"Synthetic UIKit press events through production mapper; physical keyboards untested"};
}
static NSDictionary *LRTouchpadContract(void) {
    LRFramebufferView *surface=[LRFramebufferView new];
    [surface.metal removeFromSuperview];surface.metal=nil;
    LRInputRecordingClient *client=[[LRInputRecordingClient alloc]initWithPort:0 password:@""];client.events=[NSMutableArray new];surface.client=client;
    surface.pixelWidth=1280;surface.pixelHeight=720;surface.pointerX=640;surface.pointerY=360;
    surface.relativeTouchpad=YES;[client.events removeAllObjects];
    [surface relativeMoveX:100 y:40];
    BOOL motion=[client.events.lastObject isEqual:@[@790,@420,@0]];
    [surface click:4 at:CGPointZero];
    BOOL click=client.events.count==3&&[client.events[1] isEqual:@[@790,@420,@4]]&&[client.events[2] isEqual:@[@790,@420,@0]];
    surface.pointerTouchButton=1;[surface relativeMoveX:10 y:10];
    BOOL drag=[client.events.lastObject isEqual:@[@805,@435,@1]];
    [surface insertText:@"a"];
    BOOL keyboard=[client.events.lastObject isEqual:@[@97,@NO]];
    [surface click:1 at:CGPointZero];BOOL left=[client.events containsObject:@[@805,@435,@1]];
    [surface click:8 at:CGPointZero];[surface click:16 at:CGPointZero];BOOL scroll=[client.events containsObject:@[@805,@435,@8]]&&[client.events containsObject:@[@805,@435,@16]];
    [surface stroke:0xff1b];[surface stroke:0xff09];BOOL special=[client.events containsObject:@[@0xff1b,@YES]]&&[client.events containsObject:@[@0xff09,@YES]];
    BOOL modifiers=YES;UIKeyModifierFlags flags[]={UIKeyModifierControl,UIKeyModifierAlternate,UIKeyModifierShift,UIKeyModifierCommand};uint32_t symbols[]={0xffe3,0xffe9,0xffe1,0xffeb};
    for(unsigned i=0;i<4;i++){[surface toggleModifier:flags[i]];[surface toggleModifier:flags[i]];modifiers&=[client.events containsObject:@[@(symbols[i]),@YES]]&&[client.events containsObject:@[@(symbols[i]),@NO]];}
    keyboard&=special&&modifiers;click&=left&&scroll;
    NSMutableData *pixels=[NSMutableData dataWithLength:1280*720*4];
    [surface presentPixels:pixels width:1280 height:720];
    BOOL hidden=surface.image.image==nil&&!surface.touchpadHint.hidden&&surface.latestPixels==pixels;
    surface.relativeTouchpad=NO;
    BOOL restored=surface.image.image!=nil&&surface.touchpadHint.hidden&&surface.pointerTouchButton==0;
    return @{@"relative_motion":@(motion),@"secondary_click":@(click),@"held_drag":@(drag),@"keyboard":@(keyboard),@"no_phone_frame_conversion":@(hidden),@"disconnect_restores_view":@(restored),@"status":motion&&click&&drag&&keyboard&&hidden&&restored?@"PASS":@"FAIL"};
}
static NSDictionary *LRLandscapeViewportContract(void) {
    LRFramebufferView *surface=[LRFramebufferView new];[surface.metal removeFromSuperview];surface.metal=nil;
    LRInputRecordingClient *client=[[LRInputRecordingClient alloc]initWithPort:0 password:@""];client.events=[NSMutableArray new];surface.client=client;
    NSMutableData *pixels=[NSMutableData dataWithLength:960*540*4];memset(pixels.mutableBytes,31,pixels.length);
    [surface presentPixels:pixels width:960 height:540];
    CGSize bounds[]={CGSizeMake(390,844),CGSizeMake(844,390),CGSizeMake(390,400),CGSizeMake(1024,1366),CGSizeMake(1920,1080)};
    BOOL fit=YES,mapping=YES,identity=YES;NSMutableArray *frames=[NSMutableArray new];
    for(unsigned i=0;i<sizeof(bounds)/sizeof(bounds[0]);i++){
        surface.frame=(CGRect){CGPointZero,bounds[i]};[surface setNeedsLayout];[surface layoutIfNeeded];CGRect r=surface.image.frame;
        fit&=fabs(r.size.width/r.size.height-960.0/540.0)<.0001&&r.origin.x>=-.001&&r.origin.y>=-.001&&CGRectGetMaxX(r)<=bounds[i].width+.001&&CGRectGetMaxY(r)<=bounds[i].height+.001;
        [surface position:CGPointMake(CGRectGetMidX(r),CGRectGetMidY(r))];mapping&=surface.pointerX==480&&surface.pointerY==270;
        [surface position:CGPointMake(CGRectGetMinX(r),CGRectGetMinY(r))];mapping&=surface.pointerX==0&&surface.pointerY==0;
        [surface position:CGPointMake(CGRectGetMaxX(r),CGRectGetMaxY(r))];mapping&=surface.pointerX==959&&surface.pointerY==539;
        identity&=surface.client==client&&surface.latestPixels==pixels&&surface.pixelWidth==960&&surface.pixelHeight==540;
        [frames addObject:NSStringFromCGRect(r)];
    }
    surface.frame=CGRectMake(0,0,390,844);[surface layoutIfNeeded];NSUInteger count=client.events.count;
    [surface click:1 at:CGPointMake(10,10)];BOOL bars=client.events.count==count;
    [surface click:1 at:CGPointMake(195,422)];BOOL click=client.events.count==count+2;
    [surface insertText:@"a"];BOOL keys=[client.events.lastObject isEqual:@[@97,@NO]];
    return @{@"status":fit&&mapping&&identity&&bars&&click&&keys?@"PASS":@"FAIL",@"landscape_aspect_fit":@(fit),@"center_corners_after_rotation":@(mapping),@"same_client_framebuffer":@(identity),@"letterbox_click_ignored":@(bars),@"click_and_keyboard":@(click&&keys),@"viewport_frames":frames,@"scope":@"UIKit/CG component rotation, keyboard-sized viewport and iPad/external bounds; no physical hardware"};
}
// Exercise the production menu and mode action with a simulated display only.
// This deliberately does not claim a connected monitor or running KDE session.
@interface LRTrackpadMenuContract : LRGUIViewController
@property BOOL testConnected;
@property BOOL testOutput;
@property NSUInteger startRequests;
@property NSString *lastExplanation;
@end
@implementation LRTrackpadMenuContract
- (BOOL)externalDisplayAvailable{return self.testConnected;}
- (BOOL)externalOutputActive{return self.testConnected&&self.testOutput;}
- (void)message:(NSString *)text{self.lastExplanation=text;}
- (void)performDesktopAction:(NSString *)action{if([action isEqual:@"start"]){self.startRequests++;return;}[super performDesktopAction:action];}
@end
static UIAlertAction *LRPadAction(UIAlertController *menu){for(UIAlertAction *action in menu.actions)if([action.title isEqual:@"Use iPhone as Trackpad"]||[action.title isEqual:@"Show Desktop on This Device"]||[action.title isEqual:@"Cancel Trackpad Connection"])return action;return nil;}
static NSDictionary *LRTrackpadMenuContractResult(void) {
    LRTrackpadMenuContract *controller=[[LRTrackpadMenuContract alloc]initDesktop];[controller loadViewIfNeeded];controller.viewerVisible=YES;
    BOOL visible=LRPadAction([controller desktopOptionsMenu]).enabled;
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:LRExternalDisplayEnabledKey];
    [controller performDesktopAction:@"touchpad"];
    BOOL waiting=controller.trackpadRequested&&!controller.surface.relativeTouchpad&&LRExternalOutputEnabled()&&controller.lastExplanation.length>0;
    BOOL cancelVisible=[LRPadAction([controller desktopOptionsMenu]).title isEqual:@"Cancel Trackpad Connection"];
    [controller performDesktopAction:@"touchpad"];BOOL cancelled=!controller.trackpadRequested&&!controller.surface.relativeTouchpad;
    controller.testConnected=YES;
    BOOL outputOffAccessible=LRPadAction([controller desktopOptionsMenu]).enabled;
    [controller performDesktopAction:@"touchpad"];BOOL noPrematureMode=controller.trackpadRequested&&!controller.surface.relativeTouchpad;
    controller.testOutput=YES;[controller externalDisplayChanged:nil];BOOL requestedStart=controller.startRequests>0;
    LRInputRecordingClient *client=[[LRInputRecordingClient alloc]initWithPort:0 password:@""];client.events=[NSMutableArray new];
    controller.client=client;controller.surface.client=client;controller.surface.pixelWidth=1280;controller.surface.pixelHeight=720;
    [controller resumeRequestedTrackpad];BOOL automatic=controller.surface.relativeTouchpad&&!controller.trackpadRequested&&!controller.inputToolbar.hidden;
    [controller performDesktopAction:@"touchpad"];BOOL exited=!controller.surface.relativeTouchpad;
    UIAlertController *menu=[controller desktopOptionsMenu];BOOL ordered=[[menu.actions valueForKey:@"title"] isEqual:@[@"Start / Reconnect Desktop",@"Use iPhone as Trackpad",@"Wireless Display",@"Paste Clipboard Text",@"Task Manager",@"Desktop Settings…",@"Cancel"]]&&LRPadAction(menu).enabled;
    [controller performDesktopAction:@"touchpad"];controller.testConnected=NO;[controller externalDisplayChanged:nil];BOOL reset=!controller.surface.relativeTouchpad&&!controller.trackpadRequested;
    controller.testConnected=YES;[controller performDesktopAction:@"touchpad"];BOOL reentered=controller.surface.relativeTouchpad&&controller.client==client;
    [controller performDesktopAction:@"touchpad"];
    BOOL pass=visible&&waiting&&cancelVisible&&cancelled&&outputOffAccessible&&noPrematureMode&&requestedStart&&automatic&&exited&&ordered&&reset&&reentered;
    return @{@"status":pass?@"PASS":@"FAIL",@"always_accessible":@(visible&&outputOffAccessible),@"enables_output_and_waits":@(waiting&&noPrematureMode),@"cancel_waiting":@(cancelVisible&&cancelled),@"requests_desktop_start":@(requestedStart),@"activates_after_frame":@(automatic),@"ordered_dropdown":@(ordered),@"returns_to_viewer":@(exited),@"disconnect_clears_mode":@(reset),@"reenter_without_new_client":@(reentered),@"scope":@"Simulator production controller/menu, simulated display availability; no physical monitor or KDE claim"};
}
static void LRRunExternalDisplayContract(void (^completion)(NSDictionary *)) {
    LRThreadContractDesktop *threadProbe=[[LRThreadContractDesktop alloc]initDesktop];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{[threadProbe externalDisplayChanged:nil];});
    LRExternalDisplayController *view=[LRExternalDisplayController new];[view loadViewIfNeeded];
    [view.metal removeFromSuperview];view.metal=nil; // Exercise the real CG fallback.
    id original=[NSUserDefaults.standardUserDefaults objectForKey:LRExternalDisplayEnabledKey];
    [NSUserDefaults.standardUserDefaults setBool:YES forKey:LRExternalDisplayEnabledKey];
    __weak LRFramebufferView *releasedSurface;
    BOOL observersOwned=NO;
    @autoreleasepool { LRFramebufferView *surface=[LRFramebufferView new];observersOwned=surface.inputObservers.count>0;releasedSurface=surface; }
    NSMutableDictionary *result=[@{@"environment":@"iOS Simulator component; no connected monitor",@"hardware_test":@NO} mutableCopy];
    view.lastPresentationTime=100;
    result[@"pacing_remaining_budget"]=@([view presentationDelayAt:100.01]>.023&&[view presentationDelayAt:100.01]<.024&&[view presentationDelayAt:101]==0);
    [view showPlaceholder];
    result[@"pacing_idle_and_disconnect"]=@([view presentationDelayAt:100]==0);
    result[@"accessory_registration"]=@"not required before iOS 27";
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000 && !TARGET_OS_MACCATALYST
    __block UISceneAccessoryRegistration *registration=nil;
    if(@available(iOS 27.0,*)) {
        for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)
            if([scene isKindOfClass:UIWindowScene.class]&&[scene.session.role isEqual:UIWindowSceneSessionRoleApplication])
                for(UIWindow *window in ((UIWindowScene *)scene).windows) {
                    LRExternalAccessoryOwner *owner=objc_getAssociatedObject(window.rootViewController,&LRExternalAccessoryKey);
                    if(owner.registration)registration=owner.registration;
                }
        result[@"accessory_registration"]=registration&&registration.enabled?@"PASS":@"FAIL";
    }
#endif
    result[@"trackpad_dropdown"]=LRTrackpadMenuContractResult();
    result[@"landscape_viewport"]=LRLandscapeViewportContract();
    result[@"hardware_keyboard"]=LRHardwareKeyboardContract();
    result[@"surface_observers_owned"]=@(observersOwned);result[@"surface_released"]=@(releasedSurface==nil);
    NSMutableData *first=[NSMutableData dataWithLength:16*16*4];memset(first.mutableBytes,11,first.length);
    [view presentFrame:first width:16 height:16 damage:(LRFrameDamage){0,0,16,16}];
    NSMutableData *final=[first mutableCopy];((uint8_t *)final.mutableBytes)[0]=33;
    [view presentFrame:final width:16 height:16 damage:(LRFrameDamage){0,0,1,1}];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/5),dispatch_get_main_queue(),^{
        result[@"last_update_flushed"]=@([LRTestImageBytes(view) isEqual:final]);
        NSMutableData *large=[NSMutableData dataWithLength:32*16*4];memset(large.mutableBytes,77,large.length);
        [view presentFrame:large width:32 height:16 damage:(LRFrameDamage){0,0,32,16}];
        [view presentFrame:first width:16 height:16 damage:(LRFrameDamage){0,0,16,16}];
        result[@"resize_damage_reset"]=@(view.pendingDamage.width==16&&view.pendingDamage.height==16);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/5),dispatch_get_main_queue(),^{
            result[@"resized_final_pixels"]=@([LRTestImageBytes(view) isEqual:first]);
            [view presentFrame:final width:16 height:16 damage:(LRFrameDamage){0,0,16,16}];
            [view showPlaceholder];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/5),dispatch_get_main_queue(),^{
                result[@"cancelled_frame_stays_blank"]=@(view.image.image==nil&&!view.placeholder.hidden&&!view.frameScheduled&&view.pendingPixels==nil);
                [NSUserDefaults.standardUserDefaults setBool:NO forKey:LRExternalDisplayEnabledKey];
                [view presentFrame:first width:16 height:16 damage:(LRFrameDamage){0,0,16,16}];
                result[@"disabled_does_not_render"]=@(!view.frameScheduled&&view.pendingPixels==nil);
                BOOL passed=YES;for(NSString *name in @[@"surface_observers_owned",@"surface_released",@"last_update_flushed",@"resize_damage_reset",@"resized_final_pixels",@"cancelled_frame_stays_blank",@"disabled_does_not_render"])passed=passed&&[result[name] boolValue];
                NSDictionary *benchmark=LRMeasureHiddenFramePresentation();result[@"hidden_fallback_benchmark"]=benchmark;
                passed=passed&&[benchmark[@"no_hidden_image"] boolValue]&&[benchmark[@"resume_latest_pixels"] boolValue];
                result[@"background_notification_on_main_thread"]=@(threadProbe.detached&&threadProbe.detachedOnMainThread);
                passed=passed&&threadProbe.detached&&threadProbe.detachedOnMainThread;
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000 && !TARGET_OS_MACCATALYST
                if(@available(iOS 27.0,*)) {
                    result[@"accessory_disabled_with_setting"]=@(registration&&!registration.enabled);
                    passed=passed&&[result[@"accessory_registration"] isEqual:@"PASS"]&&[result[@"accessory_disabled_with_setting"] boolValue];
                }
#endif
                NSDictionary *touchpad=LRTouchpadContract();result[@"touchpad"]=touchpad;
                passed=passed&&[touchpad[@"status"] isEqual:@"PASS"]&&[result[@"trackpad_dropdown"][@"status"] isEqual:@"PASS"]&&[result[@"landscape_viewport"][@"status"] isEqual:@"PASS"]&&[result[@"hardware_keyboard"][@"status"] isEqual:@"PASS"];
                result[@"status"]=passed?@"PASS":@"FAIL";
                if(original)[NSUserDefaults.standardUserDefaults setObject:original forKey:LRExternalDisplayEnabledKey];else [NSUserDefaults.standardUserDefaults removeObjectForKey:LRExternalDisplayEnabledKey];
                // Keep output enabled while measuring; restore the saved setting afterwards.
                [NSUserDefaults.standardUserDefaults setBool:YES forKey:LRExternalDisplayEnabledKey];
                LRMeasureExternalPacing(0,[NSMutableArray new],[NSMutableArray new],^(NSDictionary *measurements){
                    result[@"external_pacing_benchmark"]=measurements;
                    BOOL rendered=YES;for(NSString *key in @[@"fixed_delay_first_frame_ms",@"paced_first_frame_ms"])
                        for(NSNumber *latency in measurements[key])rendered&=latency.doubleValue>=0;
                    result[@"status"]=passed&&rendered&&[result[@"pacing_remaining_budget"] boolValue]&&[result[@"pacing_idle_and_disconnect"] boolValue]?@"PASS":@"FAIL";
                    if(original)[NSUserDefaults.standardUserDefaults setObject:original forKey:LRExternalDisplayEnabledKey];else [NSUserDefaults.standardUserDefaults removeObjectForKey:LRExternalDisplayEnabledKey];
                    completion(result);
                });
            });
        });
    });
}
