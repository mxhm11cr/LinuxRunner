#import "GUIViewController.h"
#import "RuntimeBridge.h"
#import "RFBClient.h"
#import "FramebufferPatch.h"
#import "DiagnosticLogTail.h"
#import "RFBProtocol.h"
#import "MetalFramebufferView.h"
#import "AudioBridge.h"
#import "LinuxRunner-Swift.h"
#import <GameController/GameController.h>
#import <objc/runtime.h>
#import "../SceneDelegate.h"
#include <Security/Security.h>
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <sys/stat.h>
static double LRGUIClock(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}

@interface TerminalViewController (LRGUIProcessStatus)
@property (readonly) int sessionPid;
@property (readonly) int runnerLaunchError;
@end

// Persistent, environment-local managed storage over the existing exchange mount.
// The support schema version is independent of the native app build number.
static NSString * const LRGUIFolder=@".linuxrunner/gui";
static NSError *GUIError(NSString *message){return [NSError errorWithDomain:@"dev.linuxrunner.gui" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];}
static NSString *SupportSHA256(NSData *data){
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString *hex=[NSMutableString new];for(unsigned i=0;i<sizeof digest;i++)[hex appendFormat:@"%02x",digest[i]];return hex;
}
static NSURL *PrepareSupport(NSError **error){
    NSFileManager *fm=NSFileManager.defaultManager;
    NSURL *folder=[LRRuntimeBridge exchangeURL:LRRuntimeBridge.activeRootName];
    for(NSString *component in [LRGUIFolder componentsSeparatedByString:@"/"]){
        folder=[folder URLByAppendingPathComponent:component isDirectory:YES];
        NSNumber *symlink=nil;[folder getResourceValue:&symlink forKey:NSURLIsSymbolicLinkKey error:nil];
        if(symlink.boolValue){if(error)*error=GUIError(@"Managed support directory is a symbolic link.");return nil;}
        if(![fm createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:error])return nil;
    }
    NSURL *bundle=[NSBundle.mainBundle URLForResource:@"GUISupport" withExtension:@"bundle"];
    NSData *manifestData=[NSData dataWithContentsOfURL:[bundle URLByAppendingPathComponent:@"support-manifest.json"] options:0 error:error];
    if(!manifestData)return nil;
    NSDictionary *manifest=[NSJSONSerialization JSONObjectWithData:manifestData options:0 error:error];
    if(![manifest isKindOfClass:NSDictionary.class]||![manifest[@"schema"] isEqual:@1]||![manifest[@"resources"] isKindOfClass:NSArray.class]){if(error)*error=GUIError(@"Invalid bundled GUI support manifest.");return nil;}
    [LRRuntimeBridge logCategory:@"SUPPORT_DEPLOY" event:[NSString stringWithFormat:@"version=%@ destination=/home/mobile/Downloads/%@",manifest[@"version"],LRGUIFolder]];
    NSMutableArray *resources=[manifest[@"resources"] mutableCopy];
    [resources addObject:@{@"name":@"support.sha256",@"mode":@"0644"}];
    [resources addObject:@{@"name":@"support-manifest.json",@"mode":@"0644"}];
    for(NSDictionary *resource in resources){
        NSString *name=resource[@"name"];
        if(![name isKindOfClass:NSString.class]||!name.length||![name isEqual:name.lastPathComponent]||[name hasPrefix:@"."]){if(error)*error=GUIError(@"Invalid support-resource name.");return nil;}
        NSData *source=[NSData dataWithContentsOfURL:[bundle URLByAppendingPathComponent:name] options:0 error:error];
        if(!source)return nil;
        if(resource[@"sha256"]&&![SupportSHA256(source) isEqual:resource[@"sha256"]]){if(error)*error=GUIError([NSString stringWithFormat:@"Bundled support checksum mismatch: %@",name]);return nil;}
        NSURL *destination=[folder URLByAppendingPathComponent:name];NSNumber *symlink=nil;
        [destination getResourceValue:&symlink forKey:NSURLIsSymbolicLinkKey error:nil];
        BOOL same=!symlink.boolValue&&[[NSData dataWithContentsOfURL:destination] isEqual:source];
        if(!same&&![source writeToURL:destination options:NSDataWritingAtomic error:error])return nil;
        NSNumber *permissions=[resource[@"mode"] isEqual:@"0755"]?@0755:@0644;
        if(![fm setAttributes:@{NSFilePosixPermissions:permissions} ofItemAtPath:destination.path error:error])return nil;
        [LRRuntimeBridge logCategory:@"SUPPORT_DEPLOY" event:[NSString stringWithFormat:@"%@=%@",name,same?@"unchanged":@"installed"]];
    }
    [LRRuntimeBridge logCategory:@"SUPPORT_DEPLOY" event:@"result=PASS"];
    return folder;
}


static NSString *ApplicationPreflightFailure(NSURL *session) {
    NSURL *log=[session URLByAppendingPathComponent:@"application.log"];NSNumber *symlink=nil;
    [log getResourceValue:&symlink forKey:NSURLIsSymbolicLinkKey error:nil];if(symlink.boolValue)return nil;
    NSFileHandle *file=[NSFileHandle fileHandleForReadingAtPath:log.path];if(!file)return nil;
    unsigned long long length=[file seekToEndOfFile];[file seekToFileOffset:length>256*1024?length-256*1024:0];
    NSData *data=[file readDataToEndOfFile];[file closeFile];NSString *output=[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding];
    for(NSString *line in [output componentsSeparatedByString:@"\n"])
        if(([line hasPrefix:@"BROWSER_PREFLIGHT:"]||[line hasPrefix:@"FIREFOX_PREFLIGHT:"]||[line hasPrefix:@"SUPPORT_DEPLOY:"])&&[line containsString:@"result=FAIL"])
            return [line stringByAppendingString:@". See GUI Logs for details."];
    return nil;
}

#import "TouchScrollPolicy.h"
#import "PointerPolicy.h"
#import "KeyboardPolicy.h"
#import "DisplayPolicy.h"
// Locally drawn pointer. The RFB Cursor pseudo-encoding gives the guest's
// real cursor image and hotspot; x11vnc then leaves it out of the
// framebuffer, so moving the pointer needs no framebuffer update at all.
@interface LRCursorOverlay : NSObject
@property(readonly) CALayer *layer;
@property uint16_t width,height,hotX,hotY;
@property(nullable) NSData *shape;
- (void)setShape:(nullable NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y;
- (void)placeInFrame:(CGRect)frame pixelWidth:(uint16_t)pw pixelHeight:(uint16_t)ph x:(uint16_t)x y:(uint16_t)y hidden:(BOOL)hidden;
@end
@implementation LRCursorOverlay
- (instancetype)init{
    if((self=[super init])){_layer=[CALayer layer];_layer.zPosition=1000;_layer.magnificationFilter=kCAFilterNearest;_layer.hidden=YES;
        _layer.actions=@{@"position":NSNull.null,@"bounds":NSNull.null,@"frame":NSNull.null,@"contents":NSNull.null,@"hidden":NSNull.null};}
    return self;
}
- (void)setShape:(NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y{
    _shape=bgra;_width=bgra?w:0;_height=bgra?h:0;_hotX=x;_hotY=y;
    if(!bgra||!w||!h||bgra.length!=(NSUInteger)w*h*4){_layer.contents=nil;_layer.hidden=YES;_width=_height=0;return;}
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)bgra);CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGImageRef image=CGImageCreate(w,h,8,32,w*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,NULL,NO,kCGRenderingIntentDefault);
    _layer.contents=(__bridge id)image;if(image)CGImageRelease(image);CGColorSpaceRelease(space);CGDataProviderRelease(provider);
}
- (void)placeInFrame:(CGRect)f pixelWidth:(uint16_t)pw pixelHeight:(uint16_t)ph x:(uint16_t)x y:(uint16_t)y hidden:(BOOL)hidden{
    if(hidden||!_width||!_height||!pw||!ph||f.size.width<=1||!_layer.contents){_layer.hidden=YES;return;}
    // Desktop scale, but never so small that the pointer is hard to see on a
    // phone showing 960x540 in portrait. The hotspot stays on the pointer.
    CGFloat s=f.size.width/pw,c=MAX(s,0.6);
    [CATransaction begin];[CATransaction setDisableActions:YES];
    _layer.frame=CGRectMake(f.origin.x+(x+0.5)*s-(_hotX+0.5)*c,f.origin.y+(y+0.5)*s-(_hotY+0.5)*c,_width*c,_height*c);_layer.hidden=NO;
    [CATransaction commit];
}
@end
// The external display (declared below) shows the same pointer.
@protocol LRCursorDisplay
- (void)setCursorShape:(nullable NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y;
- (void)placeCursorX:(uint16_t)x y:(uint16_t)y;
@end
static id<LRCursorDisplay> LRExternalCursorDisplay(void);
// Portrait touchpad: a rounded pad in the black space beneath the desktop.
// It owns its touches; LRFramebufferView's own recognizers ignore them.
@interface LRPortraitTouchpadView : UIView
@property UILabel *hint;
@end
@implementation LRPortraitTouchpadView
- (instancetype)init{
    if((self=[super initWithFrame:CGRectZero])){
        self.backgroundColor=[UIColor colorWithWhite:.11 alpha:1];
        self.layer.cornerRadius=20;self.layer.cornerCurve=kCACornerCurveContinuous;
        self.layer.borderWidth=1;self.layer.borderColor=[UIColor colorWithWhite:1 alpha:.12].CGColor;
        self.multipleTouchEnabled=YES;self.isAccessibilityElement=YES;
        self.accessibilityLabel=@"Desktop touchpad";
        self.accessibilityHint=@"Slide to move the pointer, tap to click, two fingers to scroll or tap for right click, hold then slide to drag.";
        self.accessibilityTraits=UIAccessibilityTraitAllowsDirectInteraction;
        _hint=[UILabel new];_hint.numberOfLines=0;_hint.textAlignment=NSTextAlignmentCenter;_hint.adjustsFontForContentSizeCategory=YES;
        _hint.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];_hint.textColor=[UIColor colorWithWhite:1 alpha:.55];
        _hint.text=@"Touchpad\nSlide to move · Tap to click\nTwo fingers: scroll · Two-finger tap: right-click\nHold, then slide: drag";
        _hint.isAccessibilityElement=NO;[self addSubview:_hint];
    }return self;
}
- (void)layoutSubviews{[super layoutSubviews];_hint.frame=CGRectInset(self.bounds,16,12);}
// Keep these touches away from the desktop surface's pointer-mode handlers.
- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e{}
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e{}
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e{}
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e{}
@end
@interface LRFramebufferView : UIView <UIKeyInput,UIGestureRecognizerDelegate,UIPointerInteractionDelegate>
@property UIImageView *image;
@property LRRFBClient *client;
@property uint16_t pixelWidth,pixelHeight;
@property uint16_t pointerX,pointerY;
@property UIKeyModifierFlags stickyModifiers,hardwareModifiers,sentModifiers;
@property BOOL secondaryTwoFinger,traceInput;
@property(nonatomic) BOOL softwareKeyboardRequested;
@property UIView *hardwareKeyboardInputView;
@property NSMutableDictionary<NSNumber *,NSNumber *> *pressedKeys;
@property unsigned traceCount;
@property CGFloat zoom;
@property CGPoint viewportOffset;
@property LRMetalFramebufferView *metal;
@property NSData *latestPixels;
@property BOOL presentationSuppressed;
@property(nonatomic) BOOL relativeTouchpad;
@property UILabel *touchpadHint;
@property CGPoint touchpadDragPoint;
@property LRPortraitTouchpadView *portraitPad;
@property BOOL softwareKeyboardVisible,portraitPadActive;
@property NSUInteger hintGeneration;
@property BOOL pointerEverSent;
@property LRCursorOverlay *cursor;
// Linux keys above the iOS keyboard, and the one keyboard toggle (above the
// portrait touchpad, top-right in landscape, bottom-right as a DeX touchpad).
@property(nullable) UIView *keyAccessory;
@property UIButton *keyboardButton;
@property BOOL desktopControls;
// Portrait: one dock of large controls directly above the touchpad
// (Keyboard, DeX, Settings, More, Power); the navigation bar is hidden then.
@property(nullable) UIView *dock;
@property(nullable) UIButton *dockKeyboardButton;
@property BOOL dockShown;
@property BOOL dockCollapsed;          // landscape: pill folded into a small handle
@property(nullable) UIButton *dockHandle;
@property(nullable,copy) void (^dockVisibilityChanged)(BOOL shown);
@property UIButton *touchpadHelp;
@property NSData *patchPixels;
@property(nonatomic,strong) UIPanGestureRecognizer *twoFingerScroll;
@property(nonatomic,strong) UIPinchGestureRecognizer *viewportPinch;
// External mouse (GameController GCMouse): relative motion, buttons, wheel.
@property uint8_t mouseButtons;
@property double mouseX,mouseY,mouseScroll;
@property CFTimeInterval lastMouseButtonTime;
// Pointer arbitration (PointerPolicy.h) and input diagnostics.
@property UITapGestureRecognizer *tapGesture;
@property UIPanGestureRecognizer *dragGesture;
@property UILongPressGestureRecognizer *secondaryGesture;
@property(weak) UITouch *pointerTouch;
@property uint8_t pointerTouchButton;
@property uint8_t lastSentPointerButtons;
@property BOOL sawIndirectPointer,sawHover,pointerModeActive;
@property CFTimeInterval lastGCScroll;
@property NSMutableDictionary<NSString *,NSNumber *> *inputCounts;
@property NSMutableArray *inputObservers;
@property double latencySum,latencyMax;
@property NSUInteger latencyCount;
@property NSString *lastInputSource;
// Input-to-display: a button press sent, until the next frame is presented.
@property CFTimeInterval pressSentAt,lastTouchDown;
@property double displaySum,displayMax;
@property NSUInteger displayCount;
- (void)noteFramePresented;
@property LRTwoFingerIntent twoFingerIntent;
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height;
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage;
- (void)presentPatch:(NSData *)patch width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage;
- (void)clearFrame;
- (void)configurePresentation;
- (NSDictionary *)presentationStatistics;
- (void)fitViewport;
- (void)actualViewport;
- (void)releaseInput;
- (void)focusKeyboard;
- (void)relativeMoveX:(CGFloat)x y:(CGFloat)y;
- (void)updateCursor;
- (void)setCursorShape:(nullable NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y;
- (void)refreshExternalCursor;
- (void)stroke:(uint32_t)symbol;
- (void)toggleModifier:(UIKeyModifierFlags)flag;
@end
static const NSInteger LRDockFoldTag=7802;
static const NSInteger LRDockPortraitOnlyTag=7803;   // DeX, Settings, More, Power: not in the small landscape pill
@implementation LRFramebufferView
- (instancetype)init {
    if((self=[super initWithFrame:CGRectZero])){
        _zoom=1;_inputObservers=[NSMutableArray new];self.clipsToBounds=YES;_pressedKeys=[NSMutableDictionary new];self.backgroundColor=UIColor.blackColor;_image=[UIImageView new];_image.contentMode=UIViewContentModeScaleAspectFit;_image.layer.magnificationFilter=kCAFilterNearest;
        [self addSubview:_image];_cursor=[LRCursorOverlay new];[self.layer addSublayer:_cursor.layer];self.accessibilityIdentifier=@"Linux GUI surface";self.isAccessibilityElement=YES;self.accessibilityLabel=@"Linux GUI display";
        _touchpadHint=[UILabel new];_touchpadHint.hidden=YES;_touchpadHint.numberOfLines=0;_touchpadHint.textAlignment=NSTextAlignmentCenter;_touchpadHint.textColor=[UIColor.whiteColor colorWithAlphaComponent:.85];_touchpadHint.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        _touchpadHint.text=@"External Desktop Touchpad\nSlide: move · Tap: click · Hold and slide: drag\nTwo fingers: scroll or tap to right-click\nTap the keyboard button to type.";
        [self addSubview:_touchpadHint];
        _touchpadHelp=[UIButton buttonWithType:UIButtonTypeSystem];_touchpadHelp.hidden=YES;
        [_touchpadHelp setImage:[UIImage systemImageNamed:@"questionmark.circle"] forState:UIControlStateNormal];
        _touchpadHelp.tintColor=[UIColor colorWithWhite:1 alpha:.45];_touchpadHelp.accessibilityLabel=@"Show touchpad help";
        [_touchpadHelp addTarget:self action:@selector(showTouchpadHelp) forControlEvents:UIControlEventTouchUpInside];
        [self addSubview:_touchpadHelp];
        [self configurePresentation];
        _inputCounts=[NSMutableDictionary new];
        UITapGestureRecognizer *tap=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tap:)];[self addGestureRecognizer:tap];_tapGesture=tap;
        UIPanGestureRecognizer *drag=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(drag:)];drag.maximumNumberOfTouches=1;[self addGestureRecognizer:drag];_dragGesture=drag;
        UILongPressGestureRecognizer *secondary=[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(secondary:)];secondary.minimumPressDuration=.55;[self addGestureRecognizer:secondary];_secondaryGesture=secondary;[tap requireGestureRecognizerToFail:secondary];
        UITapGestureRecognizer *two=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(twoTap:)];two.numberOfTouchesRequired=2;[self addGestureRecognizer:two];
        UIPanGestureRecognizer *scroll=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(scroll:)];scroll.minimumNumberOfTouches=2;scroll.maximumNumberOfTouches=2;[self addGestureRecognizer:scroll];[two requireGestureRecognizerToFail:scroll];
        UIPinchGestureRecognizer *pinch=[[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinch:)];[self addGestureRecognizer:pinch];[two requireGestureRecognizerToFail:pinch];
        _twoFingerScroll=scroll;_viewportPinch=pinch;scroll.delegate=self;pinch.delegate=self;
        UIHoverGestureRecognizer *hover=[[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(hover:)];[self addGestureRecognizer:hover];
        UITapGestureRecognizer *right=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(rightMouse:)];right.buttonMaskRequired=UIEventButtonMaskSecondary;right.allowedTouchTypes=@[@(UITouchTypeIndirectPointer)];[self addGestureRecognizer:right];
        [self observeMice];
        // iPad pointer: hide the system pointer over the desktop; the Linux cursor is drawn in the framebuffer.
        [self addInteraction:[[UIPointerInteraction alloc] initWithDelegate:self]];
        __weak typeof(self) weakSelf=self;
        for(NSNotificationName name in @[UIAccessibilityAssistiveTouchStatusDidChangeNotification,GCMouseDidDisconnectNotification,NSUserDefaultsDidChangeNotification])
            [_inputObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){[weakSelf refreshPointerMode];}]];
        // Deliver touch-up immediately: in pointer mode it is the mouse-button release.
        for(UIGestureRecognizer *g in self.gestureRecognizers)g.delaysTouchesEnded=NO;
        [self refreshPointerMode];
        [self buildPortraitPad];
        for(UIGestureRecognizer *g in self.gestureRecognizers)g.delegate=self;
        UIPanGestureRecognizer *wheel=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(wheel:)];wheel.allowedScrollTypesMask=UIScrollTypeMaskAll;wheel.allowedTouchTypes=@[];[self addGestureRecognizer:wheel];
    }return self;
}
- (void)dealloc{for(id observer in _inputObservers)[NSNotificationCenter.defaultCenter removeObserver:observer];}
// ---- Portrait touchpad (iPhone portrait, no external display) ----------------
- (void)buildPortraitPad{
    _portraitPad=[LRPortraitTouchpadView new];_portraitPad.hidden=YES;[self addSubview:_portraitPad];
    UIPanGestureRecognizer *move=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(padMove:)];move.maximumNumberOfTouches=1;
    UITapGestureRecognizer *tap=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(padTap:)];
    UILongPressGestureRecognizer *hold=[[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(padHold:)];hold.minimumPressDuration=.35;hold.allowableMovement=10;
    UITapGestureRecognizer *two=[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(padRightTap:)];two.numberOfTouchesRequired=2;
    UIPanGestureRecognizer *scroll=[[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(padScroll:)];scroll.minimumNumberOfTouches=2;scroll.maximumNumberOfTouches=2;
    [two requireGestureRecognizerToFail:scroll];
    for(UIGestureRecognizer *g in @[move,tap,hold,two,scroll]){g.delaysTouchesEnded=NO;[_portraitPad addGestureRecognizer:g];}
    __weak typeof(self) weakSelf=self;
    // Only the software keyboard counts: a hardware keyboard reports a tiny or off-screen frame.
    [_inputObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:UIKeyboardWillChangeFrameNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){
        typeof(self) self=weakSelf;if(!self||!self.window)return;
        CGRect end=[n.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
        CGRect screen=self.window.screen.bounds;
        BOOL visible=CGRectGetHeight(CGRectIntersection(end,screen))>80;
        [self setSoftwareKeyboardVisible:visible animated:[n.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue]];
    }]];
    [_inputObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:UIKeyboardWillHideNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){
        [weakSelf setSoftwareKeyboardVisible:NO animated:[n.userInfo[UIKeyboardAnimationDurationUserInfoKey] doubleValue]];
    }]];
}
- (void)setSoftwareKeyboardVisible:(BOOL)visible animated:(double)duration{
    if(_softwareKeyboardVisible==visible)return;
    _softwareKeyboardVisible=visible;[self setNeedsLayout];
    [UIView animateWithDuration:MAX(.15,duration) delay:0 options:UIViewAnimationOptionBeginFromCurrentState|UIViewAnimationOptionAllowUserInteraction animations:^{[self layoutIfNeeded];} completion:nil];
}
- (BOOL)portraitTouchpadEnabled{
    id v=[NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerDesktop.portraitTouchpad"];
    return v?[v boolValue]:YES;
}
// Instructional text (OLED care): show, then fade after LR_TOUCHPAD_HINT_SECONDS.
- (void)showHint:(UIView *)label{
    NSUInteger generation=++_hintGeneration;
    [UIView animateWithDuration:.25 delay:0 options:UIViewAnimationOptionBeginFromCurrentState|UIViewAnimationOptionAllowUserInteraction animations:^{label.alpha=1;} completion:nil];
    __weak typeof(self) weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(LR_TOUCHPAD_HINT_SECONDS*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        typeof(self) self=weakSelf;if(!self||self.hintGeneration!=generation)return;
        [UIView animateWithDuration:1.2 delay:0 options:UIViewAnimationOptionBeginFromCurrentState|UIViewAnimationOptionAllowUserInteraction animations:^{label.alpha=0;} completion:nil];
    });
}
- (void)showTouchpadHelp{
    if(_relativeTouchpad)[self showHint:_touchpadHint];
    else if(_portraitPadActive)[self showHint:_portraitPad.hint];
}
- (void)syncPadPointer{
    // Start from where the pointer is (a direct tap on the desktop may have moved it).
    if((uint16_t)_mouseX!=_pointerX||(uint16_t)_mouseY!=_pointerY){_mouseX=_pointerX;_mouseY=_pointerY;}
}
// Portrait pad motion is relative to the desktop as displayed: the iPhone
// shows it scaled down, so a finger moves the pointer by the same on-screen
// distance (times the sensitivity) on every screen size.
- (CGFloat)padDisplayScale{
    CGFloat shown=_image.frame.size.width;
    return (_pixelWidth&&shown>1)?(CGFloat)_pixelWidth/shown:1;
}
- (void)padMove:(UIPanGestureRecognizer *)g{
    if(g.state==UIGestureRecognizerStateBegan)[self syncPadPointer];
    CGPoint t=[g translationInView:_portraitPad];[g setTranslation:CGPointZero inView:_portraitPad];
    CGFloat k=[self padDisplayScale];
    [self relativeMoveX:t.x*k y:t.y*k];
}
- (void)padClick:(uint8_t)button{
    [self syncPadPointer];[self focusKeyboard];
    [self sendPointerFromEventTime:_lastTouchDown buttons:button];[self sendPointerFromEventTime:0 buttons:0];
}
- (void)padTap:(UITapGestureRecognizer *)g{_lastTouchDown=CACurrentMediaTime();[self padClick:1];}
- (void)padRightTap:(UITapGestureRecognizer *)g{_lastTouchDown=CACurrentMediaTime();[self padClick:4];}
- (void)padHold:(UILongPressGestureRecognizer *)g{
    CGPoint p=[g locationInView:_portraitPad];
    if(g.state==UIGestureRecognizerStateBegan){[self syncPadPointer];_touchpadDragPoint=p;_pointerTouchButton=1;[self relativeMoveX:0 y:0];}
    else if(g.state==UIGestureRecognizerStateChanged){CGFloat k=[self padDisplayScale];[self relativeMoveX:(p.x-_touchpadDragPoint.x)*k y:(p.y-_touchpadDragPoint.y)*k];_touchpadDragPoint=p;}
    else{_pointerTouchButton=0;[self relativeMoveX:0 y:0];}
}
- (void)padScroll:(UIPanGestureRecognizer *)g{
    CGPoint t=[g translationInView:_portraitPad];
    if(fabs(t.y)>=12/lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.scrollGain"],1)){
        [self syncPadPointer];[self sendPointerFromEventTime:0 buttons:t.y<0?16:8];[self sendPointerFromEventTime:0 buttons:0];
        [g setTranslation:CGPointZero inView:_portraitPad];
    }
}
// Desktop-surface recognizers never see touches that start on the pad or on a control.
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)g shouldReceiveTouch:(UITouch *)touch{
    if(g.view!=self)return YES;
    if(_portraitPadActive&&[touch.view isDescendantOfView:_portraitPad])return NO;
    // The control pill and its handle own their touches (menus included).
    if((_dock&&[touch.view isDescendantOfView:_dock])||(_dockHandle&&[touch.view isDescendantOfView:_dockHandle]))return NO;
    for(UIView *v=touch.view;v&&v!=self;v=v.superview)if([v isKindOfClass:UIControl.class])return NO;
    return YES;
}
- (void)setRelativeTouchpad:(BOOL)enabled {
    if(_relativeTouchpad==enabled)return;
    [self releaseInput];_pointerTouchButton=0;_relativeTouchpad=enabled;
    _mouseX=_pointerX;_mouseY=_pointerY;
    _presentationSuppressed=enabled;_touchpadHint.hidden=!enabled;
    _image.hidden=enabled||_metal!=nil;_metal.hidden=enabled;
    _touchpadHelp.hidden=!enabled;
    if(enabled){[_metal clearFrame];_image.image=nil;[self bringSubviewToFront:_touchpadHint];[self bringSubviewToFront:_touchpadHelp];[self showHint:_touchpadHint];}
    else if(_latestPixels)[self presentPixels:_latestPixels width:_pixelWidth height:_pixelHeight];
    [self setNeedsLayout];
    self.accessibilityLabel=enabled?@"External desktop touchpad":@"Linux GUI display";
    [self refreshPointerMode];
}
- (void)updateCursor{
    // DeX-style output: the phone is a touchpad, the pointer is on the display.
    [_cursor placeInFrame:_image.frame pixelWidth:_pixelWidth pixelHeight:_pixelHeight x:_pointerX y:_pointerY hidden:_relativeTouchpad||_presentationSuppressed];
    [LRExternalCursorDisplay() placeCursorX:_pointerX y:_pointerY];
}
- (void)setCursorShape:(NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y{
    [_cursor setShape:bgra width:w height:h hotX:x hotY:y];[self refreshExternalCursor];[self updateCursor];
}
- (void)refreshExternalCursor{
    [LRExternalCursorDisplay() setCursorShape:_cursor.shape width:_cursor.width height:_cursor.height hotX:_cursor.hotX hotY:_cursor.hotY];
    [LRExternalCursorDisplay() placeCursorX:_pointerX y:_pointerY];
}
- (void)relativeMoveX:(CGFloat)x y:(CGFloat)y {
    double px=_mouseX,py=_mouseY;
    double gain=lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.touchpadGain"],1.5);
    if(lr_touchpad_move(&px,&py,x*gain/1.5,y*gain/1.5,_pixelWidth,_pixelHeight)) {
        _pointerX=(uint16_t)px;_pointerY=(uint16_t)py;_mouseX=px;_mouseY=py;
        [self sendPointerFromEventTime:CACurrentMediaTime() buttons:_mouseButtons|_pointerTouchButton];
    }
}
- (void)configurePresentation {
    if (!LRRuntimeBridge.desktopMetalPresentationEnabled) {
        [_metal removeFromSuperview]; _metal = nil; _image.hidden = _relativeTouchpad;
        if (_latestPixels) [self presentPixels:_latestPixels width:_pixelWidth height:_pixelHeight];
        return;
    }
    if (_metal) return;
        if (LRRuntimeBridge.desktopMetalPresentationEnabled) {
            __weak typeof(self) weak = self;
            _metal = [[LRMetalFramebufferView alloc] initWithFailure:^(NSString *reason) {
                typeof(self) self = weak; if (!self) return;
                [self.metal removeFromSuperview]; self.metal = nil; self.image.hidden = NO;
                [LRRuntimeBridge logCategory:@"GUI_PRESENTATION" event:[@"Metal failed; Core Graphics fallback: " stringByAppendingString:reason]];
                [self presentPixels:self.latestPixels width:self.pixelWidth height:self.pixelHeight];
            }];
            if (_metal) { [self addSubview:_metal]; _image.hidden = YES; _metal.hidden=_relativeTouchpad; }
            [LRRuntimeBridge logCategory:@"GUI_PRESENTATION" event:_metal ? @"Metal final framebuffer presentation selected; guest rendering unchanged" : @"Metal unavailable; Core Graphics fallback selected"];
        }
}
- (void)layoutSubviews{[super layoutSubviews];
    _touchpadHint.frame=CGRectInset(self.bounds,16,16);
    CGFloat inset=MAX(8,self.safeAreaInsets.right+8);
    _touchpadHelp.frame=CGRectMake(self.bounds.size.width-inset-36,MAX(8,self.safeAreaInsets.top+8),36,36);
    // Portrait: same aspect-fit desktop, top-aligned; the pad fills the space beneath.
    lr_rect desk,pad;
    BOOL usePad=[self portraitTouchpadEnabled]&&lr_portrait_touchpad_layout(self.bounds.size.width,self.bounds.size.height,_pixelWidth,_pixelHeight,
        true,_softwareKeyboardVisible,_relativeTouchpad,&desk,&pad);
    if(usePad!=_portraitPadActive){
        _portraitPadActive=usePad;
        if(usePad){_portraitPad.hint.alpha=0;[self showHint:_portraitPad.hint];}
    }
    _portraitPad.hidden=!usePad;
    // Portrait: one connected column from the top -- desktop (aspect fit, full
    // width), the control pill right under it, and the touchpad under that,
    // growing to fill what is left. Landscape: the desktop fills the screen
    // and the pill floats at the top (collapsible). DeX: pill at the bottom.
    CGRect area=self.bounds;
    UIEdgeInsets safe=self.safeAreaInsets;
    CGFloat W=self.bounds.size.width,H=self.bounds.size.height;
    BOOL portrait=H>W;
    const CGFloat pillH=50,gap=10;
    BOOL pill=_dock&&_desktopControls;
    if(portrait&&!_relativeTouchpad){
        CGFloat top=MAX(safe.top,8)+4;
        CGFloat dh=(_pixelWidth&&_pixelHeight)?W*_pixelHeight/_pixelWidth:W*9/16;
        dh=MIN(dh,(H-top)*0.55);
        area=CGRectMake(0,top,W,dh);
        CGFloat y=top+dh+gap;
        if(pill){CGFloat pw=MIN(W-24,360);_dock.frame=CGRectMake((W-pw)/2,y,pw,pillH);y+=pillH+gap;}
        if(usePad){
            CGFloat bottom=H-MAX(safe.bottom,8)-6;
            _portraitPad.frame=CGRectMake(12,y,W-24,MAX(120,bottom-y));
            [self bringSubviewToFront:_portraitPad];
        }
    } else if(pill&&_relativeTouchpad){
        CGFloat pw=MIN(W-32,360);
        _dock.frame=CGRectMake((W-pw)/2,H-MAX(safe.bottom,12)-pillH-6,pw,pillH);
    } else if(pill){
        // Landscape: a small pill with only Keyboard and Clipboard (and fold).
        CGFloat pw=176,ph=42;
        _dock.frame=CGRectMake((W-pw)/2,MAX(safe.top,6)+4,pw,ph);
        _dockHandle.frame=CGRectMake((W-64)/2,MAX(safe.top,4),64,22);
    }
    BOOL landscapeFloating=pill&&!portrait&&!_relativeTouchpad;
    BOOL showDock=pill&&!(landscapeFloating&&_dockCollapsed);
    _dock.hidden=!showDock;
    _dockHandle.hidden=!(landscapeFloating&&_dockCollapsed);
    [_dock viewWithTag:LRDockFoldTag].hidden=!landscapeFloating;
    for(UIView *v in [_dock viewWithTag:LRDockFoldTag].superview.subviews)if(v.tag==LRDockPortraitOnlyTag)v.hidden=landscapeFloating;
    _dock.layer.cornerRadius=_dock.bounds.size.height/2;
    if(showDock)[self bringSubviewToFront:_dock];
    if(!_dockHandle.hidden)[self bringSubviewToFront:_dockHandle];
    if(pill!=_dockShown){_dockShown=pill;if(_dockVisibilityChanged)_dockVisibilityChanged(pill);}
    CGFloat fit=(_pixelWidth&&_pixelHeight)?MIN(area.size.width/_pixelWidth,area.size.height/_pixelHeight):1;
    CGSize size=CGSizeMake(_pixelWidth*fit*_zoom,_pixelHeight*fit*_zoom);
    CGFloat limitX=MAX(0,(size.width-area.size.width)/2),limitY=MAX(0,(size.height-area.size.height)/2);
    _viewportOffset=CGPointMake(MAX(-limitX,MIN(limitX,_viewportOffset.x)),MAX(-limitY,MIN(limitY,_viewportOffset.y)));
    _image.frame=CGRectMake(area.origin.x+(area.size.width-size.width)/2+_viewportOffset.x,area.origin.y+(area.size.height-size.height)/2+_viewportOffset.y,size.width,size.height);
    _metal.frame = _image.frame;
    [self updateCursor];
    if(_keyboardButton){
        CGFloat k=40;UIEdgeInsets safe=self.safeAreaInsets;
        // Hidden before the first frame (nothing to type into) and while the iOS
        // keyboard is open (its Linux-keys bar has its own hide key).
        _keyboardButton.hidden=!_desktopControls||!_pixelWidth||!_pixelHeight||(_softwareKeyboardVisible&&!_relativeTouchpad)||_dockShown;   // the pill has its own keyboard key
        if(usePad)_keyboardButton.frame=CGRectMake(pad.x+pad.w-k,pad.y-k-8,k,k);
        else if(_relativeTouchpad)_keyboardButton.frame=CGRectMake(self.bounds.size.width-MAX(16,safe.right+16)-k,self.bounds.size.height-MAX(16,safe.bottom+8)-k,k,k);
        else _keyboardButton.frame=CGRectMake(self.bounds.size.width-MAX(8,safe.right+8)-k-40,MAX(4,safe.top+4),k,k);
        [self bringSubviewToFront:_keyboardButton];
    }
}
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height {
    [self presentPixels:pixels width:width height:height damage:(LRFrameDamage){0,0,width,height}];
}
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage {
    if (!width || !height || pixels.length != (NSUInteger)width * height * 4) return;
    BOOL resized = _pixelWidth != width || _pixelHeight != height;
    _latestPixels = pixels; _pixelWidth = width; _pixelHeight = height;
    // X servers start with the pointer in the screen centre. Until this view
    // has sent a position, relative input (touchpads) starts from there too,
    // so the first slide does not jump the cursor to the corner.
    if(resized&&!_pointerEverSent){_pointerX=width/2;_pointerY=height/2;_mouseX=_pointerX;_mouseY=_pointerY;}
    if(resized)[self setNeedsLayout];
    if(_presentationSuppressed)return;
    if (_metal) { [_metal presentPixels:pixels width:width height:height damage:damage]; return; }
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)[pixels copy]);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, space, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, provider, NULL, NO, kCGRenderingIntentDefault);
    if (image) { _image.image = [UIImage imageWithCGImage:image]; CGImageRelease(image); }
    CGColorSpaceRelease(space); CGDataProviderRelease(provider);
}
- (void)presentPatch:(NSData *)patch width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage {
    size_t bytes;if(!lr_rfb_dimensions(width,height,&bytes))return;
    if(!damage.x&&!damage.y&&damage.width==width&&damage.height==height) {
        if(patch.length!=bytes)return;
        _patchPixels=patch; // Full immutable frames transfer without extra copies.
    } else {
        if(!_patchPixels||_pixelWidth!=width||_pixelHeight!=height)_patchPixels=[NSMutableData dataWithLength:bytes];
        else if(![_patchPixels isKindOfClass:NSMutableData.class])_patchPixels=[_patchPixels mutableCopy];
        if(!LRApplyFramebufferPatch((NSMutableData *)_patchPixels,patch,width,height,damage))return;
    }
    // Metal synchronously uploads from this main-thread-owned persistent frame.
    // Software CGImage providers retain an immutable snapshot instead.
    [self presentPixels:_patchPixels width:width height:height damage:damage];
}
- (NSDictionary *)presentationStatistics { return _metal ? [_metal presentationStatistics] : @{@"backend":@"Core Graphics"}; }
- (void)clearFrame { _patchPixels=nil; _latestPixels = nil; _image.image = nil; _pixelWidth = _pixelHeight = 0; [_metal clearFrame]; [self setNeedsLayout]; }
- (void)fitViewport{_zoom=1;_viewportOffset=CGPointZero;[self setNeedsLayout];}
- (void)actualViewport{if(_pixelWidth&&_pixelHeight){CGFloat fit=MIN(self.bounds.size.width/_pixelWidth,self.bounds.size.height/_pixelHeight);_zoom=MAX(1,MIN(8,1/fit));_viewportOffset=CGPointZero;[self setNeedsLayout];}}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)a shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)b {
    return (a==_twoFingerScroll&&b==_viewportPinch)||(b==_twoFingerScroll&&a==_viewportPinch);
}
- (BOOL)finishTwoFingerRecognizer:(UIGestureRecognizer *)g other:(UIGestureRecognizer *)other {
    if(g.state!=UIGestureRecognizerStateEnded&&g.state!=UIGestureRecognizerStateCancelled&&g.state!=UIGestureRecognizerStateFailed)return NO;
    if(other.state!=UIGestureRecognizerStateBegan&&other.state!=UIGestureRecognizerStateChanged)_twoFingerIntent=LRTwoFingerUndecided;
    return YES;
}
- (void)pinch:(UIPinchGestureRecognizer *)g{
    if(_relativeTouchpad)return;
    if(g.state==UIGestureRecognizerStateCancelled||g.state==UIGestureRecognizerStateFailed){[self finishTwoFingerRecognizer:g other:_twoFingerScroll];return;}
    if(!isfinite(g.scale)||g.scale<=0){[self finishTwoFingerRecognizer:g other:_twoFingerScroll];return;}
    CGPoint t=[_twoFingerScroll translationInView:self];LRTwoFingerIntent before=_twoFingerIntent;
    _twoFingerIntent=LRChooseTwoFingerIntent(before,t.x,t.y,g.scale);
    if(_twoFingerIntent==LRTwoFingerZoom){
        if(before!=LRTwoFingerZoom)[self releaseInput];
        _zoom=MAX(1,MIN(8,_zoom*g.scale));g.scale=1;[self setNeedsLayout];
    }
    // Consume the final scale/translation before resetting intent at touch-up.
    [self finishTwoFingerRecognizer:g other:_twoFingerScroll];
}

- (BOOL)canBecomeFirstResponder{return YES;}
- (UIView *)inputAccessoryView{return _softwareKeyboardRequested?_keyAccessory:nil;}
- (UIView *)inputView{
    // Keep the guest in the hardware-keyboard responder chain without opening
    // the software keyboard over the desktop. The Keyboard button opts in.
    if(_softwareKeyboardRequested)return nil;
    if(!_hardwareKeyboardInputView)_hardwareKeyboardInputView=[[UIView alloc]initWithFrame:CGRectZero];
    return _hardwareKeyboardInputView;
}
- (void)setSoftwareKeyboardRequested:(BOOL)requested{
    _softwareKeyboardRequested=requested;
    [_keyboardButton setImage:[UIImage systemImageNamed:requested?@"keyboard.chevron.compact.down":@"keyboard"] forState:UIControlStateNormal];
    _keyboardButton.accessibilityLabel=requested?@"Hide Keyboard":@"Keyboard";
    [_dockKeyboardButton setImage:[UIImage systemImageNamed:requested?@"keyboard.chevron.compact.down":@"keyboard" withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:21 weight:UIImageSymbolWeightSemibold]] forState:UIControlStateNormal];
    if(self.isFirstResponder)[self reloadInputViews];
}
- (void)focusKeyboard{
    if(!self.window.isKeyWindow||self.hidden||UIApplication.sharedApplication.applicationState!=UIApplicationStateActive)return;
    // Never take focus from a presented settings page or native text field.
    for(UIResponder *r=self.nextResponder;r;r=r.nextResponder)if([r isKindOfClass:UIViewController.class]){
        UIViewController *vc=(UIViewController *)r;
        if(vc.presentedViewController||vc.navigationController.presentedViewController||
           (vc.navigationController&&vc.navigationController.topViewController!=vc))return;
        break;
    }
    if(!self.isFirstResponder)[self becomeFirstResponder];
}
- (BOOL)resignFirstResponder{
    BOOL hadFocus=self.isFirstResponder;
    BOOL resigned=[super resignFirstResponder];
    if(hadFocus&&resigned){[self releaseInput];_softwareKeyboardRequested=NO;}
    return resigned;
}
- (BOOL)hasText{return YES;}
- (UITextAutocorrectionType)autocorrectionType{return UITextAutocorrectionTypeNo;}
- (UITextAutocapitalizationType)autocapitalizationType{return UITextAutocapitalizationTypeNone;}
- (UITextSmartQuotesType)smartQuotesType{return UITextSmartQuotesTypeNo;}
- (UITextSmartDashesType)smartDashesType{return UITextSmartDashesTypeNo;}
- (UITextSmartInsertDeleteType)smartInsertDeleteType{return UITextSmartInsertDeleteTypeNo;}
- (void)trace:(NSString *)event{if(_traceInput&&_traceCount++<2000)[LRRuntimeBridge logCategory:@"GUI_INPUT" event:event];}
- (void)insertText:(NSString *)text{
    [self countInput:@"keyboard.text"];
    NSMutableData *symbols=[[text dataUsingEncoding:NSUTF32LittleEndianStringEncoding] mutableCopy];uint32_t *codes=symbols.mutableBytes;
    [self syncModifiers];
    for(NSUInteger i=0;i<symbols.length/4;i++){uint32_t c=codes[i];
        uint32_t sym=(c=='\n'||c=='\r')?0xff0d:c=='\t'?0xff09:c==8?0xff08:c<256?c:0x01000000|c;
        if(_traceInput)[self trace:[NSString stringWithFormat:@"iOS text Unicode=U+%04X keysym=0x%08X modifiers=0x%lx",c,sym,(unsigned long)(_stickyModifiers|_hardwareModifiers)]];
        codes[i]=sym;
    }
    [_client typeKeysyms:codes count:symbols.length/4];
}
- (void)deleteBackward{[self stroke:0xff08];}
- (void)stroke:(uint32_t)symbol{[self syncModifiers];[_client key:symbol down:YES];[_client key:symbol down:NO];}
- (void)toggleModifier:(UIKeyModifierFlags)flag{_stickyModifiers^=flag;[self syncModifiers];}
- (void)syncModifiers{
    UIKeyModifierFlags desired=_stickyModifiers|_hardwareModifiers;
    const UIKeyModifierFlags flags[]={UIKeyModifierShift,UIKeyModifierControl,UIKeyModifierAlternate,UIKeyModifierCommand};
    const uint32_t symbols[]={0xffe1,0xffe3,0xffe9,0xffeb};
    for(unsigned i=0;i<4;i++)if((_sentModifiers&flags[i])!=(desired&flags[i]))[_client key:symbols[i] down:(desired&flags[i])!=0];
    _sentModifiers=desired;
}
- (void)releaseInput{
    [self sendPointerFromEventTime:0 buttons:0];_stickyModifiers=0;_hardwareModifiers=0;[self syncModifiers];
    _mouseButtons=0;_pointerTouchButton=0;_pointerTouch=nil;
    for(NSNumber *symbol in _pressedKeys.allValues)[_client key:symbol.unsignedIntValue down:NO];
    [_pressedKeys removeAllObjects];
}
- (uint32_t)symbolForKey:(UIKey *)key{
    switch(key.keyCode){
        case UIKeyboardHIDUsageKeyboardEscape:return 0xff1b;
        case UIKeyboardHIDUsageKeyboardTab:return 0xff09;
        case UIKeyboardHIDUsageKeyboardReturnOrEnter:return 0xff0d;
        case UIKeyboardHIDUsageKeyboardDeleteOrBackspace:return 0xff08;
        case UIKeyboardHIDUsageKeyboardDeleteForward:return 0xffff;
        case UIKeyboardHIDUsageKeyboardRightArrow:return 0xff53;
        case UIKeyboardHIDUsageKeyboardLeftArrow:return 0xff51;
        case UIKeyboardHIDUsageKeyboardDownArrow:return 0xff54;
        case UIKeyboardHIDUsageKeyboardUpArrow:return 0xff52;
        case UIKeyboardHIDUsageKeyboardHome:return 0xff50;
        case UIKeyboardHIDUsageKeyboardEnd:return 0xff57;
        case UIKeyboardHIDUsageKeyboardPageUp:return 0xff55;
        case UIKeyboardHIDUsageKeyboardPageDown:return 0xff56;
        case UIKeyboardHIDUsageKeyboardInsert:return 0xff63;
        case UIKeyboardHIDUsageKeypadEnter:return 0xff8d;
        case UIKeyboardHIDUsageKeyboardPrintScreen:return 0xff61;
        case UIKeyboardHIDUsageKeyboardPause:return 0xff13;
        case UIKeyboardHIDUsageKeyboardScrollLock:return 0xff14;
        case UIKeyboardHIDUsageKeyboardApplication:return 0xff67;   // Menu
        default:break;
    }
    if(key.keyCode>=UIKeyboardHIDUsageKeyboardF1&&key.keyCode<=UIKeyboardHIDUsageKeyboardF12)return 0xffbe + (uint32_t)(key.keyCode-UIKeyboardHIDUsageKeyboardF1);
    if(key.keyCode>=UIKeyboardHIDUsageKeyboardF13&&key.keyCode<=UIKeyboardHIDUsageKeyboardF24)return 0xffca + (uint32_t)(key.keyCode-UIKeyboardHIDUsageKeyboardF13);
    // Cache the down keysym by HID so releasing Shift first cannot send a
    // different key-up. Text uses UIKit's actual shifted/layout-aware symbol;
    // shortcut control codes are mapped back to printable keysyms.
    if(key.keyCode>=UIKeyboardHIDUsageKeyboardLeftControl&&key.keyCode<=UIKeyboardHIDUsageKeyboardRightGUI)return 0;
    unsigned modifiers=0;
    if(key.modifierFlags&UIKeyModifierShift)modifiers|=LR_KEY_SHIFT;
    if(key.modifierFlags&UIKeyModifierControl)modifiers|=LR_KEY_CONTROL;
    if(key.modifierFlags&UIKeyModifierAlternate)modifiers|=LR_KEY_ALT;
    if(key.modifierFlags&UIKeyModifierCommand)modifiers|=LR_KEY_COMMAND;
    return LRHardwareKeySym(key.characters,key.charactersIgnoringModifiers,modifiers);
}
- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event{
    NSMutableSet *unhandled=[presses mutableCopy];
    for(UIPress *p in presses)if(p.key){UIKey *key=p.key;_hardwareModifiers=key.modifierFlags;
        unsigned own=LRModifierForHID((unsigned)key.keyCode);
        if(own==LR_KEY_SHIFT)_hardwareModifiers|=UIKeyModifierShift;
        if(own==LR_KEY_CONTROL)_hardwareModifiers|=UIKeyModifierControl;
        if(own==LR_KEY_ALT)_hardwareModifiers|=UIKeyModifierAlternate;
        if(own==LR_KEY_COMMAND)_hardwareModifiers|=UIKeyModifierCommand;
        [self syncModifiers];
        [self countInput:@"keyboard.hardware.down"];
        NSNumber *held=_pressedKeys[@(key.keyCode)];
        uint32_t symbol=[self symbolForKey:key];
        // A repeat may change case/layout after a modifier is released.
        // Release the old keysym before replacing it; otherwise either the
        // original key stays down or repeats keep an obsolete character.
        if(held&&held.unsignedIntValue!=symbol){[_client key:held.unsignedIntValue down:NO];[_pressedKeys removeObjectForKey:@(key.keyCode)];}
        if(symbol){_pressedKeys[@(key.keyCode)]=@(symbol);[_client key:symbol down:YES];}
        [self trace:[NSString stringWithFormat:@"hardware HID=%ld down=1 keysym=0x%08X modifiers=0x%lx",(long)key.keyCode,symbol,(unsigned long)_hardwareModifiers]];
        if(symbol||key.keyCode>=UIKeyboardHIDUsageKeyboardLeftControl)[unhandled removeObject:p];
    }
    if(unhandled.count)[super pressesBegan:unhandled withEvent:event];
}
- (void)pressesEnded:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event{
    NSMutableSet *unhandled=[presses mutableCopy];
    for(UIPress *p in presses)if(p.key){UIKey *key=p.key;NSNumber *cached=_pressedKeys[@(key.keyCode)];
        [self countInput:@"keyboard.hardware.up"];
        uint32_t symbol=cached?cached.unsignedIntValue:[self symbolForKey:key];
        if(symbol){[_client key:symbol down:NO];[_pressedKeys removeObjectForKey:@(key.keyCode)];}
        _hardwareModifiers=key.modifierFlags;[self syncModifiers];
        [self trace:[NSString stringWithFormat:@"hardware HID=%ld down=0 keysym=0x%08X modifiers=0x%lx",(long)key.keyCode,symbol,(unsigned long)_hardwareModifiers]];
        if(symbol||key.keyCode>=UIKeyboardHIDUsageKeyboardLeftControl)[unhandled removeObject:p];
    }
    if(unhandled.count)[super pressesEnded:unhandled withEvent:event];
}
- (void)pressesCancelled:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event{[self releaseInput];[super pressesCancelled:presses withEvent:event];}
// ---- External mouse ---------------------------------------------------------
// On iPhone iOS gives a mouse no system pointer (only AssistiveTouch, whose
// clicks arrive as touches). GCMouse (public GameController API) reports the
// mouse itself: relative motion moves the Linux pointer, buttons press/release
// (left 1, middle 2, right 4) and the wheel scrolls (buttons 4/5 in X). Taps
// and drags that AssistiveTouch synthesizes for the same click are ignored.
- (void)observeMice{
    __weak typeof(self) weak=self;
    [_inputObservers addObject:[NSNotificationCenter.defaultCenter addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){[weak attachMouse:n.object];}]];
    for(GCMouse *mouse in GCMouse.mice)[self attachMouse:mouse];
}
- (void)attachMouse:(GCMouse *)mouse{
    if(![mouse isKindOfClass:GCMouse.class]||!mouse.mouseInput)return;
    mouse.handlerQueue=dispatch_get_main_queue();
    __weak typeof(self) weak=self;GCMouseInput *input=mouse.mouseInput;
    input.mouseMovedHandler=^(GCMouseInput *m,float dx,float dy){(void)m;[weak mouseMovedX:dx y:dy];};
    input.leftButton.pressedChangedHandler=^(GCControllerButtonInput *b,float v,BOOL pressed){(void)b;(void)v;[weak gcButton:LR_POINTER_LEFT bit:1 pressed:pressed];};
    input.middleButton.pressedChangedHandler=^(GCControllerButtonInput *b,float v,BOOL pressed){(void)b;(void)v;[weak gcButton:LR_POINTER_MIDDLE bit:2 pressed:pressed];};
    input.rightButton.pressedChangedHandler=^(GCControllerButtonInput *b,float v,BOOL pressed){(void)b;(void)v;[weak gcButton:LR_POINTER_RIGHT bit:4 pressed:pressed];};
    input.scroll.valueChangedHandler=^(GCControllerDirectionPad *pad,float x,float y){(void)pad;(void)x;[weak mouseWheel:y];};
    [self countInput:@"gc.connected"];[self refreshPointerMode];
    [LRRuntimeBridge logCategory:@"GUI_INPUT" event:[@"external mouse connected: " stringByAppendingString:mouse.vendorName?:@"mouse"]];
}
- (BOOL)mouseActive{return _pointerModeActive;}
- (LRPointerEnvironment)pointerEnvironment{
    NSString *mode=[NSUserDefaults.standardUserDefaults stringForKey:@"LinuxRunnerInput.mouseMode"];
    return (LRPointerEnvironment){.mode=[mode isEqual:@"touch"]?LR_MOUSE_MODE_TOUCH:[mode isEqual:@"pointer"]?LR_MOUSE_MODE_POINTER:LR_MOUSE_MODE_AUTO,
        .assistive_touch=UIAccessibilityIsAssistiveTouchRunning(),.gc_mouse=GCMouse.mice.count>0,.indirect_pointer=_sawIndirectPointer,.hover=_sawHover,
        .uikit_pointer_platform=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad};
}
// Pointer mode: single-finger gestures off (no tap/long-press delays); a touch
// is a mouse button press at its exact position. Two-finger scroll and zoom stay.
- (void)refreshPointerMode{
    LRPointerEnvironment e=[self pointerEnvironment];BOOL active=lr_pointer_mode(e);
    _tapGesture.enabled=_dragGesture.enabled=_secondaryGesture.enabled=!active||_relativeTouchpad;
    for(UIGestureRecognizer *g in @[_tapGesture,_dragGesture,_secondaryGesture])
        g.allowedTouchTypes=_relativeTouchpad?@[@(UITouchTypeDirect),@(UITouchTypePencil)]:@[@(UITouchTypeDirect),@(UITouchTypeIndirect),@(UITouchTypePencil),@(UITouchTypeIndirectPointer)];
    if(active!=_pointerModeActive){_pointerModeActive=active;
        [LRRuntimeBridge desktopTrace:@"input" result:@"info" detail:[NSString stringWithFormat:@"pointer mode=%@ assistive-touch=%d gc-mice=%lu indirect-pointer=%d hover=%d setting=%d",active?@"on":@"off",e.assistive_touch,(unsigned long)GCMouse.mice.count,e.indirect_pointer,e.hover,e.mode]];}
}
- (void)countInput:(NSString *)name{
    NSNumber *n=_inputCounts[name];_inputCounts[name]=@(n.unsignedLongValue+1);
    if(!n)[LRRuntimeBridge desktopTrace:@"input" result:@"info" detail:[@"first event: " stringByAppendingString:name]];
    _lastInputSource=name;
}
// Event-to-send latency: iOS event timestamp to the pointer message being queued.
- (void)sendPointerFromEventTime:(NSTimeInterval)eventTime buttons:(uint8_t)buttons{
    if(!_client)return;
    uint8_t changed=(buttons^_lastSentPointerButtons)&7;
    if((buttons&~_lastSentPointerButtons)&7){
        _pressSentAt=CACurrentMediaTime();
        // Selecting a Linux window also routes the hardware keyboard to its
        // native display view. Keep touch-only use behind the Keyboard button.
        if(GCKeyboard.coalescedKeyboard&&self.window&&!self.isFirstResponder){
            BOOL focused=[self becomeFirstResponder];
            [self countInput:focused?@"keyboard.focus.acquired":@"keyboard.focus.failed"];
        }
    }
    for(unsigned bit=1;bit<=4;bit<<=1)if(changed&bit)[self countInput:[NSString stringWithFormat:@"x11.button%u.%@",bit,buttons&bit?@"down":@"up"]];
    _lastSentPointerButtons=buttons;
    _pointerEverSent=YES;
    [_client pointerX:_pointerX y:_pointerY buttons:buttons];[self countInput:@"x11.pointer-sent"];[self updateCursor];
    if(eventTime>0){double ms=(CACurrentMediaTime()-eventTime)*1000;if(ms>=0&&ms<5000){_latencySum+=ms;_latencyCount++;_latencyMax=MAX(_latencyMax,ms);}}
}
- (NSDictionary *)inputDiagnostics{
    LRPointerEnvironment e=[self pointerEnvironment];NSMutableArray *mice=[NSMutableArray new];
    for(GCMouse *m in GCMouse.mice)[mice addObject:m.vendorName?:@"mouse"];
    return @{@"software_keyboard_requested":@(_softwareKeyboardRequested),@"pointer_mode":_pointerModeActive?@"on":@"off",@"mode_setting":@(e.mode),@"assistive_touch":@(e.assistive_touch),@"gc_mice":mice,
        @"hardware_keyboard":@(GCKeyboard.coalescedKeyboard!=nil),@"keyboard_first_responder":@(self.isFirstResponder),
        @"indirect_pointer_seen":@(_sawIndirectPointer),@"hover_seen":@(_sawHover),@"counts":_inputCounts.copy,@"last_source":_lastInputSource?:@"none",
        @"pointer":[NSString stringWithFormat:@"%u,%u buttons=%u",_pointerX,_pointerY,_mouseButtons|_pointerTouchButton],
        @"press_to_frame_ms":_displayCount?[NSString stringWithFormat:@"avg %.1f max %.1f n=%lu",_displaySum/_displayCount,_displayMax,(unsigned long)_displayCount]:@"none",
        @"latency_ms":_latencyCount?[NSString stringWithFormat:@"avg %.1f max %.1f n=%lu",_latencySum/_latencyCount,_latencyMax,(unsigned long)_latencyCount]:@"none"};
}
- (void)noteFramePresented{
    if(_pressSentAt<=0)return;double ms=(CACurrentMediaTime()-_pressSentAt)*1000;_pressSentAt=0;
    if(ms<5000){_displaySum+=ms;_displayCount++;_displayMax=MAX(_displayMax,ms);}
}
- (UIPointerStyle *)pointerInteraction:(UIPointerInteraction *)interaction styleForRegion:(UIPointerRegion *)region{return [UIPointerStyle hiddenPointerStyle];}
// ---- Pointer-mode touches (mouse through UIKit) ------------------------------
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    [self focusKeyboard];
    for(UITouch *t in touches)[self countInput:t.type==UITouchTypeIndirectPointer?@"uikit.touch.indirect-pointer":t.type==UITouchTypeDirect?@"uikit.touch.direct":t.type==UITouchTypePencil?@"uikit.touch.pencil":@"uikit.touch.indirect"];
    UITouch *t=touches.anyObject;_lastTouchDown=t.timestamp;
    if(t.type==UITouchTypeIndirectPointer&&!_sawIndirectPointer){_sawIndirectPointer=YES;[self refreshPointerMode];}
    if(_pointerModeActive&&(!_relativeTouchpad||t.type==UITouchTypeIndirectPointer)&&!_pointerTouch&&event.allTouches.count==1&&[self position:[t locationInView:self]]){
        BOOL indirect=t.type==UITouchTypeIndirectPointer;
        BOOL secondary=indirect&&(event.buttonMask&UIEventButtonMaskSecondary);
        BOOL ownButton=lr_pointer_use_uikit_button([self pointerEnvironment],indirect,secondary?LR_POINTER_RIGHT:LR_POINTER_LEFT);
        _pointerTouch=t;_pointerTouchButton=ownButton?(uint8_t)lr_pointer_touch_button(secondary):0;
        [self sendPointerFromEventTime:t.timestamp buttons:_mouseButtons];                      // move first
        [self sendPointerFromEventTime:t.timestamp buttons:_mouseButtons|_pointerTouchButton];  // then press
        return;
    }
    [super touchesBegan:touches withEvent:event];
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    if(_pointerTouch&&[touches containsObject:_pointerTouch]){
        if(event.allTouches.count>1){[self endPointerTouch:_pointerTouch];return;}   // a second finger: hand over to scroll/zoom
        if([self position:[_pointerTouch locationInView:self]])[self sendPointerFromEventTime:_pointerTouch.timestamp buttons:_mouseButtons|_pointerTouchButton];
        return;
    }
    [super touchesMoved:touches withEvent:event];
}
- (void)endPointerTouch:(UITouch *)t{
    if(t!=_pointerTouch)return;
    [self position:[t locationInView:self]];_pointerTouch=nil;_pointerTouchButton=0;
    [self sendPointerFromEventTime:t.timestamp buttons:_mouseButtons];
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    if(_pointerTouch&&[touches containsObject:_pointerTouch]){[self endPointerTouch:_pointerTouch];return;}
    [super touchesEnded:touches withEvent:event];
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event{
    if(_pointerTouch&&[touches containsObject:_pointerTouch]){[self endPointerTouch:_pointerTouch];return;}
    [super touchesCancelled:touches withEvent:event];
}
- (BOOL)gcPressTargetsSurface {
    if(!self.window.isKeyWindow||self.hidden||!_pixelWidth||!_pixelHeight)return NO;
    // Global GC callbacks must not click through a native settings sheet.
    for(UIResponder *r=self.nextResponder;r;r=r.nextResponder)if([r isKindOfClass:UIViewController.class]){
        UIViewController *controller=(UIViewController *)r;
        if(controller.presentedViewController||controller.isBeingDismissed)return NO;
    }
    CGRect image=_image.frame;
    CGPoint point=CGPointMake(image.origin.x+(CGFloat)_pointerX/_pixelWidth*image.size.width,
                             image.origin.y+(CGFloat)_pointerY/_pixelHeight*image.size.height);
    if(!CGRectContainsPoint(self.bounds,point))return NO;
    UIView *hit=[self.window hitTest:[self convertPoint:point toView:self.window] withEvent:nil];
    return hit==self||[hit isDescendantOfView:self];
}
- (void)gcButton:(int)button bit:(uint8_t)bit pressed:(BOOL)pressed{
    [self countInput:[NSString stringWithFormat:@"gc.button%d.%@",button,pressed?@"down":@"up"]];
    if(!lr_pointer_forward_gc_button([self pointerEnvironment],button,pressed,(_mouseButtons&bit)!=0,
                                    pressed?[self gcPressTargetsSurface]:NO)){
        [self countInput:@"gc.button.ignored-uikit-or-native-target"];return;
    }
    [self mouseButton:bit pressed:pressed];
}
- (void)sendMouse{if(_pixelWidth&&_pixelHeight)[self sendPointerFromEventTime:GCMouse.current.mouseInput.lastEventTimestamp buttons:_mouseButtons|_pointerTouchButton];}
- (void)mouseMovedX:(float)dx y:(float)dy{
    [self countInput:@"gc.motion"];
    if(!lr_pointer_use_gc_motion([self pointerEnvironment])){return;}
    if(!_pixelWidth||!_pixelHeight)return;
    // Device points to framebuffer pixels at the current zoom; GCMouse reports
    // upward motion as positive y.
    CGFloat scale=(_image.frame.size.width>0?_pixelWidth/_image.frame.size.width:1)*lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.mouseGain"],1);
    if(_mouseX==0&&_mouseY==0){_mouseX=_pointerX;_mouseY=_pointerY;}
    _mouseX=MAX(0,MIN(_pixelWidth-1,_mouseX+dx*scale));_mouseY=MAX(0,MIN(_pixelHeight-1,_mouseY-dy*scale));
    _pointerX=(uint16_t)_mouseX;_pointerY=(uint16_t)_mouseY;[self sendMouse];
}
- (void)mouseButton:(uint8_t)bit pressed:(BOOL)pressed{
    if(pressed)[self focusKeyboard];
    _lastMouseButtonTime=CACurrentMediaTime();
    if(pressed)_mouseButtons|=bit;else _mouseButtons&=~bit;
    [self sendMouse];
}
- (void)mouseWheel:(float)y{
    [self countInput:@"gc.scroll"];_lastGCScroll=CACurrentMediaTime();
    _mouseScroll+=y*lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.scrollGain"],1);
    while(fabs(_mouseScroll)>=1){uint8_t wheel=_mouseScroll>0?8:16;_mouseScroll+=_mouseScroll>0?-1:1;
        [_client pointerX:_pointerX y:_pointerY buttons:_mouseButtons|wheel];[_client pointerX:_pointerX y:_pointerY buttons:_mouseButtons];}
}
- (BOOL)position:(CGPoint)p{
    if(!_pixelWidth||!_pixelHeight||self.bounds.size.width<=0||self.bounds.size.height<=0)return NO;
    CGRect r=_image.frame;uint16_t x,y;
    if(!lr_rfb_viewport_point(p.x,p.y,r.origin.x,r.origin.y,r.size.width,r.size.height,_pixelWidth,_pixelHeight,&x,&y))return NO;
    _pointerX=x;_pointerY=y;_mouseX=x;_mouseY=y;return YES;
}
// Touch-gesture clicks: latency counted from the finger-down time, so recognizer waits show up.
- (void)click:(uint8_t)button at:(CGPoint)p{if(_relativeTouchpad||(CGRectContainsPoint(_image.frame,p)&&[self position:p])){[self focusKeyboard];[self sendPointerFromEventTime:_lastTouchDown buttons:button];[self sendPointerFromEventTime:0 buttons:0];}}
- (void)tap:(UITapGestureRecognizer *)g{if([self mouseActive]&&!_relativeTouchpad)return;[self click:1 at:[g locationInView:self]];}
- (void)drag:(UIPanGestureRecognizer *)g{
    if(_relativeTouchpad){CGPoint t=[g translationInView:self];[g setTranslation:CGPointZero inView:self];[self relativeMoveX:t.x y:t.y];return;}
    if([self mouseActive])return;if([self position:[g locationInView:self]]){[_client pointerX:_pointerX y:_pointerY buttons:(g.state==UIGestureRecognizerStateBegan||g.state==UIGestureRecognizerStateChanged)?1:0];[self updateCursor];}
}
- (void)secondary:(UILongPressGestureRecognizer *)g{
    if(_relativeTouchpad){CGPoint p=[g locationInView:self];
        if(g.state==UIGestureRecognizerStateBegan){_touchpadDragPoint=p;_pointerTouchButton=1;[self relativeMoveX:0 y:0];}
        else if(g.state==UIGestureRecognizerStateChanged){[self relativeMoveX:p.x-_touchpadDragPoint.x y:p.y-_touchpadDragPoint.y];_touchpadDragPoint=p;}
        else{_pointerTouchButton=0;[self relativeMoveX:0 y:0];}return;
    }
    if(!_secondaryTwoFinger&&g.state==UIGestureRecognizerStateBegan)[self click:4 at:[g locationInView:self]];
}
- (void)twoTap:(UITapGestureRecognizer *)g{if(_relativeTouchpad||_secondaryTwoFinger)[self click:4 at:[g locationInView:self]];}
- (void)scroll:(UIPanGestureRecognizer *)g{
    if(_relativeTouchpad){CGPoint t=[g translationInView:self];if(fabs(t.y)>=12/lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.scrollGain"],1)){[self click:t.y<0?16:8 at:CGPointZero];[g setTranslation:CGPointZero inView:self];}return;}
    if(g==_twoFingerScroll){
        if(g.state==UIGestureRecognizerStateCancelled||g.state==UIGestureRecognizerStateFailed){[self finishTwoFingerRecognizer:g other:_viewportPinch];return;}
        CGPoint t=[g translationInView:self];LRTwoFingerIntent before=_twoFingerIntent;_twoFingerIntent=LRChooseTwoFingerIntent(before,t.x,t.y,_viewportPinch.scale);
        if(before!=LRTwoFingerZoom&&_twoFingerIntent==LRTwoFingerZoom)[self releaseInput];
        if(_twoFingerIntent!=LRTwoFingerScroll){[self finishTwoFingerRecognizer:g other:_viewportPinch];return;}
    }
    CGPoint t=[g translationInView:self];if(_zoom>1.01&&g.allowedScrollTypesMask==0){_viewportOffset=CGPointMake(_viewportOffset.x+t.x,_viewportOffset.y+t.y);[g setTranslation:CGPointZero inView:self];[self setNeedsLayout];}else if(fabs(t.y)>=12/lr_input_gain([NSUserDefaults.standardUserDefaults doubleForKey:@"LinuxRunnerInput.scrollGain"],1)){[self click:t.y<0?16:8 at:[g locationInView:self]];[g setTranslation:CGPointZero inView:self];}
    if(g==_twoFingerScroll)[self finishTwoFingerRecognizer:g other:_viewportPinch];
}
- (void)wheel:(UIPanGestureRecognizer *)g{[self countInput:@"uikit.scroll"];if(!lr_pointer_use_uikit_wheel(CACurrentMediaTime(),_lastGCScroll))return;[self scroll:g];}
- (void)hover:(UIHoverGestureRecognizer *)g{[self countInput:@"uikit.hover"];if(!_sawHover){_sawHover=YES;[self refreshPointerMode];}
    if(!_pointerTouch&&[self position:[g locationInView:self]])[self sendPointerFromEventTime:CACurrentMediaTime() buttons:_mouseButtons];}
- (void)rightMouse:(UITapGestureRecognizer *)g{[self countInput:@"uikit.secondary-click"];if(_pointerModeActive)return;[self click:4 at:[g locationInView:self]];}
@end

@interface LRGUIViewController ()
@property LRFramebufferView *surface;
@property UILabel *status;
@property UIStackView *stack;
@property TerminalViewController *sessionTerminal;
@property LRRFBClient *client;
@property NSURL *supportURL,*sessionURL,*installStatusURL;
@property TerminalViewController *installerTerminal;
@property NSLock *frameLock;
@property NSData *pendingFrame;
@property uint64_t patchBytes,presentationPatchBytes;
@property LRFrameDamage pendingDamage;
@property uint16_t pendingWidth,pendingHeight;
@property BOOL presentationPending;
@property double pendingFrameReceivedAt;
@property NSUInteger transportDiagnosticEvents;
@property uint64_t frames,bytes;
// Display updates per second over the last status refresh (RFB updates delivered
// to iOS; not the compositor frame rate, which the guest does not report here).
@property uint64_t rateFrames;@property double rateTime,updateRate;
// Viewer reconnections after the display transport restarted (session alive).
@property NSInteger viewerReconnects;@property NSDate *viewerReconnectWindow;
@property BOOL applicationWindowMapped;
@property NSDate *startedAt;
@property id exitObserver;
@property NSTimer *metricsTimer;
@property NSTimer *restartTimer;
@property NSDictionary *launchConfiguration;
@property BOOL autoLaunchPending;
@property NSMutableArray<UIButton *> *modifierButtons;
@property NSArray<UIView *> *launchControls;
@property UISegmentedControl *resolution;
@property BOOL desktopMode;
@property NSTimer *queuedLaunchTimer; // retries a launch request held while the desktop starts
@property BOOL desktopPainted;
// Input round trip: the app moved the pointer over RFB and the X server
// reported that same position (desktop-probe --pointer). 0 not yet, 1 verified,
// -1 could not be verified (desktop shown as limited, never as fully ready).
@property NSInteger inputVerified;
@property NSInteger inputChecks;
@property uint16_t inputProbeX,inputProbeY;
@property CFTimeInterval inputProbeAt;
@property BOOL traceFirstUpdate;
@property NSMutableSet<NSString *> *traceSessionMarks;
@property UIStackView *inputToolbar;
@property UIButton *immersiveButton;
@property NSDictionary *queuedDesktopApplication;
@property NSString *lastLaunchRequest;
@property (copy) NSString *sessionGuestUser;
@property NSMutableArray<UIView *> *compactControls;
@property(nonatomic) BOOL immersive;   // setter: -setImmersive:
@property UIButton *dexButton;
@property UIButton *dockDexButton;
@property UIMenu *powerMenu;
@property UIButton *dockFoldButton;
@property(copy) NSString *linuxClipboard;   // last text Linux put on its CLIPBOARD (RFB ServerCutText)
@property UIButton *moreButton;
// Desktop startup coordinator (Open Desktop): check in Linux → repair → session.
@property NSInteger startState;            // LRStart*
@property BOOL profileReady;               // this controller checked the profile since the last failure
@property BOOL repairAttempted;            // one automatic repair per Open Desktop request
@property TerminalViewController *profileTerminal; // headless check/repair process
@property NSTimer *profileTimer;
@property NSDate *profileStarted;
@property NSString *startFailure;
@property UIView *startPanel;
@property UILabel *startTitle,*startDetail;
@property UIActivityIndicatorView *startSpinner;
@property UIStackView *startButtons;
@property LRAudioBridge *audio; // session sound; lives as long as the session, not the viewer
@property uint16_t audioPort;
@property BOOL viewerVisible;
@property NSUUID *storageActivitySource;
@property BOOL trackpadRequested;
- (void)updateStorageActivity;
@end
enum { LRStartIdle, LRStartChecking, LRStartRepairing, LRStartSession, LRStartFailed };
// ---- External display (second screen) ----------------------------------
// A monitor connected to the iPhone/iPad gets its own window scene showing the
// Linux desktop (not mirroring the iOS interface). Frames come from the desktop
// controller's framebuffer after each update; input stays on the device (touch,
// keyboard, mouse, trackpad). With "Fit Screen", the X display is sized to the
// external screen. The scene is non-interactive by iOS design.
NSNotificationName const LRExternalDisplayChangedNotification = @"LRExternalDisplayChanged";
NSString *const LRExternalDisplayEnabledKey = @"LinuxRunnerDesktop.externalDisplay";
static BOOL LRExternalOutputEnabled(void) {
    id value=[NSUserDefaults.standardUserDefaults objectForKey:LRExternalDisplayEnabledKey];
    return !value || [value boolValue]; // Preserve the existing automatic output default.
}
@interface LRExternalDisplayController : UIViewController <LRCursorDisplay>
@property LRCursorOverlay *cursor;
@property uint16_t cursorX,cursorY;
@property(nullable) LRMetalFramebufferView *metal;
@property UIImageView *image;
@property UILabel *placeholder;
@property uint16_t pixelWidth,pixelHeight;
@property NSData *pendingPixels;
@property LRFrameDamage pendingDamage;
@property BOOL frameScheduled;
@property NSUInteger frameGeneration;
@property double lastPresentationTime;
- (double)presentationDelayAt:(double)now;
- (void)presentFrame:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage;
- (void)showPlaceholder;
@end
static __weak LRExternalDisplayController *LRCurrentExternalDisplay;
static id<LRCursorDisplay> LRExternalCursorDisplay(void){return LRCurrentExternalDisplay;}
@implementation LRExternalDisplayController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor=UIColor.blackColor;
    _image=[UIImageView new];_image.contentMode=UIViewContentModeScaleToFill;_image.layer.magnificationFilter=kCAFilterNearest;[self.view addSubview:_image];
    __weak typeof(self) weak=self;
    _metal=[[LRMetalFramebufferView alloc] initWithFailure:^(NSString *reason){typeof(self) self=weak;if(!self)return;[self.metal removeFromSuperview];self.metal=nil;
        [LRRuntimeBridge logCategory:@"EXTERNAL_DISPLAY" event:[@"Metal failed; Core Graphics fallback: " stringByAppendingString:reason]];}];
    if(_metal)[self.view addSubview:_metal];
    _cursor=[LRCursorOverlay new];[self.view.layer addSublayer:_cursor.layer];
    _placeholder=[UILabel new];_placeholder.numberOfLines=0;_placeholder.textAlignment=NSTextAlignmentCenter;_placeholder.textColor=UIColor.lightGrayColor;
    _placeholder.font=[UIFont systemFontOfSize:28 weight:UIFontWeightMedium];
    _placeholder.text=@"LinuxRunner\n\nOpen the Linux desktop on your iPhone or iPad to show it on this display.";
    [self.view addSubview:_placeholder];

}
- (void)viewDidAppear:(BOOL)animated{[super viewDidAppear:animated];[NSNotificationCenter.defaultCenter postNotificationName:LRExternalDisplayChangedNotification object:self];}
- (void)dealloc{if(LRCurrentExternalDisplay==self)LRCurrentExternalDisplay=nil;}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    CGRect b=self.view.bounds;_placeholder.frame=CGRectInset(b,b.size.width*0.1,0);
    if(!_pixelWidth||!_pixelHeight)return;
    double s=MIN(b.size.width/_pixelWidth,b.size.height/_pixelHeight);CGSize z=CGSizeMake(_pixelWidth*s,_pixelHeight*s);
    CGRect f=CGRectMake((b.size.width-z.width)/2,(b.size.height-z.height)/2,z.width,z.height);_image.frame=f;_metal.frame=f;
    [self placeCursorX:_cursorX y:_cursorY];
}
- (void)setCursorShape:(NSData *)bgra width:(uint16_t)w height:(uint16_t)h hotX:(uint16_t)x hotY:(uint16_t)y{[_cursor setShape:bgra width:w height:h hotX:x hotY:y];[self placeCursorX:_cursorX y:_cursorY];}
- (void)placeCursorX:(uint16_t)x y:(uint16_t)y{
    _cursorX=x;_cursorY=y;
    [_cursor placeInFrame:_image.frame pixelWidth:_pixelWidth pixelHeight:_pixelHeight x:x y:y hidden:!_placeholder.hidden||!LRExternalOutputEnabled()];
}
- (double)presentationDelayAt:(double)now {
    // Retain the 30 Hz cap, but do not delay the first update after idle.
    return _lastPresentationTime ? MAX(0, 1.0/30.0-(now-_lastPresentationTime)) : 0;
}
- (void)presentFrame:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage {
    if(!LRExternalOutputEnabled())return;
    size_t bytes;if(!lr_rfb_dimensions(width,height,&bytes)||pixels.length!=bytes)return;
    BOOL resized=width!=_pixelWidth||height!=_pixelHeight;
    if(resized){_pixelWidth=width;_pixelHeight=height;damage=(LRFrameDamage){0,0,width,height};_pendingDamage=(LRFrameDamage){0};[self.view setNeedsLayout];}
    // Main-thread-owned backing storage. Only a Core Graphics image needs an
    // immutable copy, at presentation time. Coalescing must retain ALL damage
    // and flush the final frame even if no more updates arrive.
    _pendingPixels=pixels;_pendingDamage=lr_damage_union(_pendingDamage,damage);
    if(_frameScheduled)return;
    _frameScheduled=YES;NSUInteger generation=_frameGeneration;
    __weak typeof(self) weak=self;
    double delay=[self presentationDelayAt:LRGUIClock()];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(delay*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        typeof(self) self=weak;if(!self||self.frameGeneration!=generation)return;
        self.frameScheduled=NO;
        if(!LRExternalOutputEnabled()){[self showPlaceholder];return;}
        NSData *pixels=self.pendingPixels;LRFrameDamage dirty=self.pendingDamage;
        self.pendingPixels=nil;self.pendingDamage=(LRFrameDamage){0};
        if(!pixels)return;
        self.lastPresentationTime=LRGUIClock();
        self.placeholder.hidden=YES;
        if(self.metal){self.image.hidden=YES;[self.metal presentPixels:pixels width:self.pixelWidth height:self.pixelHeight damage:dirty];return;}
        self.image.hidden=NO;
        CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)[pixels copy]);CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
        CGImageRef cg=CGImageCreate(self.pixelWidth,self.pixelHeight,8,32,self.pixelWidth*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,NO,kCGRenderingIntentDefault);
        if(cg){self.image.image=[UIImage imageWithCGImage:cg];CGImageRelease(cg);}
        CGColorSpaceRelease(space);CGDataProviderRelease(provider);
    });
}
- (void)showPlaceholder{
    _frameGeneration++;_frameScheduled=NO;_pendingPixels=nil;_pendingDamage=(LRFrameDamage){0};_lastPresentationTime=0;
    _placeholder.text=LRExternalOutputEnabled()?@"LinuxRunner\n\nOpen a Linux desktop to show it on this display.":@"LinuxRunner\n\nExternal Desktop Output is off. Enable it in Settings › Devices.";
    _placeholder.hidden=NO;_image.hidden=YES;_image.image=nil;_pixelWidth=_pixelHeight=0;[_metal clearFrame];_cursor.layer.hidden=YES;
}
@end

@interface LRExternalDisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong,nullable) UIWindow *window;
@property(weak) UIWindowScene *outputScene;
@property(nonatomic,strong) id preferenceObserver;
@end
@implementation LRExternalDisplaySceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)connectionOptions {
    if(![scene isKindOfClass:UIWindowScene.class])return;
    _outputScene=(UIWindowScene *)scene;
    __weak typeof(self) weak=self;
    _preferenceObserver=[NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){[weak refreshOutput];}];
    [self refreshOutput];
}
- (void)refreshOutput {
    if(!LRExternalOutputEnabled()){[self hideOutput];return;}
    if(!_outputScene||_window)return;
    _window=[[UIWindow alloc] initWithWindowScene:_outputScene];
    _window.rootViewController=[LRExternalDisplayController new];
    LRCurrentExternalDisplay=(LRExternalDisplayController *)_window.rootViewController;
    [LRRuntimeBridge logCategory:@"EXTERNAL_DISPLAY" event:@"external window scene connected (public UIKit API)"];
    _window.hidden=NO;   // never key: keyboard and pointer input stay with the device
    [NSNotificationCenter.defaultCenter postNotificationName:LRExternalDisplayChangedNotification object:nil];
}
- (void)hideOutput {
    if(!_window)return;
    // Decline external content when disabled. Legacy iOS can mirror the phone
    // instead of being covered by a blank window; iOS 27 disables its accessory.
    [(LRExternalDisplayController *)_window.rootViewController showPlaceholder];
    if(LRCurrentExternalDisplay==_window.rootViewController)LRCurrentExternalDisplay=nil;
    _window.hidden=YES;_window=nil;
    dispatch_async(dispatch_get_main_queue(),^{[NSNotificationCenter.defaultCenter postNotificationName:LRExternalDisplayChangedNotification object:nil];});
}
- (void)sceneDidDisconnect:(UIScene *)scene {
    [LRRuntimeBridge logCategory:@"EXTERNAL_DISPLAY" event:@"disconnected"];
    [self hideOutput];_outputScene=nil;
    if(_preferenceObserver)[NSNotificationCenter.defaultCenter removeObserver:_preferenceObserver];_preferenceObserver=nil;
}
- (void)dealloc {if(_preferenceObserver)[NSNotificationCenter.defaultCenter removeObserver:_preferenceObserver];}
@end

// iOS 27 only creates cable/AirPlay noninteractive scenes for a registered
// accessory. Own it on the persistent native workspace, not the dismissible
// desktop viewer. iOS 15–26 continue using the existing scene manifest.
static char LRExternalAccessoryKey;
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000 && !TARGET_OS_MACCATALYST
API_AVAILABLE(ios(27.0))
@interface LRExternalAccessoryOwner : NSObject
@property(nonatomic,strong) UISceneAccessoryRegistration *registration;
@property(nonatomic,strong) id observer;
- (instancetype)initWithController:(UIViewController *)controller;
@end
@implementation LRExternalAccessoryOwner
- (instancetype)initWithController:(UIViewController *)controller {
    if((self=[super init])) {
        UISceneConfiguration *configuration=[[UISceneConfiguration alloc]initWithName:@"Linux Desktop" sessionRole:UIWindowSceneSessionRoleExternalDisplayNonInteractive];
        configuration.delegateClass=LRExternalDisplaySceneDelegate.class;
        _registration=[controller registerSceneAccessory:[UISceneAccessory externalNonInteractiveSceneAccessoryWithConfiguration:configuration]];
        _registration.enabled=LRExternalOutputEnabled();
        __weak typeof(self) weak=self;
        _observer=[NSNotificationCenter.defaultCenter addObserverForName:NSUserDefaultsDidChangeNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){weak.registration.enabled=LRExternalOutputEnabled();}];
        [LRRuntimeBridge logCategory:@"EXTERNAL_DISPLAY" event:@"iOS 27 external-display accessory registered for cable or AirPlay"];
    }return self;
}
- (void)dealloc {if(_observer)[NSNotificationCenter.defaultCenter removeObserver:_observer];}
@end
#endif

#if TARGET_OS_SIMULATOR
#include "../../tests/linuxrunner/gui/external_display_contract.inc"
#endif
static NSString *DesktopDegradedNotice(NSString *state);
@implementation LRGUIViewController
+ (void)registerExternalDisplayForController:(UIViewController *)controller {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 270000 && !TARGET_OS_MACCATALYST
    if(@available(iOS 27.0,*)) {
        if(!objc_getAssociatedObject(controller,&LRExternalAccessoryKey)) {
            LRExternalAccessoryOwner *owner=[[LRExternalAccessoryOwner alloc]initWithController:controller];
            objc_setAssociatedObject(controller,&LRExternalAccessoryKey,owner,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
    }
#endif
}
#if TARGET_OS_SIMULATOR
+ (void)runExternalDisplayContract:(void (^)(NSDictionary *))completion{LRRunExternalDisplayContract(completion);}
#endif
- (instancetype)initDesktop{if((self=[super init])){_desktopMode=YES;_autoLaunchPending=YES;}return self;}
+ (NSURL *)prepareSupport:(NSError **)error { return PrepareSupport(error); }
- (instancetype)initWithLaunchConfiguration:(NSDictionary *)configuration{if((self=[super init])){_launchConfiguration=[configuration copy];_autoLaunchPending=YES;}return self;}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    if(!_storageActivitySource)_storageActivitySource=[NSUUID UUID];
    [LRGraphicalActivity.shared setActive:YES forSource:_storageActivitySource];
}
- (void)viewDidAppear:(BOOL)animated{[super viewDidAppear:animated];
    _viewerVisible=YES;_surface.presentationSuppressed=_surface.relativeTouchpad;
    [_surface focusKeyboard];
    [self updateStorageActivity];
    if(_surface.latestPixels)[_surface presentPixels:_surface.latestPixels width:_surface.pixelWidth height:_surface.pixelHeight];
    if(_desktopMode)[LRRuntimeBridge desktopTrace:@"02 view" result:@"ok" detail:[NSString stringWithFormat:@"viewDidAppear controller=desktop auto-launch=%d window=%d size=%.0fx%.0f",_autoLaunchPending,self.view.window!=nil,self.view.bounds.size.width,self.view.bounds.size.height]];
    else [LRRuntimeBridge desktopTrace:@"02 view" result:@"info" detail:[NSString stringWithFormat:@"viewDidAppear controller=%@",_launchConfiguration?@"application":@"gui-test"]];
    // Fit measures the framebuffer after the immersive toolbar/nav layout.
    if(_desktopMode&&self.view.bounds.size.width>self.view.bounds.size.height){id automatic=[NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerDesktop.autoFullscreen"];if(!automatic||[automatic boolValue])[self setImmersive:YES animated:NO];}
    [self.navigationController.view layoutIfNeeded];[self.view layoutIfNeeded];
    if(_autoLaunchPending){_autoLaunchPending=NO;_repairAttempted=NO;[self run:_desktopMode?@"desktop":@"custom"];}}
- (void)viewDidDisappear:(BOOL)animated{
    [super viewDidDisappear:animated];
    // Opening our settings/logs must not detach the retained display transport.
    if(self.presentedViewController&&!self.isBeingDismissed&&!self.navigationController.isBeingDismissed)return;
    _viewerVisible=NO;
    if(_desktopMode){_surface.presentationSuppressed=YES;[_surface.metal clearFrame];_surface.image.image=nil;[self externalDisplayChanged:nil];}
    [self updateStorageActivity];
}
- (void)updateStorageActivity {
    BOOL active=_viewerVisible||(_desktopMode&&_client&&[self externalOutputActive]);
    if(!_storageActivitySource){if(!active)return;_storageActivitySource=[NSUUID UUID];}
    [LRGraphicalActivity.shared setActive:active forSource:_storageActivitySource];
}
- (BOOL)externalOutputActive{return lr_external_output(LRExternalOutputEnabled(),LRCurrentExternalDisplay!=nil);}
// Reuse the same RFB transport, never launch a second server for a monitor.
- (void)externalDisplayChanged:(NSNotification *)note{
    // UserDefaults notifications arrive on the writer's thread. Session/UI
    // state and the mutable presentation buffers belong to the main queue.
    if(!NSThread.isMainThread){__weak typeof(self) weak=self;dispatch_async(dispatch_get_main_queue(),^{[weak externalDisplayChanged:nil];});return;}
    if(!_desktopMode)return;
    BOOL active=[self externalOutputActive];
    if(!active){if(_surface.relativeTouchpad)_trackpadRequested=NO;_surface.relativeTouchpad=NO;[self resetModifierButtons];}
    if(!active)[LRCurrentExternalDisplay showPlaceholder];
    if(!lr_keep_desktop_transport(_viewerVisible,LRExternalOutputEnabled(),LRCurrentExternalDisplay!=nil)){
        [self detachViewer];return;
    }
    if(!_client&&_sessionTerminal.sessionPid>0&&_startState!=LRStartFailed)[self connectDesktopViewer];
    if(active&&_surface.latestPixels)[LRCurrentExternalDisplay presentFrame:_surface.latestPixels width:_surface.pixelWidth height:_surface.pixelHeight damage:(LRFrameDamage){0,0,_surface.pixelWidth,_surface.pixelHeight}];
    if(active)[_surface refreshExternalCursor];
    [self resumeRequestedTrackpad];
    [self updateStorageActivity];
}
- (void)viewDidLoad{
    [super viewDidLoad];if(_desktopMode){[NSNotificationCenter.defaultCenter addObserver:self selector:@selector(externalDisplayChanged:) name:LRExternalDisplayChangedNotification object:nil];[NSNotificationCenter.defaultCenter addObserver:self selector:@selector(externalDisplayChanged:) name:NSUserDefaultsDidChangeNotification object:nil];}[LRRuntimeBridge desktopTrace:@"02 view" result:@"start" detail:[NSString stringWithFormat:@"viewDidLoad controller=%@",_desktopMode?@"desktop":_launchConfiguration?@"application":@"gui-test"]];self.view.backgroundColor=UIColor.systemBackgroundColor;self.title=[LRRuntimeBridge activeEnvironmentIsKali]?@"Kali Applications":[LRRuntimeBridge activeEnvironmentIsDebian]?@"Debian Applications":[LRRuntimeBridge activeEnvironmentIsArch]?@"Arch Applications":[LRRuntimeBridge activeEnvironmentIsFedora]?@"Fedora Applications":@"Alpine Applications";_frameLock=[NSLock new];_compactControls=[NSMutableArray new];if(_desktopMode)self.title=@"LinuxRunner Desktop";
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Done" style:UIBarButtonItemStyleDone target:self action:@selector(done)];
#if TARGET_OS_SIMULATOR
    // Simulator workflow driver (ManagerView): taps Done on the visible screen.
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(simulatorClose) name:@"LRSimulatorClose" object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(simulatorRetry) name:@"LRSimulatorRetry" object:nil];
#endif
    UIBarButtonItem *logs=[[UIBarButtonItem alloc] initWithTitle:@"GUI Logs" style:UIBarButtonItemStylePlain target:self action:@selector(logs)];
    UIBarButtonItem *profiles=[[UIBarButtonItem alloc] initWithTitle:@"Profiles" style:UIBarButtonItemStylePlain target:self action:@selector(savedProfiles:)];
    if(_desktopMode){self.navigationItem.leftBarButtonItem=nil;self.navigationItem.rightBarButtonItems=nil;self.navigationItem.titleView=nil;[self installDesktopToolbar];}
    else self.navigationItem.rightBarButtonItems=@[logs,profiles];
    _stack=[UIStackView new];_stack.axis=UILayoutConstraintAxisVertical;_stack.spacing=10;_stack.translatesAutoresizingMaskIntoConstraints=NO;[self.view addSubview:_stack];
    [NSLayoutConstraint activateConstraints:@[[_stack.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8],[_stack.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:12],[_stack.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],[_stack.bottomAnchor constraintEqualToAnchor:self.view.keyboardLayoutGuide.topAnchor constant:-8]]];
    UIStackView *actions=[UIStackView new];actions.spacing=12;actions.distribution=UIStackViewDistributionFillEqually;
    for(NSArray *item in @[@[@"Install GUI Support",@"install"],@[@"Run X11 Test",@"runTest"],@[@"Run xterm",@"runXterm"]]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:item[0] forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:13];b.accessibilityIdentifier=item[0];[b addTarget:self action:NSSelectorFromString(item[1]) forControlEvents:UIControlEventTouchUpInside];[actions addArrangedSubview:b];}
    [_stack addArrangedSubview:actions];
    UIButton *clock=[UIButton buttonWithType:UIButtonTypeSystem];[clock setTitle:@"Run xclock" forState:UIControlStateNormal];[clock addTarget:self action:@selector(runXclock) forControlEvents:UIControlEventTouchUpInside];clock.accessibilityIdentifier=@"Run xclock";[_stack addArrangedSubview:clock];
    UIStackView *apps=[UIStackView new];apps.distribution=UIStackViewDistributionFillEqually;
    for(NSArray *item in @[@[@"Install Qt Support",@"installQt"],@[@"Run Qt Test",@"runQt"],@[@"Run App…",@"configureApp"],@[@"GPU Bridge Test",@"runGPUBridge"],@[@"Metal Gears",@"runGPUGears"],@[@"GLES2 Gears",@"runGPUGears2"],@[@"GLES2 Textures",@"runGPUTextures"],@[@"GLES2 Targets",@"runGPUTargets"],@[@"GLX Metal Check",@"runGLXMetalCheck"],@[@"GLX Metal App",@"runGLXMetalApp"]]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:item[0] forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:13];b.accessibilityIdentifier=item[0];[b addTarget:self action:NSSelectorFromString(item[1]) forControlEvents:UIControlEventTouchUpInside];[apps addArrangedSubview:b];}
    [_stack addArrangedSubview:apps];
    UIStackView *mesaActions=[UIStackView new];mesaActions.distribution=UIStackViewDistributionFillEqually;mesaActions.spacing=8;
    for(NSArray *item in @[@[@"Mesa GL Check",@"runMesaGLCheck"],@[@"Hybrid Check",@"runMesaMetalCheck"]]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:item[0] forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:14];b.accessibilityIdentifier=item[0];[b addTarget:self action:NSSelectorFromString(item[1]) forControlEvents:UIControlEventTouchUpInside];[mesaActions addArrangedSubview:b];}
    [_stack addArrangedSubview:mesaActions];
    UIStackView *blenderActions=[UIStackView new];blenderActions.distribution=UIStackViewDistributionFillEqually;blenderActions.spacing=8;
    for(NSArray *item in @[@[@"Blender Software",@"runBlenderSoftware"],@[@"Blender Hybrid",@"runBlenderHybrid"]]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:item[0] forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:14];b.accessibilityIdentifier=item[0];[b addTarget:self action:NSSelectorFromString(item[1]) forControlEvents:UIControlEventTouchUpInside];[blenderActions addArrangedSubview:b];}
    [_stack addArrangedSubview:blenderActions];
    UIStackView *browsers=[UIStackView new];browsers.distribution=UIStackViewDistributionFillEqually;
    for(NSArray *item in @[@[@"Install Browser Support",@"installBrowser"],@[@"Run LinuxRunner Browser",@"runBrowser"],@[@"Run Firefox",@"runFirefox"]]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:item[0] forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:12];b.titleLabel.numberOfLines=2;b.accessibilityIdentifier=item[0];[b addTarget:self action:NSSelectorFromString(item[1]) forControlEvents:UIControlEventTouchUpInside];[browsers addArrangedSubview:b];}
    [_stack addArrangedSubview:browsers];_resolution=[[UISegmentedControl alloc] initWithItems:_desktopMode?@[@"640×480",@"800×600",@"1024×768",@"960×540",@"Fit"]:@[@"640×480",@"800×600",@"1024×768"]];_resolution.selectedSegmentIndex=_desktopMode?LRRuntimeBridge.desktopResolutionIndex:0;_resolution.accessibilityIdentifier=@"Display resolution";[_stack addArrangedSubview:_resolution];_launchControls=@[actions,apps,mesaActions,blenderActions,browsers,clock,_resolution];_status=[UILabel new];_status.numberOfLines=0;_status.font=[UIFont systemFontOfSize:13];_status.accessibilityIdentifier=@"GUI status";_status.text=@"Install GUI Support in this environment, then open xclock or xterm. Install Qt Support for the Qt test. DISPLAY and authentication are set automatically per session. Debian GUI/browser acceptance requires physical testing.";[_stack addArrangedSubview:_status];
    _surface=[LRFramebufferView new];[_stack addArrangedSubview:_surface];[_surface.heightAnchor constraintGreaterThanOrEqualToConstant:100].active=YES;
    _modifierButtons=[NSMutableArray new];
    UIStackView *toolbar=[UIStackView new];_inputToolbar=toolbar;toolbar.spacing=4;toolbar.distribution=UIStackViewDistributionFillEqually;
    UIStackView *arrows=[UIStackView new];arrows.spacing=4;arrows.distribution=UIStackViewDistributionFillEqually;
    for(NSString *title in @[@"Keyboard",@"Esc",@"Tab",@"Shift",@"Ctrl",@"Alt",@"Super",@"←",@"↑",@"↓",@"→",@"Close"]){UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];[b setTitle:title forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:12];b.accessibilityIdentifier=[@"GUI " stringByAppendingString:title];
        if([@[@"Shift",@"Ctrl",@"Alt",@"Super"] containsObject:title])[_modifierButtons addObject:b];
        if([title isEqual:@"Keyboard"]){[b setTitle:@"" forState:UIControlStateNormal];[b setImage:[UIImage systemImageNamed:@"keyboard"] forState:UIControlStateNormal];b.accessibilityLabel=@"Keyboard";}[b addTarget:self action:@selector(tool:) forControlEvents:UIControlEventTouchUpInside];[b.heightAnchor constraintEqualToConstant:44].active=YES;
        if([@[@"←",@"↑",@"↓",@"→"] containsObject:title])[arrows addArrangedSubview:b];else [toolbar addArrangedSubview:b];}
    [_stack addArrangedSubview:toolbar];[_stack addArrangedSubview:arrows];[_compactControls addObject:arrows];
    UISegmentedControl *secondary=[[UISegmentedControl alloc] initWithItems:@[@"Right click: long press",@"Right click: two-finger tap"]];secondary.selectedSegmentIndex=0;[secondary addTarget:self action:@selector(secondaryMode:) forControlEvents:UIControlEventValueChanged];[_stack addArrangedSubview:secondary];[_compactControls addObject:secondary];
    UISwitch *trace=[UISwitch new];trace.accessibilityIdentifier=@"GUI input diagnostics";
    [trace addTarget:self action:@selector(inputDiagnostics:) forControlEvents:UIControlEventValueChanged];
    UIStackView *diagnostics=[UIStackView new];UILabel *hint=[UILabel new];hint.text=@"Log test keystrokes (includes typed text)";hint.font=[UIFont systemFontOfSize:11];[diagnostics addArrangedSubview:hint];[diagnostics addArrangedSubview:trace];[_stack addArrangedSubview:diagnostics];[_compactControls addObject:diagnostics];
    UISwitch *eagerInput=[UISwitch new];eagerInput.accessibilityIdentifier=@"GUI eager input experimental";
    eagerInput.on=[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUIEagerInput"];
    [eagerInput addTarget:self action:@selector(eagerInputChanged:) forControlEvents:UIControlEventValueChanged];
    UIStackView *inputPolicy=[UIStackView new];UILabel *inputHint=[UILabel new];inputHint.text=@"Eager input (experimental, next session)";inputHint.font=[UIFont systemFontOfSize:11];inputHint.numberOfLines=2;
    [inputPolicy addArrangedSubview:inputHint];[inputPolicy addArrangedSubview:eagerInput];[_stack addArrangedSubview:inputPolicy];[_compactControls addObject:inputPolicy];
    UISwitch *lowLatency=[UISwitch new];lowLatency.accessibilityIdentifier=@"GUI low latency input experimental";
    lowLatency.on=[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUILowLatencyInput"];
    [lowLatency addTarget:self action:@selector(lowLatencyInputChanged:) forControlEvents:UIControlEventValueChanged];
    UIStackView *latencyPolicy=[UIStackView new];UILabel *latencyHint=[UILabel new];latencyHint.text=@"Low-latency input (experimental, next session)";latencyHint.font=[UIFont systemFontOfSize:11];latencyHint.numberOfLines=2;
    [latencyPolicy addArrangedSubview:latencyHint];[latencyPolicy addArrangedSubview:lowLatency];[_stack addArrangedSubview:latencyPolicy];[_compactControls addObject:latencyPolicy];
    UISwitch *sharedImages=[UISwitch new];sharedImages.accessibilityIdentifier=@"GUI shared images experimental";
    sharedImages.on=_desktopMode?LRRuntimeBridge.desktopSharedImagesEnabled:[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUISharedImages"];
    [sharedImages addTarget:self action:@selector(sharedImagesChanged:) forControlEvents:UIControlEventValueChanged];
    UIStackView *capturePolicy=[UIStackView new];UILabel *captureHint=[UILabel new];captureHint.text=@"X11 shared images (experimental, next session)";captureHint.font=[UIFont systemFontOfSize:11];captureHint.numberOfLines=2;
    [capturePolicy addArrangedSubview:captureHint];[capturePolicy addArrangedSubview:sharedImages];[_stack addArrangedSubview:capturePolicy];[_compactControls addObject:capturePolicy];
    if(_desktopMode){
        _immersiveButton=[UIButton buttonWithType:UIButtonTypeSystem];[_immersiveButton setImage:[UIImage systemImageNamed:@"arrow.down.right.and.arrow.up.left"] forState:UIControlStateNormal];_immersiveButton.accessibilityLabel=@"Show Desktop Controls";_immersiveButton.backgroundColor=[UIColor colorWithWhite:.1 alpha:.8];_immersiveButton.layer.cornerRadius=16;_immersiveButton.translatesAutoresizingMaskIntoConstraints=NO;_immersiveButton.hidden=YES;[_immersiveButton addTarget:self action:@selector(toggleImmersive) forControlEvents:UIControlEventTouchUpInside];[self.view addSubview:_immersiveButton];[NSLayoutConstraint activateConstraints:@[[_immersiveButton.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:4],[_immersiveButton.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-8],[_immersiveButton.widthAnchor constraintEqualToConstant:32],[_immersiveButton.heightAnchor constraintEqualToConstant:32]]];
        _inputToolbar.hidden=YES;[self installKeyboardControls];
        for(UIView *v in _launchControls)v.hidden=YES;for(UIView*v in _compactControls)v.hidden=YES;_status.text=@"Open Desktop starts a real Linux window manager. The first start in an environment installs desktop support automatically (network required).";}
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(background) name:UIApplicationWillResignActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
    __weak typeof(self) weak=self;
    _exitObserver=[NSNotificationCenter.defaultCenter addObserverForName:@"ProcessExitedNotification" object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n){typeof(self) self=weak;if(self&&self.sessionTerminal.sessionPid>0&&[n.userInfo[@"pid"] intValue]==self.sessionTerminal.sessionPid){BOOL starting=self.startState==LRStartSession;[self stop];NSString *detail=[NSString stringWithContentsOfURL:[self.sessionURL URLByAppendingPathComponent:@"session.log"] encoding:NSUTF8StringEncoding error:nil];
        NSString *last=@"GUI session exited",*exitLine=nil;for(NSString *line in [detail componentsSeparatedByString:@"\n"]){if([line hasPrefix:@"GUI_SESSION:"]&&![line containsString:@"cleaned up"])last=line;if([line hasPrefix:@"DESKTOP_EXIT:"])exitLine=line;}
        // What ended, with its exit status (session.sh DESKTOP_EXIT), instead of a generic error.
        NSString *errorCode=@"DESKTOP_STARTUP_ERROR";
        if(exitLine){last=exitLine;errorCode=[exitLine containsString:@"component=display-server"]?@"DISPLAY_SERVER_EXITED":[exitLine containsString:@"component=transport"]?@"DISPLAY_TRANSPORT_FAILED":[exitLine containsString:@"reason=user-stop"]?@"SESSION_STOPPED":[exitLine containsString:@"application"]?@"DESKTOP_EXITED":errorCode;}
        if(self.desktopMode){[self traceSessionLog:detail];[LRRuntimeBridge desktopTrace:@"09 desktop" result:@"fail" detail:[NSString stringWithFormat:@"session process exited code=%@ while start-state=%ld painted=%d last=%@",n.userInfo[@"code"],(long)self.startState,self.desktopPainted,last]];}
        self.status.text=ApplicationPreflightFailure(self.sessionURL)?:[NSString stringWithFormat:@"%@. CLI remains available. GUI Logs includes application stdout/stderr.",last];
        if(self.desktopMode){
            // Missing components reported by session.sh: check again and repair (once per request).
            if([detail containsString:@"GUI_SESSION: support missing"]||[detail containsString:@"run Install Desktop Support"]||[detail containsString:@"DESKTOP_STARTUP: missing="]){self.profileReady=NO;[self beginDesktopCheck];}
            else if([detail containsString:@"GUI_SESSION: X server"]||[detail containsString:@"GUI_SESSION: focus manager"])[self desktopStartFailed:@"The display could not start" detail:[NSString stringWithFormat:@"%@\n\nError: DISPLAY_INITIALIZATION_ERROR. Tap Retry; Details shows the display log.",last]];
            else if(starting)[self desktopStartFailed:@"The desktop could not start" detail:[NSString stringWithFormat:@"%@\n\nError: %@. Tap Retry; Details shows the session log.",last,errorCode]];
            else{
                // Name the desktop component that ended and its last error line.
                NSString *app=[NSString stringWithContentsOfURL:[self.sessionURL URLByAppendingPathComponent:@"application.log"] encoding:NSUTF8StringEncoding error:nil],*component=nil;
                for(NSString *line in [app componentsSeparatedByString:@"\n"])if([line containsString:@"component_exited"])component=line;
                NSString *cause=last;
                if(component){cause=component;NSRange n=[component rangeOfString:@"("];NSRange e=[component rangeOfString:@")"];
                    if(n.location!=NSNotFound&&e.location!=NSNotFound&&e.location>n.location){NSString *logName=[component substringWithRange:NSMakeRange(n.location+1,e.location-n.location-1)];
                        NSString *componentLog=[NSString stringWithContentsOfURL:[self.sessionURL URLByAppendingPathComponent:logName] encoding:NSUTF8StringEncoding error:nil];NSString *errorLine=nil;
                        for(NSString *line in [componentLog componentsSeparatedByString:@"\n"])if([line containsString:@"ERROR"]||[line containsString:@"Bail out"]||[line containsString:@"rror:"])errorLine=line;
                        if(errorLine)cause=[NSString stringWithFormat:@"%@\n%@",component,[errorLine substringToIndex:MIN(errorLine.length,(NSUInteger)300)]];}}
                [self desktopStartFailed:@"The desktop stopped" detail:[NSString stringWithFormat:@"%@\n\nError: %@. Tap Retry to start it again, or Details for the logs.",cause,errorCode]];
            }
        }}}];
}
- (void)dealloc{NSUUID *source=_storageActivitySource;if(source)dispatch_async(dispatch_get_main_queue(),^{[LRGraphicalActivity.shared setActive:NO forSource:source];});[NSNotificationCenter.defaultCenter removeObserver:self];if(_exitObserver)[NSNotificationCenter.defaultCenter removeObserver:_exitObserver];[_client stop];[_metricsTimer invalidate];[_restartTimer invalidate];}
- (void)message:(NSString *)text{_status.text=text;[LRRuntimeBridge logCategory:@"GUI" event:text];if(_desktopMode)[LRRuntimeBridge desktopTrace:@"status" result:@"info" detail:text];}
#if TARGET_OS_SIMULATOR
- (void)simulatorClose{if(self.viewIfLoaded.window)[self done];}
- (void)simulatorRetry{if(self.viewIfLoaded.window&&_desktopMode&&_startState==LRStartFailed)[self retryDesktopStart];}
#endif
- (void)done{if(_desktopMode)[LRRuntimeBridge desktopTrace:@"lifecycle" result:@"info" detail:[NSString stringWithFormat:@"Done tapped start-state=%ld painted=%d",(long)_startState,_desktopPainted]];if(_desktopMode){if(![self externalOutputActive])[self detachViewer];_autoLaunchPending=YES;}else [self stop];[self dismissViewControllerAnimated:YES completion:nil];}
- (void)secondaryMode:(UISegmentedControl *)c{_surface.secondaryTwoFinger=c.selectedSegmentIndex==1;}
// ---- Desktop startup coordinator ------------------------------------------
// Open Desktop → (unless already verified) desktop-profile.sh check inside
// Linux → if components are missing, one targeted repair with the real stage
// shown → the session. The check runs in Linux because only the guest knows
// its packages and executable modes. Failures stop with the failed stage,
// Retry and Details; nothing loops and only one check/repair runs at a time.
- (void)desktopStartLog:(NSString *)event{[LRRuntimeBridge logCategory:@"DESKTOP_START" event:event];}
- (void)buildStartPanel{
    if(_startPanel)return;
    UIVisualEffectView *panel=[[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterial]];
    panel.layer.cornerRadius=16;panel.clipsToBounds=YES;panel.translatesAutoresizingMaskIntoConstraints=NO;
    UIStackView *stack=[UIStackView new];stack.axis=UILayoutConstraintAxisVertical;stack.spacing=10;stack.alignment=UIStackViewAlignmentCenter;stack.translatesAutoresizingMaskIntoConstraints=NO;
    _startSpinner=[[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleLarge];
    _startTitle=[UILabel new];_startTitle.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];_startTitle.numberOfLines=0;_startTitle.textAlignment=NSTextAlignmentCenter;_startTitle.adjustsFontForContentSizeCategory=YES;
    _startDetail=[UILabel new];_startDetail.font=[UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];_startDetail.textColor=UIColor.secondaryLabelColor;_startDetail.numberOfLines=0;_startDetail.textAlignment=NSTextAlignmentCenter;_startDetail.adjustsFontForContentSizeCategory=YES;
    _startButtons=[UIStackView new];_startButtons.spacing=20;
    UIButton *retry=[UIButton buttonWithType:UIButtonTypeSystem];[retry setTitle:@"Retry" forState:UIControlStateNormal];retry.titleLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];[retry addTarget:self action:@selector(retryDesktopStart) forControlEvents:UIControlEventTouchUpInside];
    UIButton *details=[UIButton buttonWithType:UIButtonTypeSystem];[details setTitle:@"Details" forState:UIControlStateNormal];[details addTarget:self action:@selector(desktopStartDetails) forControlEvents:UIControlEventTouchUpInside];
    [_startButtons addArrangedSubview:retry];[_startButtons addArrangedSubview:details];
    for(UIView *v in @[_startSpinner,_startTitle,_startDetail,_startButtons])[stack addArrangedSubview:v];
    [panel.contentView addSubview:stack];[self.view addSubview:panel];_startPanel=panel;
    UILayoutGuide *safe=self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *width=[panel.widthAnchor constraintEqualToConstant:420];width.priority=UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[[panel.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],[panel.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],width,
        [panel.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:16],[panel.trailingAnchor constraintLessThanOrEqualToAnchor:safe.trailingAnchor constant:-16],
        [panel.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor constant:-32],
        [stack.topAnchor constraintEqualToAnchor:panel.contentView.topAnchor constant:20],[stack.bottomAnchor constraintEqualToAnchor:panel.contentView.bottomAnchor constant:-20],
        [stack.leadingAnchor constraintEqualToAnchor:panel.contentView.leadingAnchor constant:20],[stack.trailingAnchor constraintEqualToAnchor:panel.contentView.trailingAnchor constant:-20],
        [_startTitle.widthAnchor constraintEqualToAnchor:stack.widthAnchor],[_startDetail.widthAnchor constraintEqualToAnchor:stack.widthAnchor]]];
}
- (void)showStartStage:(NSString *)title detail:(NSString *)detail busy:(BOOL)busy{
    [self buildStartPanel];[self.view bringSubviewToFront:_startPanel];
    _startPanel.hidden=NO;_startTitle.text=title;_startDetail.text=detail;_startButtons.hidden=busy;
    if(busy)[_startSpinner startAnimating];else [_startSpinner stopAnimating];_startSpinner.hidden=!busy;
    _status.text=title;
}
- (void)hideStartPanel{_startPanel.hidden=YES;[_startSpinner stopAnimating];}
- (NSString *)supportText:(NSString *)name{return [[NSString stringWithContentsOfURL:[_supportURL URLByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];}
- (NSDate *)supportDate:(NSString *)name{return [NSFileManager.defaultManager attributesOfItemAtPath:[_supportURL URLByAppendingPathComponent:name].path error:nil].fileModificationDate;}
// A recent "ready" result stays valid until the bundled support changes
// (PrepareSupport rewrites support.sha256) or a session reports a gap.
- (BOOL)profileVerified{
    if(_profileReady)return YES;
    NSString *result=[self supportText:@"profile-check.result"];NSDate *checked=[self supportDate:@"profile-check.result"],*deployed=[self supportDate:@"support.sha256"];
    return [result hasPrefix:@"ready"]&&checked&&deployed&&[checked compare:deployed]==NSOrderedDescending;
}
- (void)runProfile:(NSArray<NSString *> *)arguments{
    NSString *base=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/",LRGUIFolder];
    NSArray *command=[arguments.firstObject isEqual:@"check"]?[@[@"/bin/sh",[base stringByAppendingString:@"desktop-profile.sh"]] arrayByAddingObjectsFromArray:arguments]:
        [@[@"/bin/sh",[base stringByAppendingString:@"installer-run.sh"],@"desktop-profile.sh"] arrayByAddingObjectsFromArray:arguments];
    _profileStarted=NSDate.date;_profileTerminal=[LRRuntimeBridge terminalForCommand:command directory:@"/root"];
    [_profileTimer invalidate];__weak typeof(self) weak=self;
    _profileTimer=[NSTimer scheduledTimerWithTimeInterval:.5 repeats:YES block:^(NSTimer *timer){typeof(self) self=weak;if(!self){[timer invalidate];return;}[self profileTick];}];
}
- (void)beginDesktopCheck{
    if(_startState==LRStartChecking||_startState==LRStartRepairing)return;
    NSError *error=nil;_supportURL=PrepareSupport(&error);if(!_supportURL){[self desktopStartFailed:@"Desktop files could not be prepared" detail:error.localizedDescription];return;}
    _startState=LRStartChecking;[self desktopStartLog:@"stage=check"];[LRRuntimeBridge desktopTrace:@"06 packages" result:@"start" detail:@"desktop-profile.sh check"];
    [NSFileManager.defaultManager removeItemAtURL:[_supportURL URLByAppendingPathComponent:@"profile-check.result"] error:nil];
    [self showStartStage:@"Checking the Linux desktop" detail:@"Making sure the display, window manager and helpers are installed." busy:YES];
    [self runProfile:@[@"check"]];
}
- (void)beginDesktopRepair:(NSString *)missing{
    if(_repairAttempted){[self desktopStartFailed:@"Desktop components are still missing" detail:[NSString stringWithFormat:@"Missing after installation: %@. Check the network and tap Retry; Details shows the installer log.",missing]];return;}
    _repairAttempted=YES;_startState=LRStartRepairing;[self desktopStartLog:[@"stage=repair missing=" stringByAppendingString:missing]];[LRRuntimeBridge desktopTrace:@"repair" result:@"start" detail:[@"missing=" stringByAppendingString:missing]];
    [self showStartStage:@"Installing desktop components" detail:@"First start in this environment, or components were missing. This needs the network once and can take several minutes. Keep LinuxRunner open." busy:YES];
    // Setup, account tools, Plasma and Open Desktop share one native runner.
    // A second view cannot clear another operation's result or start a duplicate.
    __weak typeof(self) weak=self;
    [LRRuntimeBridge prepareDesktopProfile:^(NSString *stage){
        typeof(self) self=weak;if(self)self->_startTitle.text=stage;
    } completion:^(NSError *error){
        typeof(self) self=weak;if(!self)return;
        if(error){[self desktopStartFailed:@"Desktop installation could not finish" detail:error.localizedDescription];return;}
        self->_profileReady=YES;self->_startState=LRStartIdle;[self run:@"desktop"];
    }];
}
- (NSString *)latestProfileLine:(NSString *)prefix{
    NSString *log=[NSString stringWithContentsOfURL:[_supportURL URLByAppendingPathComponent:@"desktop-profile.sh.log"] encoding:NSUTF8StringEncoding error:nil];NSString *found=nil;
    for(NSString *line in [log componentsSeparatedByString:@"\n"])if([line hasPrefix:prefix])found=[line substringFromIndex:prefix.length];
    return [found stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
}
// Latest progress line from the installer log: package-lock waits
// (PACKAGE_STAGE, e.g. Debian's first-start configuration) or profile stages.
- (NSString *)latestProgress{
    NSString *log=[NSString stringWithContentsOfURL:[_supportURL URLByAppendingPathComponent:@"desktop-profile.sh.log"] encoding:NSUTF8StringEncoding error:nil];NSString *found=nil;
    for(NSString *line in [log componentsSeparatedByString:@"\n"])for(NSString *prefix in @[@"PROFILE_STAGE: ",@"PACKAGE_STAGE: "])if([line hasPrefix:prefix])found=[line substringFromIndex:prefix.length];
    return found;
}
// The installer's structured failure (PROFILE_FAILURE / PACKAGE_LOCK category=…)
// as a title and an explanation with the safe next step.
- (NSArray<NSString *> *)profileFailureMessage{
    NSString *log=[NSString stringWithContentsOfURL:[_supportURL URLByAppendingPathComponent:@"desktop-profile.sh.log"] encoding:NSUTF8StringEncoding error:nil];
    NSString *category=@"UNKNOWN",*detail=nil;
    for(NSString *line in [log componentsSeparatedByString:@"\n"]){
        NSRange c=[line rangeOfString:@"category="];if(c.location==NSNotFound||!([line hasPrefix:@"PROFILE_FAILURE:"]||[line hasPrefix:@"PACKAGE_LOCK:"]))continue;
        NSString *rest=[line substringFromIndex:NSMaxRange(c)];NSRange space=[rest rangeOfString:@" "];
        category=space.location==NSNotFound?rest:[rest substringToIndex:space.location];
        NSRange d=[line rangeOfString:@"detail="];detail=d.location!=NSNotFound?[line substringFromIndex:NSMaxRange(d)]:(space.location==NSNotFound?nil:[rest substringFromIndex:space.location+1]);
    }
    if(!detail.length)detail=[self latestProfileLine:@"PROFILE: result=FAIL reason="]?:@"see Details";
    NSDictionary *messages=@{
        @"PACKAGE_MANAGER_BUSY":@[@"Another package installation is still running",@"LinuxRunner waits for it and does not interrupt it. Tap Retry to keep waiting."],
        @"PACKAGE_MANAGER_STUCK":@[@"Debian setup is not progressing",@"The first-start configuration has made no progress for 15 minutes. Details shows the package and process; keep LinuxRunner open and tap Retry."],
        @"PACKAGE_DATABASE_RECOVERY_REQUIRED":@[@"Debian's package setup needs to finish",@"An earlier installation was interrupted. Tap Retry to let it complete."],
        @"TLS_CERTIFICATE_ERROR":@[@"Could not verify the Debian server certificate",@"Certificate checking stays on. Tap Retry; if it repeats, check the date and time in iOS Settings."],
        @"NETWORK_UNAVAILABLE":@[@"No connection to the Debian servers",@"Check Wi-Fi or mobile data, then tap Retry."],
        @"INSUFFICIENT_STORAGE":@[@"Not enough storage",@"Free some space on the iPhone, then tap Retry."],
        @"PACKAGE_NOT_FOUND":@[@"A desktop package is not available",@"The Debian servers did not offer a required package. Tap Retry later."],
        @"DEPENDENCY_CONFLICT":@[@"Package dependencies conflict",@"Details shows which packages; this environment may have been changed manually."],
        @"REPOSITORY_METADATA_ERROR":@[@"Debian package lists could not be read",@"Tap Retry; if it repeats, the Debian servers may be updating."],
        @"FILESYSTEM_ERROR":@[@"Files could not be written in Linux",@"Details shows the path; check free storage and tap Retry."],
    };
    NSArray *message=messages[category]?:@[@"Installing desktop components failed",@"Tap Retry; Details shows the installer log."];
    return @[message[0],[NSString stringWithFormat:@"%@\n\n%@\nError: %@",message[1],detail,category]];
}
- (void)profileTick{
    NSTimeInterval age=-[_profileStarted timeIntervalSinceNow];BOOL ended=_profileTerminal.sessionPid<=0;
    if(_startState==LRStartChecking){
        NSDate *when=[self supportDate:@"profile-check.result"];NSString *result=[self supportText:@"profile-check.result"];
        if(result.length&&when&&[when compare:_profileStarted]!=NSOrderedAscending){
            [_profileTimer invalidate];_profileTimer=nil;_profileTerminal=nil;
            NSString *first=[result componentsSeparatedByString:@"\n"].firstObject;[self desktopStartLog:[@"check=" stringByAppendingString:[result stringByReplacingOccurrencesOfString:@"\n" withString:@" "]]];
            [self traceCheckResult:result age:age];
            // Sound is optional but part of the standard desktop: add it once
            // (the repair continues without it if the network is unavailable).
            NSURL *soundMarker=[_supportURL URLByAppendingPathComponent:@"sound-repair-attempted"];
            if([first isEqual:@"ready"]&&[result containsString:@"optional-missing="]&&[result containsString:@"audio"]&&![NSFileManager.defaultManager fileExistsAtPath:soundMarker.path]&&!_repairAttempted){
                [[NSData data] writeToURL:soundMarker atomically:YES];[self beginDesktopRepair:@"audio (optional)"];[self showStartStage:@"Adding sound support" detail:@"One time, so Linux programs can play sound. The desktop starts afterwards even without network." busy:YES];return;}
            if([first isEqual:@"ready"]){_profileReady=YES;_startState=LRStartIdle;[self run:@"desktop"];}
            else [self beginDesktopRepair:[first hasPrefix:@"missing="]?[first substringFromIndex:8]:first];
            return;
        }
        if((ended&&age>5)||age>180){[_profileTimer invalidate];_profileTimer=nil;[LRRuntimeBridge desktopTrace:@"06 packages" result:@"fail" detail:[NSString stringWithFormat:@"check ended without a result after %.0fs (process ended=%d)",age,ended]];_profileTerminal=nil;[self desktopStartFailed:@"Could not check the Linux desktop" detail:@"The check inside Linux did not finish. Tap Retry; if it repeats, Details shows the startup log."];}
        return;
    }
    if(_startState==LRStartRepairing){
        NSString *stage=[self latestProgress];
        if(stage.length)_startTitle.text=stage;
        NSString *result=[self supportText:@"install-result"];
        if(result.length){
            [_profileTimer invalidate];_profileTimer=nil;_profileTerminal=nil;_startState=LRStartIdle;[self desktopStartLog:[@"repair exit=" stringByAppendingString:result]];
            [LRRuntimeBridge desktopTrace:@"repair" result:[result isEqual:@"0"]?@"ok":@"fail" detail:[NSString stringWithFormat:@"exit=%@ after %.0fs last-stage=%@",result,age,stage?:@"none"]];
            if([result isEqual:@"0"]){_profileReady=YES;[self run:@"desktop"];}
            else{NSArray *failure=[self profileFailureMessage];[self desktopStartFailed:failure[0] detail:[NSString stringWithFormat:@"%@\n\nStage: %@",failure[1],stage?:@"starting"]];}
            return;
        }
        // installer-run.sh writes install-result last; ending without it means
        // it could not run (e.g. another package operation holds the lock).
        if((ended&&age>5)||age>3600){[_profileTimer invalidate];_profileTimer=nil;_profileTerminal=nil;
            [LRRuntimeBridge desktopTrace:@"repair" result:@"fail" detail:[NSString stringWithFormat:@"installer ended without a result after %.0fs",age]];
            NSString *lock=[[NSString stringWithContentsOfURL:[_supportURL URLByAppendingPathComponent:@"desktop-profile.sh.log"] encoding:NSUTF8StringEncoding error:nil] containsString:@"PACKAGE_LOCK"]?@" Another package operation was running.":@"";
            [self desktopStartFailed:@"The installer stopped early" detail:[NSString stringWithFormat:@"It ended without a result.%@ Tap Retry.",lock]];}
    }
}
// ---- Open Desktop timeline and report --------------------------------------
- (void)traceCheckResult:(NSString *)result age:(NSTimeInterval)age{
    NSMutableDictionary *v=[NSMutableDictionary new];NSArray *lines=[result componentsSeparatedByString:@"\n"];
    for(NSString *line in lines){NSRange e=[line rangeOfString:@"="];if(e.location!=NSNotFound)v[[line substringToIndex:e.location]]=[line substringFromIndex:NSMaxRange(e)];}
    NSString *first=lines.firstObject;
    [LRRuntimeBridge desktopTrace:@"04 distribution" result:[v[@"distribution"] length]?@"ok":@"info" detail:[NSString stringWithFormat:@"distribution=%@",v[@"distribution"]?:@"unknown"]];
    if(v[@"first-start"])[LRRuntimeBridge desktopTrace:@"05 first-start" result:[v[@"first-start"] isEqual:@"done"]?@"ok":@"info" detail:[@"first-start=" stringByAppendingString:v[@"first-start"]]];
    [LRRuntimeBridge desktopTrace:@"06 packages" result:[first isEqual:@"ready"]?@"ok":@"info" detail:[NSString stringWithFormat:@"check %@ optional-missing=%@ in %.1fs",first,v[@"optional-missing"]?:@"none",age]];
    if(v[@"helpers"])[LRRuntimeBridge desktopTrace:@"07 helpers" result:[v[@"helpers"] isEqual:@"current"]?@"ok":@"info" detail:[@"helpers=" stringByAppendingString:v[@"helpers"]]];
}
// Session milestones from session.log / desktop-state, each recorded once.
- (void)traceSessionLog:(NSString *)log{
    if(!_desktopMode||!_traceSessionMarks)return;
    NSArray *marks=@[@[@"GUI_SESSION: starting X.Org",@"08 x11",@"info"],@[@"GUI_SESSION: stage=x11-ready",@"08 x11",@"ok"],@[@"GUI_SESSION: X server",@"08 x11",@"fail"],
        @[@"GUI_SESSION: keyboard=",@"09 desktop",@"start"],@[@"GUI_SESSION: stage=vnc-started",@"10 display-socket",@"info"],@[@"GUI_SESSION: application started mode=desktop",@"09 desktop",@"info"],
        @[@"GUI_SESSION: support missing",@"07 helpers",@"fail"],@[@"GUI_SESSION: application leader exited",@"09 desktop",@"fail"],@[@"GUI_SESSION: selected account missing",@"09 desktop",@"fail"],
        @[@"GUI_SESSION: invalid",@"09 desktop",@"fail"]];
    for(NSString *line in [log componentsSeparatedByString:@"\n"])for(NSArray *m in marks){
        if(![line hasPrefix:m[0]]||[_traceSessionMarks containsObject:m[0]])continue;
        [_traceSessionMarks addObject:m[0]];[LRRuntimeBridge desktopTrace:m[1] result:m[2] detail:[line substringToIndex:MIN(line.length,(NSUInteger)200)]];}
    NSString *app=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"application.log"] encoding:NSUTF8StringEncoding error:nil];
    for(NSString *line in [app componentsSeparatedByString:@"\n"]){
        if(![line hasPrefix:@"DESKTOP_STARTUP:"]||[_traceSessionMarks containsObject:line])continue;
        [_traceSessionMarks addObject:line];
        BOOL ok=[line containsString:@"wm=PASS"],fail=[line containsString:@"timeout"]||[line containsString:@"exited"]||[line containsString:@"missing"];
        [LRRuntimeBridge desktopTrace:@"09 desktop" result:ok?@"ok":fail?@"fail":@"info" detail:[line substringToIndex:MIN(line.length,(NSUInteger)200)]];}
}
static NSString *LRTail(NSURL *url,NSUInteger lines){
    NSData *data=LRReadDiagnosticTail(url,128*1024);NSString *text=LRDiagnosticTailString(data);if(!text.length)return nil;
    NSArray *all=[text componentsSeparatedByString:@"\n"];return [[all subarrayWithRange:NSMakeRange(all.count>lines?all.count-lines:0,MIN(all.count,lines))] componentsJoinedByString:@"\n"];
}
- (NSString *)reportControllerState{
    NSArray *states=@[@"Idle",@"Checking",@"Installing/Repairing",@"Starting session",@"Failed"];
    return [NSString stringWithFormat:@"controller: desktop=%d start-state=%@ profile-ready=%d repair-attempted=%d failure=%@\nsession: pid=%d dir=%@\nviewer: connected=%d frames=%llu bytes=%llu first-update=%d painted=%d\nrenderer: surface=%d metal=%d hidden=%d in-window=%d size=%.0fx%.0f view-in-window=%d\napp-state=%ld",
        _desktopMode,_startState<(NSInteger)states.count?states[_startState]:@"?",_profileReady,_repairAttempted,_startFailure?:@"none",
        _sessionTerminal.sessionPid,_sessionURL.lastPathComponent?:@"none",_client!=nil,_frames,_bytes,_traceFirstUpdate,_desktopPainted,
        _surface!=nil,_surface.metal!=nil,_surface.hidden,_surface.window!=nil,_surface.bounds.size.width,_surface.bounds.size.height,self.viewIfLoaded.window!=nil,(long)UIApplication.sharedApplication.applicationState];
}
+ (NSDictionary *)latestInputDiagnostics{
    UIViewController *root=[(UINavigationController *)LRRuntimeBridge.desktopControllerIfCreated viewControllers].firstObject;
    if(![root isKindOfClass:LRGUIViewController.class]||!((LRGUIViewController *)root).surface)return @{@"pointer_mode":@"desktop not opened in this app run",@"gc_mice":@(GCMouse.mice.count),@"assistive_touch":@(UIAccessibilityIsAssistiveTouchRunning())};
    return [((LRGUIViewController *)root).surface inputDiagnostics];
}
+ (void)buildDesktopReport:(void (^)(NSString *text))completion{
    NSMutableString *text=[NSMutableString new];
    NSDictionary *info=NSBundle.mainBundle.infoDictionary;
    [text appendFormat:@"LinuxRunner Open Desktop report\napp %@ (%@) source %@\niOS %@ %@\nattempt %@ created %@\n",info[@"CFBundleShortVersionString"],info[@"CFBundleVersion"],info[@"LinuxRunnerSourceCommit"]?:@"unknown",UIDevice.currentDevice.systemVersion,UIDevice.currentDevice.model,LRRuntimeBridge.desktopTraceAttempt,[[NSISO8601DateFormatter new] stringFromDate:NSDate.date]];
    [text appendFormat:@"runtime: running=%d status=%@\nenvironment: %@ debian=%d user=%@ graphics=%@\n",LRRuntimeBridge.isRunning,LRRuntimeBridge.runtimeStatus,LRRuntimeBridge.activeRootName,LRRuntimeBridge.activeEnvironmentIsDebian,LRRuntimeBridge.defaultLinuxUser,LRSessionGraphicsMode()];
    NSUserDefaults *defaults=NSUserDefaults.standardUserDefaults;
    [text appendFormat:@"screen awake: idle_timer_disabled=%d setting=%d reasons=%@ installation_running=%d\n",UIApplication.sharedApplication.idleTimerDisabled,
        [defaults objectForKey:@"LinuxRunner.keepScreenAwake"]?[defaults boolForKey:@"LinuxRunner.keepScreenAwake"]:YES,[LRRuntimeBridge.awakeReasons componentsJoinedByString:@","]?:@"",LRRuntimeBridge.installationRunning];
    // Timeline of the latest attempt, its last successful and first failed stage.
    NSString *attempt=[NSString stringWithFormat:@"attempt=%@ ",LRRuntimeBridge.desktopTraceAttempt];
    NSMutableArray *timeline=[NSMutableArray new];for(NSString *line in LRRuntimeBridge.desktopTraceRecords)if([line hasPrefix:attempt])[timeline addObject:line];
    NSString *lastOK=@"none",*firstFail=@"none";
    for(NSString *line in timeline){NSRange st=[line rangeOfString:@"stage="];if(st.location==NSNotFound)continue;
        NSString *rest=[line substringFromIndex:NSMaxRange(st)];NSRange r=[rest rangeOfString:@" result="];if(r.location==NSNotFound)continue;
        NSString *stage=[rest substringToIndex:r.location];
        if([rest containsString:@" result=ok"]&&[stage characterAtIndex:0]>='0'&&[stage characterAtIndex:0]<='9')lastOK=stage;
        if([rest containsString:@" result=fail"]&&[firstFail isEqual:@"none"])firstFail=[rest substringToIndex:MIN(rest.length,(NSUInteger)220)];}
    [text appendFormat:@"\n== summary\nlast successful stage: %@\nfirst failed stage: %@\n",lastOK,firstFail];
    [text appendFormat:@"\n== timeline (this attempt)\n%@\n",timeline.count?[timeline componentsJoinedByString:@"\n"]:@"(no records in this app run; see earlier attempts below)"];
    LRGUIViewController *controller=nil;UIViewController *root=[(UINavigationController *)LRRuntimeBridge.desktopControllerIfCreated viewControllers].firstObject;
    if([root isKindOfClass:LRGUIViewController.class])controller=(LRGUIViewController *)root;
    [text appendFormat:@"\n== app state\n%@\n",controller?[controller reportControllerState]:@"desktop controller not created in this app run"];
    [text appendFormat:@"\n== input (mouse, touch, keyboard sources)\n%@\n",[self latestInputDiagnostics]];
    NSMutableString *shares=[NSMutableString new];
    for(NSDictionary *f in LRRuntimeBridge.sharedFolders)[shares appendFormat:@"%@ available=%@ kind=%@\n",f[@"mountPoint"],f[@"available"],[f[@"path"] containsString:@"Mobile Documents"]?@"iCloud":[f[@"path"] containsString:@"/LiveFiles/"]?@"external drive":[f[@"path"] containsString:@"File Provider Storage"]?@"On My iPhone/app":@"other"];
    for(NSString *line in LRRuntimeBridge.logLines)if([line containsString:@"[FILES]"])[shares appendFormat:@"%@\n",line];
    [text appendFormat:@"\n== shared iPhone folders\n%@",shares.length?shares:@"none\n"];
    NSURL *support=[[LRRuntimeBridge exchangeURL:LRRuntimeBridge.activeRootName] URLByAppendingPathComponent:LRGUIFolder isDirectory:YES];
    NSURL *session=controller.sessionURL;
    void (^finish)(NSString *)=^(NSString *guest){
        [text appendFormat:@"\n== guest state\n%@\n",guest?:@"(not collected)"];
        for(NSString *name in @[@"profile-check.result",@"install-result"]){NSString *t=LRTail([support URLByAppendingPathComponent:name],10);if(t)[text appendFormat:@"\n== %@\n%@\n",name,t];}
        NSString *profile=LRTail([support URLByAppendingPathComponent:@"desktop-profile.sh.log"],40);if(profile)[text appendFormat:@"\n== desktop-profile.sh.log (last 40 lines)\n%@\n",profile];
        if(session)for(NSString *name in @[@"desktop-state",@"session.log",@"application.log",@"xvfb.log",@"xorg.log",@"graphics.log",@"display.log",@"wm.log",@"panel.log",@"desktop.log",@"plasma.log"]){
            NSString *t=LRTail([session URLByAppendingPathComponent:name],name.length>12?25:40);if([name isEqual:@"plasma.log"])t=[LRRuntimeBridge redactDiagnosticText:t?:@"" application:YES];if(t.length)[text appendFormat:@"\n== %@ (tail)\n%@\n",name,t];}
        NSString *saved=LRRuntimeBridge.desktopTraceSavedText;NSArray *savedLines=[saved componentsSeparatedByString:@"\n"];
        [text appendFormat:@"\n== earlier attempts (saved timeline, last 150 records)\n%@\n",[[savedLines subarrayWithRange:NSMakeRange(savedLines.count>150?savedLines.count-150:0,MIN(savedLines.count,(NSUInteger)150))] componentsJoinedByString:@"\n"]];
        completion([LRRuntimeBridge redactDiagnosticText:text application:NO]);
    };
    if(!LRRuntimeBridge.isRunning||!PrepareSupport(nil)){finish(@"Linux is not running; guest state not collected.");return;}
    NSURL *output=[support URLByAppendingPathComponent:@"desktop-report.out"];[NSFileManager.defaultManager removeItemAtURL:output error:nil];
    NSString *script=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/desktop-report.sh > /home/mobile/Downloads/%@/desktop-report.out 2>&1",LRGUIFolder,LRGUIFolder];
    TerminalViewController *probe=[LRRuntimeBridge terminalForCommand:@[@"/bin/sh",@"-c",script] directory:@"/"];
    NSDate *started=NSDate.date;
    __block NSTimer *poll=[NSTimer scheduledTimerWithTimeInterval:.3 repeats:YES block:^(NSTimer *timer){
        NSString *out=[NSString stringWithContentsOfURL:output encoding:NSUTF8StringEncoding error:nil];
        BOOL done=[out containsString:@"== end"],late=-[started timeIntervalSinceNow]>10;
        if(!done&&!late)return;
        [timer invalidate];(void)probe;
        finish(out.length?(done?out:[out stringByAppendingString:@"\n(incomplete after 10 s)"]):[NSString stringWithFormat:@"guest collector produced no output in 10 s (process pid=%d)",probe.sessionPid]);
    }];(void)poll;
}
// Session stages from session.log until the first painted frame; a session
// that shows no picture within 4 minutes is reported, never waited on forever.
- (void)sessionTick{
    if(_startState!=LRStartSession){[_profileTimer invalidate];_profileTimer=nil;return;}
    NSString *log=_sessionURL?[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"session.log"] encoding:NSUTF8StringEncoding error:nil]:nil;
    if([log containsString:@"GUI_SESSION: keyboard="])_startTitle.text=@"Starting the desktop";
    // Plasma's display helper is compiled inside Linux once per update; say so
    // instead of leaving a silent minute-long wait.
    if([log containsString:@"PLASMA_BUILD: building"]&&![log containsString:@"PLASMA_BUILD: compiled"]&&![log containsString:@"PLASMA_SESSION: starting"]){
        _startTitle.text=@"Preparing Plasma";_startDetail.text=@"One-time setup after an update (about a minute). Later starts take seconds.";}
    [self traceSessionLog:log];
    [self updateStartupStatus];
    if(_startState!=LRStartSession)return;
    NSString *state=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil];
    if(DesktopDegradedNotice(state)){[self desktopStartFailed:@"Desktop components could not initialize" detail:DesktopDegradedNotice(state)];return;}
    if(_sessionTerminal.sessionPid<=0||[NSFileManager.defaultManager fileExistsAtPath:[_sessionURL URLByAppendingPathComponent:@"stop"].path]){[self desktopStartFailed:@"The desktop session stopped" detail:@"Details contains the component startup logs. Existing Linux files are preserved."];return;}
    if(-[_profileStarted timeIntervalSinceNow]>240){[_profileTimer invalidate];_profileTimer=nil;
        if(_desktopPainted){
            [self desktopStartFailed:@"The desktop did not finish starting" detail:@"The Linux display connected, but the window manager, panel and desktop surface did not all become ready within 4 minutes.\n\nError: DESKTOP_COMPONENT_INITIALIZATION_ERROR. Details shows the session log."];
        }else{
            [self desktopStartFailed:@"The display did not appear" detail:@"Linux started the desktop session, but no picture arrived within 4 minutes.\n\nError: DISPLAY_INITIALIZATION_ERROR. Tap Retry; Details shows the session log."];
        }}
}
- (void)desktopStartFailed:(NSString *)title detail:(NSString *)detail{
    [LRRuntimeBridge desktopTrace:@"failure" result:@"fail" detail:[NSString stringWithFormat:@"%@ | %@",title,[detail stringByReplacingOccurrencesOfString:@"\n" withString:@" "]]];
    _startState=LRStartFailed;_startFailure=[NSString stringWithFormat:@"%@: %@",title,detail];[self desktopStartLog:[@"failed: " stringByAppendingString:_startFailure]];
    [self showStartStage:title detail:detail busy:NO];
}
- (void)retryDesktopStart{[LRRuntimeBridge desktopTraceBegin:@"retry"];_startState=LRStartIdle;_repairAttempted=NO;_profileReady=NO;[self hideStartPanel];[self run:@"desktop"];}
- (void)desktopStartDetails{
    NSMutableString *text=[NSMutableString stringWithFormat:@"%@\n",_startFailure?:@"Desktop startup"];
    for(NSString *name in @[@"profile-check.result",@"desktop-profile.sh.log"]){NSString *body=[self supportText:name];if(body.length)[text appendFormat:@"\n== %@\n%@\n",name,body.length>6000?[body substringFromIndex:body.length-6000]:body];}
    NSString *session=_sessionURL?[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"session.log"] encoding:NSUTF8StringEncoding error:nil]:nil;
    if(session.length)[text appendFormat:@"\n== session.log\n%@\n",session.length>6000?[session substringFromIndex:session.length-6000]:session];
    UIViewController *vc=[UIViewController new];vc.title=@"Desktop Startup Details";UITextView *view=[UITextView new];view.editable=NO;view.font=[UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];view.text=text;vc.view=view;
    vc.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone primaryAction:[UIAction actionWithHandler:^(UIAction *a){[vc dismissViewControllerAnimated:YES completion:nil];}]];
    __weak UIViewController *weakVC=vc;
    vc.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Share Report" style:UIBarButtonItemStylePlain target:nil action:nil];
    vc.navigationItem.leftBarButtonItem.primaryAction=[UIAction actionWithTitle:@"Share Report" image:nil identifier:nil handler:^(UIAction *a){
        [LRRuntimeBridge exportDesktopReport:^(NSURL *url){UIViewController *host=weakVC;if(!url||!host)return;
            UIActivityViewController *share=[[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
            share.popoverPresentationController.barButtonItem=host.navigationItem.leftBarButtonItem;[host presentViewController:share animated:YES completion:nil];}];}];
    [self presentViewController:[[UINavigationController alloc] initWithRootViewController:vc] animated:YES completion:nil];
}
- (void)install{[self installScript:@"install.sh"];}
- (void)installQt{[self installScript:@"install-qt.sh"];}
- (void)installScript:(NSString *)installer{
    if(_client||(_desktopMode&&_sessionTerminal.sessionPid>0)){[self message:@"Stop the desktop before installing support. Save open documents first."];return;}
    NSError *error=nil;_supportURL=PrepareSupport(&error);if(!_supportURL){[self message:error.localizedDescription];return;}
    _installStatusURL=[_supportURL URLByAppendingPathComponent:@"install-result"];
    [NSFileManager.defaultManager removeItemAtURL:_installStatusURL error:nil];
    NSString *path=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/installer-run.sh",LRGUIFolder];
    TerminalViewController *terminal=[LRRuntimeBridge terminalForCommand:@[@"/bin/sh",path,installer] directory:@"/root"];
    _installerTerminal=terminal;
    terminal.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Done" style:UIBarButtonItemStyleDone target:self action:@selector(closeInstaller)];
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:terminal];nav.modalPresentationStyle=UIModalPresentationFullScreen;[self presentViewController:nav animated:YES completion:^{currentTerminalViewController=terminal;}];
}
- (void)closeInstaller{
    if(_installerTerminal.sessionPid>0){
        UIAlertController *wait=[UIAlertController alertControllerWithTitle:@"Installation is running" message:@"Keep LinuxRunner in the foreground until installation finishes. Package installation alone does not finish compiling and verifying Browser Support." preferredStyle:UIAlertControllerStyleAlert];
        [wait addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];[_installerTerminal presentViewController:wait animated:YES completion:nil];return;
    }
    NSString *result=[[NSString stringWithContentsOfURL:_installStatusURL encoding:NSUTF8StringEncoding error:nil] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    [self message:[result isEqual:@"0"]?@"Support installation verified. You can launch the Linux applications.":@"Support installation failed or was interrupted. Open GUI Logs for the exact package/compiler failure, then retry installation."];
    _installerTerminal=nil;currentTerminalViewController=nil;[self dismissViewControllerAnimated:YES completion:^{if(self.desktopMode&&[result isEqual:@"0"]) [self run:@"desktop"];}];
}
- (void)eagerInputChanged:(UISwitch *)control{[NSUserDefaults.standardUserDefaults setBool:control.on forKey:@"LinuxRunnerGUIEagerInput"];}
- (void)lowLatencyInputChanged:(UISwitch *)control{[NSUserDefaults.standardUserDefaults setBool:control.on forKey:@"LinuxRunnerGUILowLatencyInput"];}
- (void)sharedImagesChanged:(UISwitch *)control{[NSUserDefaults.standardUserDefaults setBool:control.on forKey:@"LinuxRunnerGUISharedImages"];}
- (void)inputDiagnostics:(UISwitch *)control{_surface.traceInput=control.on;_surface.traceCount=0;[self configureTransportDiagnostics];}
- (void)configureTransportDiagnostics {
    _transportDiagnosticEvents=0;
    if(!_surface.traceInput){_client.transportMetricsHandler=nil;_client.inputTraceHandler=nil;return;}
    __weak typeof(self) weak=self;__weak LRRFBClient *client=_client;
    _client.inputTraceHandler=^(NSString *event){dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;if(self.client==client)[self.surface trace:event];});};
    _client.transportMetricsHandler=^(NSDictionary *event){
        double delivered=LRGUIClock();
        dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;
            if(!self||!client||self.client!=client||!self.surface.traceInput)return;
            if(self.transportDiagnosticEvents>=96){self.client.transportMetricsHandler=nil;return;}
            self.transportDiagnosticEvents++;
            NSMutableDictionary *record=[event mutableCopy];record[@"native_monotonic_ms"]=@(delivered*1000);
            record[@"diagnostic_main_queue_ms"]=@((LRGUIClock()-delivered)*1000);
            NSData *json=[NSJSONSerialization dataWithJSONObject:record options:0 error:nil];
            if(json)[LRRuntimeBridge logCategory:@"GUI_TRANSPORT" event:[[NSString alloc]initWithData:json encoding:NSUTF8StringEncoding]];
            if(self.transportDiagnosticEvents==96)self.client.transportMetricsHandler=nil;
        });
    };
}
- (void)installBrowser{[self installScript:@"install-browser.sh"];}
- (void)runBrowser{[self launchBrowser:@"chromium"];}
- (void)runFirefox{[self launchBrowser:@"firefox"];}
- (void)launchBrowser:(NSString *)target{
    NSString *base=[NSString stringWithFormat:@"/home/mobile/Downloads/%@",LRGUIFolder];
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"browser-launch.sh"],target],@"environment":@{@"LINUXRUNNER_ENVIRONMENT":LRRuntimeBridge.activeRootName}};
    [self run:@"custom"];
}
- (void)runGPUGears {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){_status.text=@"Metal Gears currently requires Debian ARM64";return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"gpu-gears.sh"]],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runGPUGears2 {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){_status.text=@"GLES2 Gears currently requires Debian ARM64";return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"gpu-gears2.sh"]],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runGPUTextures {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){_status.text=@"GLES2 texture checks require Debian ARM64";return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"gpu-textures-check.sh"]],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runGPUTargets {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){_status.text=@"GLES2 target checks require Debian ARM64";return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"gpu-framebuffers-check.sh"]],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runGLXMetalScript:(NSString *)script {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){_status.text=@"These graphics checks require the Debian ARM64 profile";return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:script]],@"environment":@{@"LINUXRUNNER_GLX":@"software"}};
    [self run:@"custom"];
}
- (void)runGLXMetalCheck { [self runGLXMetalScript:@"gpu-glx-check.sh"]; }
- (void)runGLXMetalApp { [self runGLXMetalScript:@"gpu-glxheads.sh"]; }
- (void)runMesaGLCheck { [self runGLXMetalScript:@"gpu-mesa-check.sh"]; }
- (void)runBlenderSoftware { [self runGLXMetalScript:@"blender-software.sh"]; }
- (void)runBlenderHybrid { [self runGLXMetalScript:@"blender-hybrid.sh"]; }
- (void)runMesaMetalCheck { [self runGLXMetalScript:@"gpu-mesa-metal-check.sh"]; }
- (void)runGPUBridge {
    if(![LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsKali]){[self message:@"The experimental GPU demo currently requires Debian ARM64 glibc. Alpine graphics and the existing Metal presenter remain available."];return;}
    NSString *base=@"/home/mobile/Downloads/.linuxrunner/gui";
    self.launchConfiguration=@{@"executable":@"/bin/sh",@"directory":base,@"arguments":@[[base stringByAppendingPathComponent:@"gpu-demo.sh"]],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runXclock{
    self.launchConfiguration=@{@"executable":@"/usr/bin/xclock",@"directory":@"/root",@"arguments":@[],@"environment":@{}};
    [self run:@"custom"];
}
- (void)runQt{[self run:@"qt"];}
- (NSString *)profilesKey{return [@"LinuxRunnerGUILaunchProfiles." stringByAppendingString:LRRuntimeBridge.activeRootName];}
- (NSArray *)environmentProfiles{
    NSArray *saved=[NSUserDefaults.standardUserDefaults arrayForKey:self.profilesKey];
    if(!saved&&[LRRuntimeBridge.activeRootName isEqual:@"default"]){
        // One-time legacy migration into the original Alpine environment only.
        saved=[NSUserDefaults.standardUserDefaults arrayForKey:@"LinuxRunnerGUILaunchProfiles"];
        if(saved)[NSUserDefaults.standardUserDefaults setObject:saved forKey:self.profilesKey];
    }
    return saved?:@[];
}
- (void)runTest{[self run:@"test"];}
- (void)runXterm{[self run:@"xterm"];}
static NSString *ShellQuote(NSString *value){return [NSString stringWithFormat:@"'%@'",[value stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];}
- (void)savedProfiles:(UIBarButtonItem *)sender{
    NSArray *profiles=self.environmentProfiles;
    UIAlertController *menu=[UIAlertController alertControllerWithTitle:@"Saved Linux applications" message:profiles.count?nil:@"Run App… saves named launch configurations." preferredStyle:UIAlertControllerStyleActionSheet];
    for(NSDictionary *p in profiles){[menu addAction:[UIAlertAction actionWithTitle:p[@"name"] style:UIAlertActionStyleDefault handler:^(UIAlertAction*a){self.launchConfiguration=p;[self run:@"custom"];}]];}
    [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    menu.popoverPresentationController.barButtonItem=sender;[self presentViewController:menu animated:YES completion:nil];
}
- (void)configureApp{
    UIAlertController *editor=[UIAlertController alertControllerWithTitle:@"Run Linux GUI application" message:@"Absolute guest paths. Arguments: JSON array. Environment: JSON object. DISPLAY/authentication are managed automatically." preferredStyle:UIAlertControllerStyleAlert];
    NSArray *labels=@[@"Executable",@"Arguments JSON",@"Working directory",@"Environment JSON",@"Profile name (optional)"];
    NSArray *defaults=@[@"/usr/bin/xterm",@"[]",@"/root",@"{}",@""];
    for(unsigned i=0;i<5;i++){[editor addTextFieldWithConfigurationHandler:^(UITextField *f){f.placeholder=labels[i];f.accessibilityIdentifier=labels[i];f.text=defaults[i];f.autocorrectionType=UITextAutocorrectionTypeNo;f.autocapitalizationType=UITextAutocapitalizationTypeNone;}];}
    [editor addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [editor addAction:[UIAlertAction actionWithTitle:@"Run" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){
        NSArray *fields=editor.textFields;NSString *executable=[fields[0] text],*directory=[fields[2] text];
        id args=[NSJSONSerialization JSONObjectWithData:[[fields[1] text] dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingFragmentsAllowed error:nil];
        id env=[NSJSONSerialization JSONObjectWithData:[[fields[3] text] dataUsingEncoding:NSUTF8StringEncoding] options:NSJSONReadingFragmentsAllowed error:nil];
        BOOL valid=[executable hasPrefix:@"/"]&&[directory hasPrefix:@"/"]&&[args isKindOfClass:NSArray.class]&&[env isKindOfClass:NSDictionary.class];
        NSMutableArray *values=[NSMutableArray arrayWithObjects:executable?:@"",directory?:@"",nil];
        if(valid){for(id arg in args){if(![arg isKindOfClass:NSString.class]){valid=NO;break;}[values addObject:arg];}
            NSRegularExpression *name=[NSRegularExpression regularExpressionWithPattern:@"^[A-Za-z_][A-Za-z0-9_]*$" options:0 error:nil];
            for(NSString *key in env){if(![env[key] isKindOfClass:NSString.class]||![name firstMatchInString:key options:0 range:NSMakeRange(0,key.length)]||[@[@"DISPLAY",@"XAUTHORITY",@"XDG_RUNTIME_DIR"] containsObject:key]){valid=NO;break;}[values addObject:key];[values addObject:env[key]];}}
        NSUInteger bytes=0;for(NSString *v in values){bytes+=[v lengthOfBytesUsingEncoding:NSUTF8StringEncoding]+1;if([v rangeOfString:[NSString stringWithFormat:@"%C",(unichar)0]].location!=NSNotFound)valid=NO;}
        if(!valid||bytes>8192){[self message:@"Invalid launch configuration: absolute paths, string arguments/environment, no NUL, total ≤8 KiB. DISPLAY/auth are managed."];return;}
        NSString *name=[fields[4] text];if(!name.length)name=executable.lastPathComponent;if(name.length>80)name=[name substringToIndex:80];
        self.launchConfiguration=@{@"name":name,@"executable":executable,@"directory":directory,@"arguments":args,@"environment":env};
        NSMutableArray *saved=[self.environmentProfiles mutableCopy];
        NSIndexSet *replace=[saved indexesOfObjectsPassingTest:^BOOL(NSDictionary *p,NSUInteger i,BOOL *stop){return [p[@"name"] isEqual:name];}];[saved removeObjectsAtIndexes:replace];[saved insertObject:self.launchConfiguration atIndex:0];while(saved.count>10)[saved removeLastObject];
        [NSUserDefaults.standardUserDefaults setObject:saved forKey:self.profilesKey];[self run:@"custom"];
    }]];[self presentViewController:editor animated:YES completion:nil];
}
NSString *const LRGraphicsModeDefaultsKey=@"LinuxRunnerGraphics.mode";
static __weak LRGUIViewController *LRLatestSessionController;
static NSString *LRSessionGraphicsMode(void){
    NSString *mode=[NSUserDefaults.standardUserDefaults stringForKey:LRGraphicsModeDefaultsKey];
    // GPU by default: the Metal bridge where the distribution passed its
    // contract (gpu-env.sh falls back to Mesa elsewhere, with the reason), and
    // it retires itself per process when Metal would be slower than Mesa.
    return [@[@"mesa",@"mesa-metal-resident",@"mesa-metal-verify"] containsObject:mode]?mode:@"mesa-metal-resident";
}
// The session's routing decision (last GPU_ROUTING line the guest logged) and
// the bridge's per-process statistics files (<session>/gpu-stats/<pid>.txt).
+ (NSDictionary *)latestGraphicsSummary{
    LRGUIViewController *controller=LRLatestSessionController;NSURL *session=controller.sessionURL;
    NSMutableDictionary *summary=[@{@"requested":LRSessionGraphicsMode(),@"session":@(session!=nil)} mutableCopy];
    if(!session)return summary;
    NSString *log=[NSString stringWithContentsOfURL:[session URLByAppendingPathComponent:@"application.log"] encoding:NSUTF8StringEncoding error:nil];
    for(NSString *line in [log componentsSeparatedByString:@"\n"])if([line hasPrefix:@"GPU_ROUTING:"])summary[@"routing"]=line;
    NSMutableArray *processes=[NSMutableArray array];NSURL *stats=[session URLByAppendingPathComponent:@"gpu-stats" isDirectory:YES];
    for(NSURL *file in [NSFileManager.defaultManager contentsOfDirectoryAtURL:stats includingPropertiesForKeys:nil options:0 error:nil]){
        if(![file.pathExtension isEqual:@"txt"])continue;
        NSString *text=[NSString stringWithContentsOfURL:file encoding:NSUTF8StringEncoding error:nil];if(text.length>8192)continue;
        NSMutableDictionary *values=[NSMutableDictionary dictionary];
        for(NSString *line in [text componentsSeparatedByString:@"\n"]){NSRange eq=[line rangeOfString:@"="];if(eq.location!=NSNotFound&&eq.location>0)values[[line substringToIndex:eq.location]]=[line substringFromIndex:eq.location+1];}
        if(values[@"pid"])[processes addObject:values];
    }
    [processes sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){return [@([b[@"updated_unix"] longLongValue]) compare:@([a[@"updated_unix"] longLongValue])];}];
    summary[@"processes"]=processes;return summary;
}
- (void)run:(NSString *)mode{
    LRLatestSessionController=self;
    if(_desktopMode)[LRRuntimeBridge desktopTrace:@"03 runtime" result:LRRuntimeBridge.isRunning?@"ok":@"fail" detail:[NSString stringWithFormat:@"run mode=%@ start-state=%ld client=%d session-pid=%d profile-ready=%d status=%@",mode,(long)_startState,_client!=nil,_sessionTerminal.sessionPid,_profileReady,LRRuntimeBridge.runtimeStatus]];
    // Re-entering a desktop is idempotent: keep its authenticated viewer and
    // server. Standalone GUI tests retain their single-display guard.
    if(_client&&_desktopMode){
        if(_sessionTerminal.sessionPid>0&&_sessionURL&&![NSFileManager.defaultManager fileExistsAtPath:[_sessionURL URLByAppendingPathComponent:@"stop"].path]){[self updateStartupStatus];return;}
        [self detachViewer]; // ended owned session; never stop unrelated guests
    }
    if(_client){[self message:@"A display session is already running. Close it first."];return;}
    if(_desktopMode&&_sessionTerminal.sessionPid>0&&_sessionURL&&![NSFileManager.defaultManager fileExistsAtPath:[_sessionURL URLByAppendingPathComponent:@"stop"].path]){[self connectDesktopViewer];return;}
    if(![LRRuntimeBridge isRunning]){if(_desktopMode)[self desktopStartFailed:@"Linux is not running" detail:[NSString stringWithFormat:@"%@\n\nError: RUNTIME_NOT_RUNNING. Tap Retry once Linux has started.",LRRuntimeBridge.runtimeStatus]];else [self message:LRRuntimeBridge.runtimeStatus];return;}
    NSError *error=nil;_supportURL=PrepareSupport(&error);if(!_supportURL){if(_desktopMode)[self desktopStartFailed:@"Desktop files could not be prepared" detail:[error.localizedDescription stringByAppendingString:@"\n\nError: FILESYSTEM_ERROR"]];else [self message:error.localizedDescription];return;}
    if(_desktopMode&&(_startState==LRStartChecking||_startState==LRStartRepairing)){[self.view bringSubviewToFront:_startPanel];return;} // already working on it
    if(_desktopMode&&![self profileVerified]){[self beginDesktopCheck];return;}
    if(_desktopMode)[LRRuntimeBridge desktopTrace:@"06 packages" result:@"ok" detail:_profileReady?@"verified in this app run":@"verified earlier (profile-check.result newer than the support files)"];
    if(_desktopMode){_startState=LRStartSession;[self showStartStage:@"Starting the display" detail:@"Starting the display and window manager." busy:YES];[self desktopStartLog:@"stage=session"];
        [_profileTimer invalidate];_profileStarted=NSDate.date;__weak typeof(self) weak=self;
        _profileTimer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer){typeof(self) self=weak;if(!self){[timer invalidate];return;}[self sessionTick];}];}
    if([mode isEqual:@"custom"]&& !_launchConfiguration){[self message:@"Choose an application first."];return;}
    // Debian's installed Mesa renderer needs the server extension even when
    // the final framebuffer uses Metal. Keep an explicit diagnostic opt-out.
    // Arch's Mesa GLX clients need the same server extension as Debian.
    // This enables a protocol capability, not a hardware-acceleration claim.
    NSString *glxMode=([LRRuntimeBridge activeEnvironmentIsDebian]||[LRRuntimeBridge activeEnvironmentIsArch]||[LRRuntimeBridge activeEnvironmentIsFedora])?@"software":@"off";
    if([mode isEqual:@"custom"]&&_launchConfiguration[@"environment"][@"LINUXRUNNER_GLX"])
        glxMode=_launchConfiguration[@"environment"][@"LINUXRUNNER_GLX"];
    if(![@[@"off",@"software"] containsObject:glxMode]){[self message:@"LINUXRUNNER_GLX must be off or software. Software GLX is Mesa rendering, not Metal acceleration."];return;}
    NSString *session=[@"session-" stringByAppendingString:NSUUID.UUID.UUIDString.lowercaseString];_sessionURL=[_supportURL URLByAppendingPathComponent:session isDirectory:YES];
    if(![NSFileManager.defaultManager createDirectoryAtURL:_sessionURL withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:&error]){[self message:error.localizedDescription];return;}
    uint8_t random[24];if(SecRandomCopyBytes(kSecRandomDefault,sizeof random,random)!=errSecSuccess){[self message:@"Cannot generate display authentication."];return;}
    NSString *alphabet=@"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";NSMutableString *password=[NSMutableString new],*cookie=[NSMutableString new];
    for(unsigned i=0;i<8;i++)[password appendFormat:@"%C",[alphabet characterAtIndex:random[i]%alphabet.length]];
    for(unsigned i=8;i<24;i++)[cookie appendFormat:@"%02x",random[i]];
    int fd=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in address={.sin_family=AF_INET,.sin_port=0};address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);socklen_t length=sizeof address;
    if(fd<0||bind(fd,(struct sockaddr *)&address,sizeof address)||getsockname(fd,(struct sockaddr *)&address,&length)){if(fd>=0)close(fd);[self message:@"Cannot allocate a local display port."];return;}
    uint16_t port=ntohs(address.sin_port);close(fd);unsigned display=1000+((unsigned)random[0]*256+random[1])%4000;
    // Sound: a second loopback port for the session's PulseAudio output.
    _audioPort=0;
    if(LRAudioBridge.enabled){int afd=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in a2={.sin_family=AF_INET,.sin_port=0};a2.sin_addr.s_addr=htonl(INADDR_LOOPBACK);socklen_t l2=sizeof a2;
        if(afd>=0&&!bind(afd,(struct sockaddr *)&a2,sizeof a2)&&!getsockname(afd,(struct sockaddr *)&a2,&l2)&&ntohs(a2.sin_port)!=port)_audioPort=ntohs(a2.sin_port);if(afd>=0)close(afd);}
    if(_desktopMode)_resolution.selectedSegmentIndex=LRRuntimeBridge.desktopResolutionIndex;
    NSString *geometry=_desktopMode&&_resolution.selectedSegmentIndex==4?[self fitScreenGeometry]:@[@"640x480",@"800x600",@"1024x768",@"960x540"][_resolution.selectedSegmentIndex];
    {unsigned gw=0,gh=0;if(sscanf(geometry.UTF8String,"%ux%u",&gw,&gh)==2)[LRRuntimeBridge setDesktopDisplayWidth:gw height:gh];}   // KMS compositors use exactly this size
    [LRRuntimeBridge logCategory:@"GUI_SESSION" event:[NSString stringWithFormat:@"root=%@ distribution=%@ geometry=%@ mode=%@ GLX=%@ (server capability; application renderer is independent)",LRRuntimeBridge.activeRootName,LRRuntimeBridge.activeEnvironmentIsSteamOS?@"steamos":[LRRuntimeBridge activeEnvironmentIsDebian]?@"debian":[LRRuntimeBridge activeEnvironmentIsArch]?@"archarm":[LRRuntimeBridge activeEnvironmentIsFedora]?@"fedora":@"alpine",geometry,mode,glxMode]];
    _sessionGuestUser=[LRRuntimeBridge.defaultLinuxUser copy];
    NSString *desktopKey=[@"LinuxRunnerDesktop.environment." stringByAppendingString:LRRuntimeBridge.activeRootName];
    NSString *selectedDesktop=[NSUserDefaults.standardUserDefaults stringForKey:desktopKey];
    if(!selectedDesktop && LRRuntimeBridge.activeEnvironmentIsDebian && !LRRuntimeBridge.activeEnvironmentIsKali)selectedDesktop=[NSUserDefaults.standardUserDefaults stringForKey:@"LinuxRunnerDesktop.environment"];
    NSDictionary *files=@{@"hyprland-effects":([NSUserDefaults.standardUserDefaults stringForKey:[@"LinuxRunnerDesktop.hyprland.effects." stringByAppendingString:LRRuntimeBridge.activeRootName]]?:@"visual"),@"desktop-environment":([selectedDesktop isEqual:@"plasma"]||[selectedDesktop isEqual:@"lxqt"]||[selectedDesktop isEqual:@"enlightenment"]||[selectedDesktop isEqual:@"wayfire"]||[selectedDesktop isEqual:@"hyprland"]||[selectedDesktop isEqual:@"xfce"])?selectedDesktop:@"linuxrunner",@"guest-user":_sessionGuestUser,@"geometry":geometry,@"glx-mode":glxMode,@"gpu-mode":LRSessionGraphicsMode(),@"input-mode":[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUIEagerInput"]?@"eager":@"legacy",@"shm-mode":(_desktopMode?LRRuntimeBridge.desktopSharedImagesEnabled:[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUISharedImages"])?@"sysv":@"off",@"vnc-timing":[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerGUILowLatencyInput"]?@"threads":@"default",@"password":password,@"cookie":cookie,@"port":[NSString stringWithFormat:@"%u",port],@"audio-port":_audioPort?[NSString stringWithFormat:@"%u",_audioPort]:@"",@"display":[NSString stringWithFormat:@"%u",display]};
    for(NSString *name in files){NSURL *url=[_sessionURL URLByAppendingPathComponent:name];if(![files[name] writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error]||![NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:url.path error:&error]){[self message:error.localizedDescription];return;}}
    NSString *xkb=[NSUserDefaults.standardUserDefaults stringForKey:@"LinuxRunnerGUIXKBMode"]?:@"xkb";
    [xkb writeToURL:[_sessionURL URLByAppendingPathComponent:@"xkb-mode"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    if([mode isEqual:@"custom"]){
        NSDictionary *config=_launchConfiguration;NSMutableString *command=[NSMutableString stringWithString:@"#!/bin/sh\nset -eu\n"];
        for(NSString *name in config[@"environment"]){
            // Launchers saved before Settings › Graphics stored "mesa" as the implicit default; they follow the session default now.
            if([name isEqual:@"LR_GUI_GPU_MODE"]&&[config[@"environment"][name] isEqual:@"mesa"])continue;
            [command appendFormat:@"export %@=%@\n",name,ShellQuote(config[@"environment"][name])];}
        NSString *launcher=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/launch.sh",LRGUIFolder];
        [command appendFormat:@"exec /bin/sh %@ %@ %@",ShellQuote(launcher),ShellQuote(config[@"directory"]),ShellQuote(config[@"executable"])];
        for(NSString *arg in config[@"arguments"])[command appendFormat:@" %@",ShellQuote(arg)];[command appendString:@"\n"];
        if(![command writeToURL:[_sessionURL URLByAppendingPathComponent:@"command.sh"] atomically:YES encoding:NSUTF8StringEncoding error:&error]){[self message:error.localizedDescription];return;}
        [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:[_sessionURL URLByAppendingPathComponent:@"command.sh"].path error:nil];
    }
    if(_desktopMode)[NSFileManager.defaultManager createDirectoryAtURL:[_sessionURL URLByAppendingPathComponent:@"requests"] withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil];
    NSString *script=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/session.sh",LRGUIFolder];
    _traceSessionMarks=[NSMutableSet new];
    if(_desktopMode)[LRRuntimeBridge desktopTrace:@"08 x11" result:@"start" detail:[NSString stringWithFormat:@"session.sh launch display=:%u geometry=%@ user=%@ gpu=%@ audio-port=%u",display,geometry,_sessionGuestUser,LRSessionGraphicsMode(),_audioPort]];
    _sessionTerminal=[LRRuntimeBridge terminalForCommand:@[@"/bin/sh",script,session,mode] directory:@"/root"];
    if(_desktopMode)[LRRuntimeBridge desktopTrace:@"08 x11" result:_sessionTerminal.sessionPid>0?@"info":@"fail" detail:[NSString stringWithFormat:@"session process pid=%d launch-error=%d",_sessionTerminal.sessionPid,_sessionTerminal.runnerLaunchError]];
    if(_sessionTerminal.runnerLaunchError<0){if(_desktopMode)[self desktopStartFailed:@"The desktop session could not start" detail:@"Linux could not run the session script. Tap Retry; Details shows the startup log."];else [self message:@"Guest session could not launch. See startup logs."];return;}
    if(_client){[self updateStartupStatus];return;}
    [self attachViewerWithPort:port password:password];
    if(_queuedDesktopApplication){NSDictionary *app=_queuedDesktopApplication;_queuedDesktopApplication=nil;[self launchDesktopApplication:app];}
}
// Sound state for Settings › Audio: the bridge's own counters plus what the
// session reported (audio=pulseaudio / unavailable) in session.log.
- (NSDictionary *)audioSummary{
    if(!LRAudioBridge.enabled)return @{@"state":@"Off in Settings"};
    NSString *log=_sessionURL?[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"session.log"] encoding:NSUTF8StringEncoding error:nil]:nil;
    if([log containsString:@"GUI_SESSION: audio=unavailable"])return @{@"state":@"Sound support not installed (Desktop › Install Desktop Support adds it)"};
    return _audio?[_audio statistics]:@{@"state":@"No running session"};
}
// desktop-components.sh restarts a crashed panel or desktop surface a few times,
// then keeps the session running without it and records degraded=<roles>.
static NSString *DesktopDegradedNotice(NSString *state){
    for(NSString *line in [state componentsSeparatedByString:@"\n"])if([line hasPrefix:@"degraded="]&&line.length>9){
        NSMutableArray *names=[NSMutableArray array];
        for(NSString *role in [[line substringFromIndex:9] componentsSeparatedByString:@","])[names addObject:[role isEqual:@"panel"]?@"panel (panel.log)":[role isEqual:@"surface"]?@"desktop icons and wallpaper (desktop.log)":role];
        return [NSString stringWithFormat:@"Desktop running without the %@ — it kept crashing. Apps still work; Details has the log.",[names componentsJoinedByString:@" and "]];}
    return nil;
}
- (void)setPreviewWanted:(BOOL)wanted{
    if(!_desktopMode||!self.isViewLoaded)return;
    BOOL running=_sessionTerminal.sessionPid>0&&_sessionURL&&_startState!=LRStartFailed&&![NSFileManager.defaultManager fileExistsAtPath:[_sessionURL URLByAppendingPathComponent:@"stop"].path];
    if(wanted){if(!_client&&running)[self connectDesktopViewer];}
    // Only the preview needed it: leave the desktop exactly as Done left it.
    else if(_client&&!self.viewIfLoaded.window&&![self externalOutputActive])[self detachViewer];
}
- (UIImage *)previewImageIfChangedSince:(uint64_t *)generation{
    [_frameLock lock];uint64_t frames=_frames;[_frameLock unlock];
    NSData *pixels=_surface.latestPixels;uint16_t w=_surface.pixelWidth,h=_surface.pixelHeight;
    if(!pixels||!w||!h||pixels.length!=(NSUInteger)w*h*4)return nil;
    if(generation&&*generation==frames&&frames)return nil;
    if(generation)*generation=frames;
    // A copy: the live frame buffer is patched in place by later updates.
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)[pixels copy]);
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGImageRef image=CGImageCreate(w,h,8,32,w*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,NO,kCGRenderingIntentDefault);
    CGColorSpaceRelease(space);CGDataProviderRelease(provider);
    if(!image)return nil;
    UIImage *result=[UIImage imageWithCGImage:image];CGImageRelease(image);
    return result;
}
- (NSDictionary *)desktopSessionSummary {
    BOOL alive=_desktopMode&&_sessionTerminal.sessionPid>0;
    NSString *state=alive?@"Starting":@"Stopped";
    if(alive){NSString *readiness=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil];
        if([readiness containsString:@"ready=1"])state=!_desktopPainted?@"Components ready; waiting for paint":_inputVerified==0?@"Painted; checking input":(DesktopDegradedNotice(readiness)||_inputVerified<0)?@"Ready (degraded)":@"Ready";}
    NSString *geometry=_sessionURL?[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"geometry"] encoding:NSUTF8StringEncoding error:nil]:nil;
    return @{@"state":state,@"alive":@(alive),@"connected":@(_client!=nil),@"geometry":geometry?:@"",@"user":_sessionGuestUser?:@"unknown",@"selectedUser":LRRuntimeBridge.defaultLinuxUser,@"root":LRRuntimeBridge.activeRootName,@"audio":[self audioSummary]};
}
// "Fit" chooses a bounded LANDSCAPE logical mode once at session startup.
// Portrait phones/monitors and narrow iPad windows letterbox that same desktop.
// UIKit rotation/keyboard layout only changes the existing aspect-fit viewport.
- (NSString *)fitScreenGeometry{
    UIView *target=[self externalOutputActive]?LRCurrentExternalDisplay.view:_surface;
    [target layoutIfNeeded];
    CGSize s=target.bounds.size;
    if(s.width<1||s.height<1)s=self.view.bounds.size;
    uint16_t W,H;lr_desktop_geometry(s.width,s.height,&W,&H);
    return [NSString stringWithFormat:@"%ux%u",W,H];
}
// iPad / hardware keyboard: app actions on Control-Option-Command, a combination
// Linux desktops do not use, so every other key (Command is Super) still
// reaches Linux. Hold Command to see them in the iPadOS shortcut overlay.
- (NSArray<UIKeyCommand *> *)keyCommands{
    if(!_desktopMode)return @[];
    UIKeyModifierFlags m=UIKeyModifierControl|UIKeyModifierAlternate|UIKeyModifierCommand;
    return @[[UIKeyCommand commandWithTitle:@"Desktop Options" image:nil action:@selector(shortcutOptions) input:@"m" modifierFlags:m propertyList:nil],
             [UIKeyCommand commandWithTitle:@"Fit to Screen" image:nil action:@selector(shortcutFit) input:@"f" modifierFlags:m propertyList:nil],
             [UIKeyCommand commandWithTitle:@"Actual Size" image:nil action:@selector(shortcutActualSize) input:@"0" modifierFlags:m propertyList:nil],
             [UIKeyCommand commandWithTitle:@"Close Desktop View" image:nil action:@selector(done) input:@"w" modifierFlags:m propertyList:nil]];
}
- (void)shortcutOptions{[self desktopOptions:_moreButton];}
- (void)shortcutFit{[self.surface fitViewport];}
- (void)shortcutActualSize{[self.surface actualViewport];}
// Full-screen desktop: hide the home indicator and let edge swipes reach the
// Linux desktop first (a second swipe still triggers the system gesture).
- (BOOL)prefersHomeIndicatorAutoHidden{return _desktopMode;}
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures{return _desktopMode?UIRectEdgeAll:UIRectEdgeNone;}
- (void)stopDesktopSession{if(_desktopMode)[self stop];}
- (void)connectDesktopViewer{
    uint16_t port=[[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"port"] encoding:NSUTF8StringEncoding error:nil] intValue];
    NSString *password=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"password"] encoding:NSUTF8StringEncoding error:nil];
    if(!port||!password.length){[self message:@"Desktop credentials unavailable. Stop Desktop and open again."];return;}
    if(_client){[self updateStartupStatus];return;}
    [self attachViewerWithPort:port password:password];
    if(_queuedDesktopApplication){NSDictionary *app=_queuedDesktopApplication;_queuedDesktopApplication=nil;[self launchDesktopApplication:app];}
}
- (void)drainFrameForClient:(LRRFBClient *)client {
    if(!client||_client!=client)return;
    [_frameLock lock];_presentationPending=NO;
    if(UIApplication.sharedApplication.applicationState==UIApplicationStateBackground){[_frameLock unlock];return;}
    uint16_t w=_pendingWidth,h=_pendingHeight;LRFrameDamage damage=_pendingDamage;double received=_pendingFrameReceivedAt;
    NSData *patch=LRFramebufferPatch(_pendingFrame,w,h,damage);
    _pendingDamage=(LRFrameDamage){0};_presentationPatchBytes+=patch.length;[_frameLock unlock];
    if(patch&&damage.width&&damage.height){
        double submitted=received?LRGUIClock():0;[_surface presentPatch:patch width:w height:h damage:damage];[_surface noteFramePresented];
        // Readiness depends on authenticated X11 component windows and actual
        // frame submission, never the wallpaper color or sampled pixel values.
        if(_desktopMode&&!_desktopPainted){_desktopPainted=YES;
            [LRRuntimeBridge desktopTrace:@"13 first-frame" result:@"ok" detail:@"frame submitted to the native surface; component readiness checked separately (GPU completion unmeasured)"];
        }
            if(_desktopMode&&[self externalOutputActive])[LRCurrentExternalDisplay presentFrame:_surface.latestPixels width:w height:h damage:damage];
        if(_desktopMode)[self resumeRequestedTrackpad];
        if(received&&_surface.traceInput&&_transportDiagnosticEvents<96){_transportDiagnosticEvents++;
            [LRRuntimeBridge logCategory:@"GUI_TRANSPORT" event:[NSString stringWithFormat:@"stage=native_present_submit frame=%llu native_monotonic_ms=%.3f newest_frame_queue_ms=%.3f submit_cpu_ms=%.3f patch_bytes=%lu (submission, not GPU completion)",(unsigned long long)_frames,submitted*1000,(submitted-received)*1000,(LRGUIClock()-submitted)*1000,(unsigned long)patch.length]];
            if(_transportDiagnosticEvents==96)_client.transportMetricsHandler=nil;
        }
        [self updateStartupStatus];
    }
}
- (void)attachViewerWithPort:(uint16_t)port password:(NSString *)password{
    if(_client)return;
    _desktopPainted=NO;_inputVerified=0;_inputChecks=0;_inputProbeAt=0;[_surface clearFrame];[_surface configurePresentation];_traceFirstUpdate=NO;
    if(_desktopMode){[LRRuntimeBridge desktopTrace:@"11 viewer" result:@"ok" detail:[NSString stringWithFormat:@"RFB client created port=%u",port]];
        [LRRuntimeBridge desktopTrace:@"12 renderer" result:_surface?@"ok":@"fail" detail:[NSString stringWithFormat:@"surface=%d metal=%d hidden=%d in-window=%d size=%.0fx%.0f",_surface!=nil,_surface.metal!=nil,_surface.hidden,_surface.window!=nil,_surface.bounds.size.width,_surface.bounds.size.height]];}
    _client=[[LRRFBClient alloc] initWithPort:port password:password];_surface.client=_client;_startedAt=NSDate.date;_frames=0;_bytes=0;_applicationWindowMapped=NO;
    [self updateStorageActivity];
    __weak typeof(self) weak=self;
    __weak LRRFBClient *thisClient=_client;
    _client.patchFrameHandler=^(NSData *patch,uint16_t w,uint16_t h,uint64_t frames,uint64_t bytes,LRFrameDamage damage){
        typeof(self) self=weak;if(!self)return;[self.frameLock lock];
        if(!self.client||self.client!=thisClient){[self.frameLock unlock];return;}
        size_t full;if(!lr_rfb_dimensions(w,h,&full)){[self.frameLock unlock];return;}
        if(!damage.x&&!damage.y&&damage.width==w&&damage.height==h) {
            if(patch.length!=full){[self.frameLock unlock];return;}
            self.pendingFrame=patch;self.pendingDamage=damage;
        } else {
            if(!self.pendingFrame||self.pendingWidth!=w||self.pendingHeight!=h){
                self.pendingFrame=[NSMutableData dataWithLength:full];self.pendingDamage=(LRFrameDamage){0,0,w,h};
            }else {
                self.pendingDamage=lr_damage_union(self.pendingDamage,damage);
                if(![self.pendingFrame isKindOfClass:NSMutableData.class])self.pendingFrame=[self.pendingFrame mutableCopy];
            }
            if(!LRApplyFramebufferPatch((NSMutableData *)self.pendingFrame,patch,w,h,damage)){[self.frameLock unlock];return;}
        }
        if(self.desktopMode&&!self.traceFirstUpdate){self.traceFirstUpdate=YES;[LRRuntimeBridge desktopTrace:@"13 first-frame" result:@"start" detail:[NSString stringWithFormat:@"first framebuffer update %ux%u bytes=%lu frames=%llu",w,h,(unsigned long)patch.length,frames]];}

        self.pendingFrameReceivedAt=self.client.transportMetricsHandler?LRGUIClock():0;
        self.patchBytes+=patch.length;self.pendingWidth=w;self.pendingHeight=h;self.frames=frames;self.bytes=bytes;
        BOOL schedule=!self.presentationPending;self.presentationPending=YES;[self.frameLock unlock];
        if(schedule)dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;if(self&&self.client==thisClient)[self drainFrameForClient:thisClient];});
    };
    _client.cursorHandler=^(NSData *bgra,uint16_t w,uint16_t h,uint16_t hx,uint16_t hy){dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;
        if(self&&self.client==thisClient)[self.surface setCursorShape:bgra width:w height:h hotX:hx hotY:hy];});};
    _client.pointerPositionHandler=^(uint16_t x,uint16_t y){dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;
        if(self&&self.client==thisClient){self.surface.pointerX=x;self.surface.pointerY=y;self.surface.mouseX=x;self.surface.mouseY=y;[self.surface updateCursor];}});};
    _client.cutTextHandler=^(NSString *text){dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;if(self&&self.client==thisClient)self.linuxClipboard=text;});};
    _client.statusHandler=^(NSString *status){dispatch_async(dispatch_get_main_queue(),^{typeof(self) self=weak;if(self.client&&self.client==thisClient){
        if(self.desktopMode)[LRRuntimeBridge desktopTrace:@"10 display-socket" result:[status hasPrefix:@"Connected"]?@"ok":[status hasPrefix:@"Display error:"]?@"fail":@"info" detail:status];
        [self message:status];if([status hasPrefix:@"Linux display ended"]||[status hasPrefix:@"Display error:"]){
        if(self.desktopMode)[self detachViewer];else [self stop];
        NSString *failure=ApplicationPreflightFailure(self.sessionURL);if(failure)[self message:failure];
        // The viewer gives up after ~30 s; a first start on the phone can take
        // longer. While the session runs, keep connecting (the startup
        // watchdog reports a display that never appears).
        // After start, the session restarts its display transport (x11vnc) while
        // the desktop keeps running: reconnect, at most 5 times a minute.
        if(self.desktopMode&&self.startState!=LRStartSession&&self.sessionTerminal.sessionPid>0&&[status hasPrefix:@"Linux display ended"]){
            if(!self.viewerReconnectWindow||-[self.viewerReconnectWindow timeIntervalSinceNow]>60){self.viewerReconnectWindow=NSDate.date;self.viewerReconnects=0;}
            if(self.viewerReconnects<5){self.viewerReconnects++;[self desktopStartLog:[NSString stringWithFormat:@"viewer reconnect %ld/5 (transport restarted; desktop running)",(long)self.viewerReconnects]];
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{typeof(self) self=weak;
                    if(self&&!self.client&&self.sessionTerminal.sessionPid>0)[self connectDesktopViewer];});}
        }
        if(self.desktopMode&&self.startState==LRStartSession&&self.sessionTerminal.sessionPid>0&&[status hasPrefix:@"Display error:"]){
            [self desktopStartLog:@"viewer retry"];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(2*NSEC_PER_SEC)),dispatch_get_main_queue(),^{typeof(self) self=weak;
                if(self&&self.startState==LRStartSession&&!self.client&&self.sessionTerminal.sessionPid>0)[self connectDesktopViewer];});
        }
    }}});};
    _surface.traceCount=0;
    [self configureTransportDiagnostics];
    for(UIView *control in _launchControls)control.hidden=YES;
    [_client start];
    if(_audioPort&&!_audio){_audio=[[LRAudioBridge alloc] initWithPort:_audioPort];[_audio start];}
    _metricsTimer=[NSTimer scheduledTimerWithTimeInterval:15 target:self selector:@selector(metrics) userInfo:nil repeats:YES];
    [self message:_desktopMode?@"Starting or reconnecting to LinuxRunner Desktop…":@"Starting Linux display…"];
}
- (void)updateStartupStatus{
    if(!_client)return;
    NSString *state=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"window-state"] encoding:NSUTF8StringEncoding error:nil];
    BOOL mapped=NO;for(NSString *line in [state componentsSeparatedByString:@"\n"]){
        if([line hasPrefix:@"mapped="]){mapped=[[line substringFromIndex:7] integerValue]>0;break;}
    }
    NSTimeInterval elapsed=-[_startedAt timeIntervalSinceNow];
    if(mapped&&!_applicationWindowMapped){
        _applicationWindowMapped=YES;
        [LRRuntimeBridge logCategory:@"X11_STARTUP" event:[NSString stringWithFormat:@"window_mapped=PASS elapsed=%.1fs first_paint=UNVERIFIED (mapping alone does not prove rendering)",elapsed]];
    }
    NSString *progress=mapped?@"Linux application window mapped":@"Waiting for Linux application window — keep this screen open";
    if(_desktopMode){NSString *desktopState=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil];progress=[desktopState containsString:@"ready=1"]&&_desktopPainted?(DesktopDegradedNotice(desktopState)?:@"Desktop ready"):@"Starting desktop components — keep this screen open";
        if([desktopState containsString:@"ready=1"]&&_desktopPainted&&_startState==LRStartSession){_startState=LRStartIdle;[self hideStartPanel];[self desktopStartLog:@"stage=painted components=verified"];}
        if([desktopState containsString:@"ready=1"]&&_desktopPainted)[self verifyDesktopInput];
        if(DesktopDegradedNotice(desktopState))progress=DesktopDegradedNotice(desktopState);
        if([desktopState containsString:@"ready=1"]&&_queuedDesktopApplication){NSDictionary *app=_queuedDesktopApplication;_queuedDesktopApplication=nil;[self launchDesktopApplication:app];}}
    _surface.accessibilityValue=[NSString stringWithFormat:@"%@; framebuffer %ux%u",progress,_surface.pixelWidth,_surface.pixelHeight];
    double now=LRGUIClock();
    lr_display_update_rate(now,_frames,&_rateTime,&_rateFrames,&_updateRate);
    _status.text=[NSString stringWithFormat:@"%@ · %.0fs · %.1f display updates/s",progress,elapsed,_updateRate];
}
- (void)metrics{
    [LRRuntimeBridge logCategory:@"GUI_PRESENTATION" event:[NSString stringWithFormat:@"component counters (guest rendering unchanged): %@",[self.surface presentationStatistics]]];
    [self updateStartupStatus];
    [_frameLock lock];uint64_t frames=_frames,bytes=_bytes,patchBytes=_patchBytes,submittedPatchBytes=_presentationPatchBytes;[_frameLock unlock];
    [LRRuntimeBridge logCategory:@"GUI_TRANSPORT" event:[NSString stringWithFormat:@"decoder_patch_bytes=%llu presenter_patch_bytes=%llu frames=%llu",(unsigned long long)patchBytes,(unsigned long long)submittedPatchBytes,(unsigned long long)frames]];
    [LRRuntimeBridge logCategory:@"GUI" event:[NSString stringWithFormat:@"frames=%llu wire-bytes=%llu elapsed=%.2fs resolution=%ux%u presenter=%@",(unsigned long long)frames,(unsigned long long)bytes,-[_startedAt timeIntervalSinceNow],_pendingWidth,_pendingHeight,_surface.metal?@"Metal":@"Core Graphics"]];
    if(_desktopMode&&[NSUserDefaults.standardUserDefaults boolForKey:@"LinuxRunnerDesktop.performanceDiagnostics"]){
        struct rusage usage={0};getrusage(RUSAGE_SELF,&usage);
        double cpu=(double)usage.ru_utime.tv_sec+usage.ru_utime.tv_usec*1e-6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec*1e-6;
        NSDictionary *sample=@{@"monotonic_s":@(LRGUIClock()),@"cpu_s":@(cpu),@"footprint_bytes":@([LRRuntimeBridge appFootprintBytes]),@"thermal_state":@(NSProcessInfo.processInfo.thermalState),@"frames":@(frames),@"framebuffer_pixels":@[@(_surface.pixelWidth),@(_surface.pixelHeight)],@"viewer_points":@[@(_surface.bounds.size.width),@(_surface.bounds.size.height)],@"screen_native_pixels":@[@(_surface.window.screen.nativeBounds.size.width),@(_surface.window.screen.nativeBounds.size.height)],@"screen_scale":@(_surface.window.screen.scale),@"screen_native_scale":@(_surface.window.screen.nativeScale),@"input":[_surface inputDiagnostics],@"presenter":[_surface presentationStatistics]};
        NSData *data=[NSJSONSerialization dataWithJSONObject:sample options:0 error:nil];
        if(data){[LRRuntimeBridge logCategory:@"DESKTOP_PERFORMANCE" event:[[NSString alloc]initWithData:data encoding:NSUTF8StringEncoding]];
            NSURL *url=[_sessionURL URLByAppendingPathComponent:@"performance.jsonl"];
            int fd=open(url.fileSystemRepresentation,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0600);
            if(fd>=0){struct stat st;if(!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size<1024*1024){write(fd,data.bytes,data.length);write(fd,"\n",1);}close(fd);}
        }
    }
}
- (void)stop{
    [_audio stop];_audio=nil;
    for(UIView *control in _launchControls)control.hidden=_desktopMode;
    if(_client)[self metrics];
    if(_sessionURL)[[NSData data] writeToURL:[_sessionURL URLByAppendingPathComponent:@"stop"] atomically:YES];
    [_surface releaseInput];[self resetModifierButtons];[_client stop];_client.frameHandler=nil;_client.damageFrameHandler=nil;_client.patchFrameHandler=nil;_client.statusHandler=nil;_client.cutTextHandler=nil;_client.cursorHandler=nil;_client.pointerPositionHandler=nil;[_surface setCursorShape:nil width:0 height:0 hotX:0 hotY:0];_client=nil;_surface.client=nil;
    [self updateStorageActivity];
    [_metricsTimer invalidate];_metricsTimer=nil;
    [_surface resignFirstResponder];
    [_frameLock lock];_pendingFrame=nil;_pendingDamage=(LRFrameDamage){0};_pendingWidth=_pendingHeight=0;_presentationPending=NO;[_frameLock unlock];
    // x11vnc exits on disconnect; the session script traps and waits for only
    // its own children. Never signal guest init, shell sessions or whole runtime.
    [LRRuntimeBridge logCategory:@"GUI" event:@"display disconnected; session cleanup requested"];
}
static const NSInteger LRAccessoryKeyTag=7701;
- (void)resetModifierButtons{for(UIButton *button in _modifierButtons){button.selected=NO;[self styleModifierButton:button];}}
- (void)styleModifierButton:(UIButton *)button{
    if(button.tag==LRAccessoryKeyTag){button.backgroundColor=button.selected?UIColor.systemBlueColor:UIColor.tertiarySystemFillColor;[button setTitleColor:button.selected?UIColor.whiteColor:UIColor.labelColor forState:UIControlStateNormal];}
    else [button setTitleColor:button.selected?UIColor.systemRedColor:UIColor.systemBlueColor forState:UIControlStateNormal];
}
static UIButton *LRSquareButton(NSString *symbol,NSString *label,UIColor *tint){
    UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];
    UIImageSymbolConfiguration *c=[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightSemibold];
    [b setImage:[UIImage systemImageNamed:symbol withConfiguration:c] forState:UIControlStateNormal];
    b.tintColor=tint;b.backgroundColor=[tint colorWithAlphaComponent:.16];b.layer.cornerRadius=9;b.layer.cornerCurve=kCACornerCurveContinuous;
    b.accessibilityLabel=label;b.translatesAutoresizingMaskIntoConstraints=NO;
    [NSLayoutConstraint activateConstraints:@[[b.widthAnchor constraintEqualToConstant:34],[b.heightAnchor constraintEqualToConstant:34]]];
    return b;
}
// DeX, Settings and Power as icon tiles; everything else under "more".
- (void)installDesktopToolbar{
    __weak typeof(self) weak=self;
    _dexButton=LRSquareButton(@"display",@"DeX Mode",UIColor.systemIndigoColor);_dexButton.accessibilityIdentifier=@"Desktop DeX";
    [_dexButton addAction:[UIAction actionWithHandler:^(UIAction *a){[weak performDesktopAction:@"touchpad"];[weak refreshDesktopToolbar];}] forControlEvents:UIControlEventTouchUpInside];
    UIButton *settings=LRSquareButton(@"gearshape.fill",@"Desktop Settings",UIColor.systemGrayColor);settings.accessibilityIdentifier=@"Desktop Settings";
    [settings addAction:[UIAction actionWithHandler:^(UIAction *a){typeof(self) self=weak;if(!self)return;[self.view endEditing:YES];[self.surface releaseInput];[self resetModifierButtons];
        UIViewController *page=[LinuxRunnerShell makeDesktopSettingsWithController:self];page.modalPresentationStyle=UIModalPresentationPageSheet;[self presentViewController:page animated:YES completion:nil];}] forControlEvents:UIControlEventTouchUpInside];
    UIButton *power=LRSquareButton(@"power",@"Exit",UIColor.systemRedColor);power.accessibilityIdentifier=@"Desktop Power";
    UIAction *stop=[UIAction actionWithTitle:@"Stop Desktop…" image:[UIImage systemImageNamed:@"power"] identifier:nil handler:^(UIAction *a){[weak confirmStopDesktop];}];
    stop.attributes=UIMenuElementAttributesDestructive;
    power.menu=[UIMenu menuWithTitle:@"Apps keep running when you return to Home." children:@[
        [UIAction actionWithTitle:@"Return to Home" image:[UIImage systemImageNamed:@"house"] identifier:nil handler:^(UIAction *a){[weak done];}],stop]];
    power.showsMenuAsPrimaryAction=YES;
    _moreButton=LRSquareButton(@"ellipsis",@"More Desktop Actions",UIColor.systemBlueColor);_moreButton.accessibilityIdentifier=@"Desktop More";
    [_moreButton addTarget:self action:@selector(desktopOptions:) forControlEvents:UIControlEventTouchUpInside];
    UIStackView *row=[[UIStackView alloc] initWithArrangedSubviews:@[_moreButton,_dexButton,settings,power]];row.spacing=8;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithCustomView:row];
    _powerMenu=power.menu;   // the dock is built once the surface exists (installKeyboardControls)
    [self refreshDesktopToolbar];
}
/// Large labelled controls above the portrait touchpad.
- (UIButton *)dockButton:(NSString *)symbol title:(NSString *)title tint:(UIColor *)tint stack:(UIStackView *)stack{
    UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];
    [b setImage:[UIImage systemImageNamed:symbol withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:19 weight:UIImageSymbolWeightMedium]] forState:UIControlStateNormal];
    b.tintColor=tint;b.accessibilityLabel=title;b.translatesAutoresizingMaskIntoConstraints=NO;
    b.layer.cornerRadius=20;b.layer.cornerCurve=kCACornerCurveContinuous;
    [NSLayoutConstraint activateConstraints:@[[b.widthAnchor constraintEqualToConstant:44],[b.heightAnchor constraintEqualToConstant:40]]];
    [stack addArrangedSubview:b];
    return b;
}- (UIMenu *)clipboardMenu{
    __weak typeof(self) weak=self;
    UIAction *paste=[UIAction actionWithTitle:@"Paste into Linux" image:[UIImage systemImageNamed:@"doc.on.clipboard"] identifier:nil handler:^(UIAction *a){[weak performDesktopAction:@"clipboard"];}];
    UIAction *copy=[UIAction actionWithTitle:@"Copy Linux Clipboard to iPhone" image:[UIImage systemImageNamed:@"square.and.arrow.down.on.square"] identifier:nil handler:^(UIAction *a){typeof(self) self=weak;if(!self)return;
        if(self.linuxClipboard.length){UIPasteboard.generalPasteboard.string=self.linuxClipboard;[self message:@"Copied the Linux clipboard to iPhone."];}
        else [self message:@"Nothing copied in Linux yet. Copy text there first (for example Ctrl+C)."];}];
    return [UIMenu menuWithTitle:@"" children:@[paste,copy]];
}
/// One pill of plain icons in the reference style: Keyboard, Clipboard, DeX,
/// Settings, More | Power. Portrait: between the desktop and the touchpad.
/// Landscape: floating at the top, foldable into a handle. DeX: at the bottom.
- (void)installDock:(UIMenu *)powerMenu{
    UIVisualEffectView *dock=[[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemThinMaterialDark]];
    dock.layer.cornerRadius=25;dock.layer.cornerCurve=kCACornerCurveContinuous;dock.clipsToBounds=YES;
    dock.layer.borderWidth=0.5;dock.layer.borderColor=[UIColor colorWithWhite:1 alpha:.14].CGColor;
    UIStackView *stack=[UIStackView new];stack.axis=UILayoutConstraintAxisHorizontal;stack.distribution=UIStackViewDistributionEqualSpacing;stack.alignment=UIStackViewAlignmentCenter;stack.translatesAutoresizingMaskIntoConstraints=NO;
    [dock.contentView addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[[stack.leadingAnchor constraintEqualToAnchor:dock.contentView.leadingAnchor constant:12],[stack.trailingAnchor constraintEqualToAnchor:dock.contentView.trailingAnchor constant:-12],[stack.centerYAnchor constraintEqualToAnchor:dock.contentView.centerYAnchor]]];
    __weak typeof(self) weak=self;
    UIColor *white=[UIColor colorWithWhite:1 alpha:.92];
    UIButton *keyboard=[self dockButton:@"keyboard" title:@"Keyboard" tint:white stack:stack];keyboard.accessibilityIdentifier=@"GUI Keyboard";
    [keyboard addTarget:self action:@selector(tool:) forControlEvents:UIControlEventTouchUpInside];
    UIButton *clipboard=[self dockButton:@"doc.on.clipboard" title:@"Clipboard" tint:white stack:stack];clipboard.accessibilityIdentifier=@"Dock Clipboard";
    clipboard.menu=[self clipboardMenu];clipboard.showsMenuAsPrimaryAction=YES;
    UIButton *dex=[self dockButton:@"display" title:@"DeX Mode" tint:white stack:stack];dex.accessibilityIdentifier=@"Dock DeX";dex.tag=LRDockPortraitOnlyTag;
    [dex addAction:[UIAction actionWithHandler:^(UIAction *a){[weak performDesktopAction:@"touchpad"];[weak refreshDesktopToolbar];}] forControlEvents:UIControlEventTouchUpInside];
    UIButton *settings=[self dockButton:@"gearshape" title:@"Settings" tint:white stack:stack];settings.accessibilityIdentifier=@"Dock Settings";settings.tag=LRDockPortraitOnlyTag;
    [settings addAction:[UIAction actionWithHandler:^(UIAction *a){typeof(self) self=weak;if(!self)return;[self.view endEditing:YES];[self.surface releaseInput];[self resetModifierButtons];
        UIViewController *page=[LinuxRunnerShell makeDesktopSettingsWithController:self];page.modalPresentationStyle=UIModalPresentationPageSheet;[self presentViewController:page animated:YES completion:nil];}] forControlEvents:UIControlEventTouchUpInside];
    UIButton *more=[self dockButton:@"ellipsis.circle" title:@"More" tint:white stack:stack];more.accessibilityIdentifier=@"Dock More";more.tag=LRDockPortraitOnlyTag;
    [more addAction:[UIAction actionWithHandler:^(UIAction *a){[weak desktopOptions:a.sender];}] forControlEvents:UIControlEventTouchUpInside];
    UIView *divider=[UIView new];divider.tag=LRDockPortraitOnlyTag;divider.backgroundColor=[UIColor colorWithWhite:1 alpha:.22];divider.translatesAutoresizingMaskIntoConstraints=NO;
    [NSLayoutConstraint activateConstraints:@[[divider.widthAnchor constraintEqualToConstant:1],[divider.heightAnchor constraintEqualToConstant:24]]];[stack addArrangedSubview:divider];
    UIButton *power=[self dockButton:@"power" title:@"Exit" tint:[UIColor colorWithRed:1 green:.42 blue:.40 alpha:1] stack:stack];power.accessibilityIdentifier=@"Dock Power";power.tag=LRDockPortraitOnlyTag;
    power.menu=powerMenu;power.showsMenuAsPrimaryAction=YES;
    // Landscape: fold the pill away; a small handle brings it back.
    UIButton *fold=[self dockButton:@"chevron.up" title:@"Hide Controls" tint:[UIColor colorWithWhite:1 alpha:.6] stack:stack];fold.accessibilityIdentifier=@"Dock Fold";fold.tag=LRDockFoldTag;
    [fold addAction:[UIAction actionWithHandler:^(UIAction *a){typeof(self) self=weak;self.surface.dockCollapsed=YES;[self.surface setNeedsLayout];}] forControlEvents:UIControlEventTouchUpInside];
    UIButton *handle=[UIButton buttonWithType:UIButtonTypeSystem];handle.accessibilityLabel=@"Show Desktop Controls";handle.accessibilityIdentifier=@"Dock Handle";
    [handle setImage:[UIImage systemImageNamed:@"chevron.compact.down" withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightBold]] forState:UIControlStateNormal];
    handle.tintColor=UIColor.whiteColor;handle.backgroundColor=[UIColor colorWithWhite:.08 alpha:.72];handle.layer.cornerRadius=11;handle.hidden=YES;
    [handle addAction:[UIAction actionWithHandler:^(UIAction *a){typeof(self) self=weak;self.surface.dockCollapsed=NO;[self.surface setNeedsLayout];}] forControlEvents:UIControlEventTouchUpInside];
    _dockDexButton=dex;_dockFoldButton=fold;
    _surface.dock=dock;_surface.dockHandle=handle;_surface.dockKeyboardButton=keyboard;[_surface addSubview:dock];[_surface addSubview:handle];dock.hidden=YES;
    // The pill carries every desktop control: no navigation bar in the desktop.
    _surface.dockVisibilityChanged=^(BOOL shown){typeof(self) self=weak;if(!self)return;
        [self.navigationController setNavigationBarHidden:shown||self.immersive animated:YES];self.status.hidden=shown||self.immersive;};
}- (void)refreshDesktopToolbar{
    BOOL on=_surface.relativeTouchpad||_trackpadRequested;
    _dexButton.backgroundColor=on?UIColor.systemIndigoColor:[UIColor.systemIndigoColor colorWithAlphaComponent:.16];_dexButton.tintColor=on?UIColor.whiteColor:UIColor.systemIndigoColor;
    _dexButton.accessibilityValue=on?@"On":@"Off";
    _dockDexButton.backgroundColor=on?[UIColor colorWithWhite:1 alpha:.22]:UIColor.clearColor;
}
- (void)confirmStopDesktop{
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Stop Desktop?" message:@"Save documents first. This closes the apps in this desktop. Linux files, the terminal and other sessions are kept." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Stop Desktop" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *a){[self stopDesktopSession];[self dismissViewControllerAnimated:YES completion:nil];}]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)installKeyboardControls{
    _surface.desktopControls=YES;
    UIButton *k=[UIButton buttonWithType:UIButtonTypeSystem];k.accessibilityIdentifier=@"GUI Keyboard";k.accessibilityLabel=@"Keyboard";
    [k setImage:[UIImage systemImageNamed:@"keyboard" withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:17 weight:UIImageSymbolWeightMedium]] forState:UIControlStateNormal];
    k.tintColor=UIColor.whiteColor;k.backgroundColor=[UIColor colorWithWhite:.18 alpha:.85];k.layer.cornerRadius=11;k.layer.cornerCurve=kCACornerCurveContinuous;
    [k addTarget:self action:@selector(tool:) forControlEvents:UIControlEventTouchUpInside];
    _surface.keyboardButton=k;[_surface addSubview:k];
    // Desktop: one compact, horizontally scrolling row of Linux keys above the
    // iOS keyboard (the two-row classic layout belongs to the terminal).
    UIInputView *bar=[[UIInputView alloc] initWithFrame:CGRectMake(0,0,320,48) inputViewStyle:UIInputViewStyleKeyboard];
    bar.allowsSelfSizing=NO;bar.autoresizingMask=UIViewAutoresizingFlexibleWidth;
    UIScrollView *scroll=[UIScrollView new];scroll.showsHorizontalScrollIndicator=NO;scroll.translatesAutoresizingMaskIntoConstraints=NO;[bar addSubview:scroll];
    UIStackView *keys=[UIStackView new];keys.spacing=6;keys.translatesAutoresizingMaskIntoConstraints=NO;[scroll addSubview:keys];
    UIButton *(^chip)(void)=^UIButton *{UIButton *b=[UIButton buttonWithType:UIButtonTypeSystem];b.tag=LRAccessoryKeyTag;
        b.backgroundColor=UIColor.tertiarySystemFillColor;b.layer.cornerRadius=8;b.layer.cornerCurve=kCACornerCurveContinuous;b.tintColor=UIColor.labelColor;
        b.contentEdgeInsets=UIEdgeInsetsMake(0,11,0,11);[b.heightAnchor constraintEqualToConstant:36].active=YES;[b.widthAnchor constraintGreaterThanOrEqualToConstant:42].active=YES;return b;};
    UIButton *clip=chip();[clip setImage:[UIImage systemImageNamed:@"doc.on.clipboard" withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightSemibold]] forState:UIControlStateNormal];
    clip.accessibilityLabel=@"Clipboard";clip.accessibilityIdentifier=@"GUI Clipboard";clip.menu=[self clipboardMenu];clip.showsMenuAsPrimaryAction=YES;[keys addArrangedSubview:clip];
    for(NSString *title in @[@"Esc",@"Tab",@"Ctrl",@"Alt",@"Super",@"Shift",@"←",@"↑",@"↓",@"→",@"Home",@"End",@"PgUp",@"PgDn"]){
        UIButton *b=chip();[b setTitle:title forState:UIControlStateNormal];
        b.titleLabel.font=[UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];[b setTitleColor:UIColor.labelColor forState:UIControlStateNormal];
        b.accessibilityIdentifier=[@"GUI " stringByAppendingString:title];
        if([@[@"Shift",@"Ctrl",@"Alt",@"Super"] containsObject:title])[_modifierButtons addObject:b];
        [b addTarget:self action:@selector(tool:) forControlEvents:UIControlEventTouchUpInside];[keys addArrangedSubview:b];
    }
    UIButton *hide=[UIButton buttonWithType:UIButtonTypeSystem];hide.accessibilityIdentifier=@"GUI Hide";hide.accessibilityLabel=@"Hide Keyboard";
    [hide setImage:[UIImage systemImageNamed:@"keyboard.chevron.compact.down"] forState:UIControlStateNormal];hide.translatesAutoresizingMaskIntoConstraints=NO;
    [hide addTarget:self action:@selector(tool:) forControlEvents:UIControlEventTouchUpInside];[bar addSubview:hide];
    [NSLayoutConstraint activateConstraints:@[
        [hide.trailingAnchor constraintEqualToAnchor:bar.trailingAnchor constant:-8],[hide.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],[hide.widthAnchor constraintEqualToConstant:40],
        [scroll.leadingAnchor constraintEqualToAnchor:bar.leadingAnchor],[scroll.trailingAnchor constraintEqualToAnchor:hide.leadingAnchor constant:-4],[scroll.topAnchor constraintEqualToAnchor:bar.topAnchor],[scroll.bottomAnchor constraintEqualToAnchor:bar.bottomAnchor],
        [keys.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:8],[keys.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-8],
        [keys.centerYAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.centerYAnchor],[keys.heightAnchor constraintEqualToConstant:36]]];
    _surface.keyAccessory=bar;
    if(_powerMenu)[self installDock:_powerMenu];
}
- (void)background{if(_desktopMode)[LRRuntimeBridge desktopTrace:@"lifecycle" result:@"info" detail:[NSString stringWithFormat:@"app inactive start-state=%ld",(long)_startState]];[_surface releaseInput];[self resetModifierButtons];[_surface resignFirstResponder];[LRRuntimeBridge logCategory:@"GUI" event:@"scene inactive; input released; guest session retained"];
    // Avoid retaining a screen containing guest data in iOS's app switcher.
    _surface.hidden=YES;
}
- (void)foreground{if(_desktopMode)[LRRuntimeBridge desktopTrace:@"lifecycle" result:@"info" detail:[NSString stringWithFormat:@"app active start-state=%ld client=%d session-pid=%d",(long)_startState,_client!=nil,_sessionTerminal.sessionPid]];_surface.hidden=NO;if(_viewerVisible)[_surface focusKeyboard];if(_client)[self drainFrameForClient:_client];if(_surface.metal)[_surface.metal setNeedsDisplay];if(_desktopMode&&!_client&&_sessionTerminal.sessionPid>0&&![NSFileManager.defaultManager fileExistsAtPath:[_sessionURL URLByAppendingPathComponent:@"stop"].path])[self connectDesktopViewer];}
- (void)tool:(UIButton *)button{
    NSString *title=[button.accessibilityIdentifier substringFromIndex:4];
        if([title isEqual:@"Keyboard"]){_surface.softwareKeyboardRequested=!_surface.softwareKeyboardRequested;[_surface focusKeyboard];return;}
    if([title isEqual:@"Hide"]){_surface.softwareKeyboardRequested=NO;return;}
    if([title isEqual:@"Close"]&&_desktopMode){[self done];return;}
    if([title isEqual:@"Close"]){[self stop];[self message:@"Display closed. CLI remains functional."];return;}
    NSDictionary *modifiers=@{@"Shift":@(UIKeyModifierShift),@"Ctrl":@(UIKeyModifierControl),@"Alt":@(UIKeyModifierAlternate),@"Super":@(UIKeyModifierCommand)};
    if(modifiers[title]){[_surface toggleModifier:[modifiers[title] unsignedIntegerValue]];button.selected=!button.selected;[self styleModifierButton:button];
        for(UIButton *other in _modifierButtons)if(other!=button&&[other.accessibilityIdentifier isEqual:button.accessibilityIdentifier]){other.selected=button.selected;[self styleModifierButton:other];}
        return;}
    NSDictionary *symbols=@{@"Esc":@0xff1b,@"Tab":@0xff09,@"←":@0xff51,@"↑":@0xff52,@"↓":@0xff54,@"→":@0xff53,@"Home":@0xff50,@"End":@0xff57,@"PgUp":@0xff55,@"PgDn":@0xff56,@"/":@0x2f,@"-":@0x2d};if(symbols[title])[_surface stroke:[symbols[title] unsignedIntValue]];
}
- (void)detachViewer{
    [_surface releaseInput];[self resetModifierButtons];[_surface resignFirstResponder];
    _client.frameHandler=nil;_client.damageFrameHandler=nil;_client.patchFrameHandler=nil;_client.statusHandler=nil;_client.cutTextHandler=nil;_client.cursorHandler=nil;_client.pointerPositionHandler=nil;[_surface setCursorShape:nil width:0 height:0 hotX:0 hotY:0];[_client stop];_client=nil;_surface.client=nil;
    [self updateStorageActivity];
    [_metricsTimer invalidate];_metricsTimer=nil;[_frameLock lock];_pendingFrame=nil;_pendingDamage=(LRFrameDamage){0};_pendingWidth=_pendingHeight=0;_presentationPending=NO;[_frameLock unlock];
    [LRRuntimeBridge logCategory:@"DESKTOP" event:@"viewer detached; owned guest desktop retained within iOS lifecycle limits"];
}
- (void)launchDesktopApplication:(NSDictionary *)config{
    if(config[@"requestID"]&&[_lastLaunchRequest isEqual:config[@"requestID"]])return;
    if(!_sessionURL||_sessionTerminal.sessionPid<=0){_queuedDesktopApplication=[config copy];return;}
    // A starting desktop has not yet given its session directory to the desktop
    // user, so publishing as that user failed with a permission error. Requests
    // are only served once the desktop is ready; hold it until then.
    if(_desktopMode&&![[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil] containsString:@"ready=1"]){
        _queuedDesktopApplication=[config copy];[LRRuntimeBridge logCategory:@"DESKTOP" event:@"launch request held until the desktop is ready"];
        if(!_queuedLaunchTimer){__weak typeof(self) weak=self;_queuedLaunchTimer=[NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer *timer){typeof(self) self=weak;
            if(!self||!self.sessionURL){[timer invalidate];return;}
            NSDictionary *held=self->_queuedDesktopApplication;if(!held){[timer invalidate];self.queuedLaunchTimer=nil;return;}
            if([[NSString stringWithContentsOfURL:[self.sessionURL URLByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil] containsString:@"ready=1"]){
                [timer invalidate];self.queuedLaunchTimer=nil;self->_queuedDesktopApplication=nil;[self launchDesktopApplication:held];}}];}
        return;}
    NSString *selected=LRRuntimeBridge.defaultLinuxUser;
    if(_sessionGuestUser && ![_sessionGuestUser isEqual:selected]){[self message:[NSString stringWithFormat:@"This desktop belongs to %@, but the selected login is %@. Save your work, stop the old desktop from Sessions, then open a new desktop. The application was not launched under the old identity.",_sessionGuestUser,selected]];return;}
    NSString *executable=config[@"executable"],*directory=config[@"directory"];
    if(![executable hasPrefix:@"/"]||![directory hasPrefix:@"/"]||![config[@"arguments"] isKindOfClass:NSArray.class]){[self message:@"Invalid desktop application configuration"];return;}
    NSMutableString *command=[NSMutableString stringWithString:@"#!/bin/sh\nset -eu\n"];
    NSRegularExpression *key=[NSRegularExpression regularExpressionWithPattern:@"^[A-Za-z_][A-Za-z0-9_]*$" options:0 error:nil];
    for(NSString *name in config[@"environment"]){if(![key firstMatchInString:name options:0 range:NSMakeRange(0,name.length)]||! [config[@"environment"][name] isKindOfClass:NSString.class]||[@[@"DISPLAY",@"XAUTHORITY",@"XDG_RUNTIME_DIR"] containsObject:name]){[self message:@"Invalid or reserved environment variable"];return;}if([name isEqual:@"LR_GUI_GPU_MODE"]&&[config[@"environment"][name] isEqual:@"mesa"])continue;/* legacy implicit default: follow Settings › Graphics */[command appendFormat:@"export %@=%@\n",name,ShellQuote(config[@"environment"][name])];}
    [command appendFormat:@"exec /bin/sh %@ %@ %@",ShellQuote([NSString stringWithFormat:@"/home/mobile/Downloads/%@/launch.sh",LRGUIFolder]),ShellQuote(directory),ShellQuote(executable)];
    for(id arg in config[@"arguments"]){if(![arg isKindOfClass:NSString.class])return;[command appendFormat:@" %@",ShellQuote(arg)];}[command appendString:@"\n"];
    if([command lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>8192||[command rangeOfString:[NSString stringWithFormat:@"%C",(unichar)0]].location!=NSNotFound){[self message:@"Launch request exceeds bounds"];return;}
    NSString *sessionUser=_sessionGuestUser ?: @"root";
    NSString *name=[NSString stringWithFormat:@"launch-%@.sh",NSUUID.UUID.UUIDString.lowercaseString];
    NSString *guestPath=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/%@/requests/%@",LRGUIFolder,_sessionURL.lastPathComponent,name];
    NSError *error=nil;
    // Publish complete contents under a non-launchable name, then rename.
    if(![LRRuntimeBridge writeDesktopRequest:[command dataUsingEncoding:NSUTF8StringEncoding] guestPath:[guestPath stringByAppendingString:@".pending"] user:sessionUser error:&error]){[self message:error.localizedDescription];return;}
    NSURL *target=[[_sessionURL URLByAppendingPathComponent:@"requests"] URLByAppendingPathComponent:name];
    if(![LRRuntimeBridge publishDesktopRequest:guestPath user:sessionUser error:&error]){[self message:error.localizedDescription];return;}
    _lastLaunchRequest=config[@"requestID"];
    [LRRuntimeBridge logCategory:@"DESKTOP_LAUNCH" event:[NSString stringWithFormat:@"executable=%@ request=%@",executable,target.lastPathComponent]];
}
/// Ready means interactive: move the X pointer through RFB, then ask the X
/// server where its pointer is. KMS compositors draw their own pointer and are
/// probed by their own first-frame check instead.
- (void)verifyDesktopInput{
    if(_inputVerified!=0||!_client||!_sessionURL||!_surface.pixelWidth)return;
    NSString *environment=[NSString stringWithContentsOfURL:[_sessionURL URLByAppendingPathComponent:@"desktop-environment"] encoding:NSUTF8StringEncoding error:nil]?:@"";
    if([environment hasPrefix:@"hyprland"]||[environment hasPrefix:@"wayfire"]){_inputVerified=1;return;}
    NSURL *result=[_sessionURL URLByAppendingPathComponent:@"input-check"];
    CFTimeInterval now=CACurrentMediaTime();
    if(_inputProbeAt>0){
        NSString *text=[NSString stringWithContentsOfURL:result encoding:NSUTF8StringEncoding error:nil];
        NSString *expected=[NSString stringWithFormat:@"pointer=%u,%u",_inputProbeX,_inputProbeY];
        if([text containsString:expected]){_inputVerified=1;[self desktopStartLog:[NSString stringWithFormat:@"stage=input-verified pointer=%u,%u after=%.0fms",_inputProbeX,_inputProbeY,(now-_inputProbeAt)*1000]];
            [LRRuntimeBridge desktopTrace:@"16 input" result:@"ok" detail:[NSString stringWithFormat:@"X server reports the pointer the viewer sent (%@)",expected]];return;}
        if(now-_inputProbeAt<3)return;   // the request may still be running
        if(++_inputChecks>=3){_inputVerified=-1;[self desktopStartLog:[NSString stringWithFormat:@"stage=input-unverified expected=%@ reported=%@",expected,[text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]?:@"none"]];
            [LRRuntimeBridge desktopTrace:@"16 input" result:@"fail" detail:@"pointer round trip not confirmed"];return;}
    }
    // A one-pixel move the user will not notice, to a position we can recognise.
    uint16_t x=_surface.pointerX,y=_surface.pointerY;
    x=(uint16_t)MIN(_surface.pixelWidth-1,MAX(1,x+(x+1<_surface.pixelWidth?1:-1)));
    _surface.pointerX=x;_surface.pointerY=y;_surface.mouseX=x;_surface.mouseY=y;[_client pointerX:x y:y buttons:0];[_surface updateCursor];
    _inputProbeX=x;_inputProbeY=y;_inputProbeAt=now;
    [NSFileManager.defaultManager removeItemAtURL:result error:nil];
    NSString *session=[NSString stringWithFormat:@"/home/mobile/Downloads/%@/%@",LRGUIFolder,_sessionURL.lastPathComponent];
    NSString *command=[NSString stringWithFormat:@"#!/bin/sh\nsleep 0.3\n/home/mobile/Downloads/%@/desktop-probe --pointer > %@/input-check.new && mv %@/input-check.new %@/input-check\n",LRGUIFolder,session,session,session];
    NSString *name=[NSString stringWithFormat:@"input-%@.sh",NSUUID.UUID.UUIDString.lowercaseString];
    NSString *guestPath=[NSString stringWithFormat:@"%@/requests/%@",session,name];
    NSString *user=_sessionGuestUser?:@"root";NSError *error=nil;
    if(![LRRuntimeBridge writeDesktopRequest:[command dataUsingEncoding:NSUTF8StringEncoding] guestPath:[guestPath stringByAppendingString:@".pending"] user:user error:&error]||![LRRuntimeBridge publishDesktopRequest:guestPath user:user error:&error]){
        _inputVerified=-1;[self desktopStartLog:[@"stage=input-unverified request-failed " stringByAppendingString:error.localizedDescription?:@""]];}
}
- (void)setImmersive:(BOOL)hide{_immersive=hide;_inputToolbar.hidden=hide||_desktopMode;[_surface setNeedsLayout];_status.hidden=hide||_surface.dockShown;_immersiveButton.hidden=!hide||_surface.dock!=nil;   /* the pill has every control */[self.navigationController setNavigationBarHidden:hide||_surface.dockShown animated:YES];[self setNeedsStatusBarAppearanceUpdate];}
- (void)toggleImmersive{[self setImmersive:!_immersive];}
- (void)setImmersive:(BOOL)hide animated:(BOOL)animated{[self setImmersive:hide];}
- (void)viewWillTransitionToSize:(CGSize)size withTransitionCoordinator:(id<UIViewControllerTransitionCoordinator>)coordinator {
    [super viewWillTransitionToSize:size withTransitionCoordinator:coordinator];
    if(_desktopMode){id automatic=[NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerDesktop.autoFullscreen"];
        if(!automatic||[automatic boolValue])[coordinator animateAlongsideTransition:^(id<UIViewControllerTransitionCoordinatorContext>c){[self setImmersive:size.width>size.height];} completion:nil];}
}
- (NSDictionary *)desktopControlState {
    NSMutableDictionary *state=[[self desktopSessionSummary] mutableCopy];
    state[@"externalConnected"]=@(LRCurrentExternalDisplay!=nil);
    state[@"externalEnabled"]=@(LRExternalOutputEnabled());
    state[@"touchpad"]=@(_surface.relativeTouchpad);
    state[@"trackpadRequested"]=@(_trackpadRequested);
    state[@"trackpadReady"]=@([self trackpadReady]);
    state[@"externalAvailable"]=@([self externalDisplayAvailable]);
    state[@"presenter"]=_surface.metal?@"Metal framebuffer presenter":@"Core Graphics framebuffer presenter";
    state[@"immersive"]=@(_immersive);
    state[@"framebuffer"]=_surface.pixelWidth?[NSString stringWithFormat:@"%u × %u",_surface.pixelWidth,_surface.pixelHeight]:@"No frames received";
    state[@"fitGeometry"]=[self fitScreenGeometry];
    UIScreen *screen=LRCurrentExternalDisplay.view.window.screen?:self.view.window.screen;
    state[@"screenMaximumRefresh"]=screen?@(screen.maximumFramesPerSecond):@0;
    NSMutableSet *modes=[NSMutableSet new];for(UIScreenMode *mode in screen.availableModes)[modes addObject:[NSString stringWithFormat:@"%.0f × %.0f",mode.size.width,mode.size.height]];
    state[@"screenModes"]=[[modes allObjects] sortedArrayUsingSelector:@selector(compare:)];
    NSString *displayBackend=LRDiagnosticTailString(LRReadDiagnosticTail([_sessionURL URLByAppendingPathComponent:@"display-backend"],64));
    BOOL acceleratedDisplay=[[displayBackend stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqual:@"xorg-virgl"];
    state[@"displayServer"]=[state[@"alive"] boolValue]?(acceleratedDisplay?@"X11 / Xorg (virgl)":@"X11 / Xvfb"):@"No running display";
    NSString *readiness=LRDiagnosticTailString(LRReadDiagnosticTail([_sessionURL URLByAppendingPathComponent:@"desktop-state"],8192));
    NSString *environment=@"Not reported";
    for(NSString *line in [readiness componentsSeparatedByString:@"\n"])if([line hasPrefix:@"environment="])environment=[line substringFromIndex:12];
    if([readiness containsString:@"display_server=wayland"] && [state[@"alive"] boolValue])state[@"displayServer"]=@"Wayland / KWin";
    state[@"desktopEnvironment"]=environment;
    state[@"statusDetail"]=_status.text?:@"";
    return state;
}
- (void)performDesktopAction:(NSString *)action {
    if(!NSThread.isMainThread){__weak typeof(self) weak=self;dispatch_async(dispatch_get_main_queue(),^{[weak performDesktopAction:action];});return;}
    if([action isEqual:@"start"]){
        _resolution.selectedSegmentIndex=LRRuntimeBridge.desktopResolutionIndex;
        if(!_client)[self run:@"desktop"];
    } else if([action isEqual:@"stop"]){_trackpadRequested=NO;[_restartTimer invalidate];_restartTimer=nil;[self stop];[self message:@"Stopping desktop. Saved files remain in Linux."];}
    else if([action isEqual:@"restart"]){
        if(_restartTimer)return;
        [self stop];[self message:@"Waiting for the existing desktop to stop before restarting…"];
        __weak typeof(self) weak=self;NSDate *began=NSDate.date;
        _restartTimer=[NSTimer scheduledTimerWithTimeInterval:.25 repeats:YES block:^(NSTimer *timer){
            typeof(self) self=weak;if(!self){[timer invalidate];return;}
            if(self.sessionTerminal.sessionPid<=0){[timer invalidate];self.restartTimer=nil;[self performDesktopAction:@"start"];}
            else if(-[began timeIntervalSinceNow]>20){[timer invalidate];self.restartTimer=nil;[self message:@"The desktop is still stopping. Reconnect after it exits; another display server was not started."];}
        }];
    } else if([action isEqual:@"fit"])[_surface fitViewport];
    else if([action isEqual:@"actual"])[_surface actualViewport];
    else if([action isEqual:@"fullscreen"])[self toggleImmersive];
    else if([action isEqual:@"touchpad"]){
        if(_surface.relativeTouchpad||_trackpadRequested){_trackpadRequested=NO;[_surface releaseInput];[self resetModifierButtons];_surface.relativeTouchpad=NO;[self setImmersive:NO];return;}
        _trackpadRequested=YES;
        [NSUserDefaults.standardUserDefaults setBool:YES forKey:LRExternalDisplayEnabledKey];
        [self resumeRequestedTrackpad];
        if(!_surface.relativeTouchpad)[self message:[self externalDisplayAvailable]?@"External output is enabled. Starting or reconnecting your desktop; trackpad controls will open when its first frame arrives.":@"External output is enabled. Connect a USB-C display, or choose an AirPlay receiver in Control Center → Screen Mirroring. Trackpad controls will open when iOS provides a separate desktop output and the desktop is ready. Ordinary mirroring alone cannot show different content on the phone and receiver. Tap Cancel Trackpad Connection to cancel waiting."];
    }
    else if([action isEqual:@"wireless"])[self wirelessDisplayHelp];
    else if([action isEqual:@"install"])[self installScript:@"install-desktop.sh"];
    else if([action isEqual:@"clipboard"]){
        NSString *text=UIPasteboard.generalPasteboard.string;
        if(!text.length){[self message:@"The clipboard contains no text."];return;}
        if([text lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>16384){[self message:@"Clipboard text exceeds the 16 KiB input limit."];return;}
        [_surface insertText:text];
    }
}
- (BOOL)externalDisplayAvailable {
    if(LRCurrentExternalDisplay)return YES;
    for(UIScene *scene in UIApplication.sharedApplication.connectedScenes)
        if([scene.session.role isEqual:UIWindowSceneSessionRoleExternalDisplay]||[scene.session.role isEqual:UIWindowSceneSessionRoleExternalDisplayNonInteractive])return YES;
    return NO;
}
- (BOOL)trackpadReady {
    return [self externalOutputActive]&&_client&&_surface.pixelWidth&&_surface.pixelHeight;
}
- (void)resumeRequestedTrackpad {
    if(!_trackpadRequested)return;
    if([self trackpadReady]){_trackpadRequested=NO;[_surface releaseInput];[self resetModifierButtons];_surface.relativeTouchpad=YES;[self setImmersive:NO];}
    else if([self externalOutputActive]&&!_client&&_startState==LRStartIdle)[self performDesktopAction:@"start"];
}
- (UIAlertController *)desktopOptionsMenu {
    NSString *hint=_trackpadRequested?@"Waiting for external desktop output. You can cancel below.":nil;
    UIAlertController *menu=[UIAlertController alertControllerWithTitle:nil message:hint preferredStyle:UIAlertControllerStyleActionSheet];
    [menu addAction:[UIAlertAction actionWithTitle:@"Start / Reconnect Desktop" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){[self performDesktopAction:@"start"];}]];
    [menu addAction:[UIAlertAction actionWithTitle:_surface.relativeTouchpad?@"Show Desktop on This Device":_trackpadRequested?@"Cancel Trackpad Connection":@"Use iPhone as Trackpad" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){[self performDesktopAction:@"touchpad"];}]];
    for(NSArray *item in @[@[@"Wireless Display",@"wireless"],@[@"Paste Clipboard Text",@"clipboard"]]){
        UIAlertAction *action=[UIAlertAction actionWithTitle:item[0] style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){[self performDesktopAction:item[1]];}];
        if([item[1] isEqual:@"clipboard"])action.enabled=_client&&_surface.pixelWidth&&_surface.pixelHeight;
        [menu addAction:action];
    }
    [menu addAction:[UIAlertAction actionWithTitle:@"Task Manager" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){UIViewController *tasks=[LinuxRunnerShell makeTaskManagerController];tasks.modalPresentationStyle=UIModalPresentationPageSheet;[self presentViewController:tasks animated:YES completion:nil];}]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Desktop Settings…" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){UIViewController *settings=[LinuxRunnerShell makeDesktopSettingsWithController:self];settings.modalPresentationStyle=UIModalPresentationPageSheet;[self presentViewController:settings animated:YES completion:nil];}]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    return menu;
}
- (void)desktopOptions:(id)sender {
    [self.view endEditing:YES];[_surface releaseInput];[self resetModifierButtons];
    UIAlertController *menu=[self desktopOptionsMenu];
    if([sender isKindOfClass:UIBarButtonItem.class])menu.popoverPresentationController.barButtonItem=(UIBarButtonItem *)sender;
    else{UIView *anchor=[sender isKindOfClass:UIView.class]&&!((UIView *)sender).hidden?(UIView *)sender:(_moreButton?:self.view);menu.popoverPresentationController.sourceView=anchor;menu.popoverPresentationController.sourceRect=anchor.bounds;}
    [self presentViewController:menu animated:YES completion:nil];
}
- (void)wirelessDisplayHelp {
    UIAlertController *help=[UIAlertController alertControllerWithTitle:@"Wireless Linux Desktop" message:@"Connect this device and an AirPlay-compatible TV, Apple TV or Mac to the same Wi-Fi. Open iOS Control Center → Screen Mirroring and choose the receiver. Enter any passcode in iOS.\n\nWhen iOS supplies a separate display, enable External Desktop Output in Settings → Devices and use this menu’s touchpad option. Otherwise, use full-screen mirroring: the receiver shows this device’s view. Miracast-only receivers are not supported." preferredStyle:UIAlertControllerStyleAlert];
    [help addAction:[UIAlertAction actionWithTitle:@"Use Full-Screen Desktop" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){self.trackpadRequested=NO;self.surface.relativeTouchpad=NO;[self.surface fitViewport];[self setImmersive:YES];}]];
    [help addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:help animated:YES completion:nil];
}
- (void)logs {
    [self.view endEditing:YES];[_surface releaseInput];[self resetModifierButtons];
    UIViewController *logs=[LinuxRunnerShell makeDesktopLogsController];logs.modalPresentationStyle=UIModalPresentationPageSheet;
    [self presentViewController:logs animated:YES completion:nil];
}
@end
