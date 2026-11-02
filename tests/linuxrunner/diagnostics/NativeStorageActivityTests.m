// Host ownership/transport policy fixture. UIKit is replaced by BaseController;
// the runner inserts the production lifecycle methods and a Swift bridge receiver.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import "StorageFixture-Swift.h"
#include <assert.h>
@interface LRGraphicalActivity : NSObject
+ (instancetype)shared;
- (void)setActive:(BOOL)active forSource:(NSUUID *)source;
@end
@interface BaseController : NSObject
@property id presentedViewController;
@property BaseController *navigationController;
@property(getter=isBeingDismissed) BOOL beingDismissed;
- (void)viewWillAppear:(BOOL)animated;
- (void)viewDidDisappear:(BOOL)animated;
@end
@implementation BaseController
- (void)viewWillAppear:(BOOL)animated {}
- (void)viewDidDisappear:(BOOL)animated {}
@end
@interface TestPresentation : NSObject
@property id image;
- (void)clearFrame;
@end
@implementation TestPresentation
- (void)clearFrame {}
@end
@interface TestSurface : NSObject
@property BOOL presentationSuppressed;
@property TestPresentation *metal;
@property TestPresentation *image;
@end
@implementation TestSurface
@end
@interface TestClient : NSObject
- (void)stop;
@end
@implementation TestClient
- (void)stop {}
@end
@interface LRGUIViewController : BaseController {
@public
 BOOL _viewerVisible,_desktopMode,_external;
 NSUUID *_storageActivitySource;
 TestClient *_client;
 TestSurface *_surface;
 id _exitObserver;
 NSTimer *_metricsTimer,*_restartTimer;
}
- (BOOL)externalOutputActive;
- (void)externalDisplayChanged:(NSNotification *)notification;
- (void)updateStorageActivity;
@end
@implementation LRGUIViewController
- (BOOL)externalOutputActive { return _external; }
// The presentation callback is the seam; viewDidDisappear and the predicate
// are production methods, including the settings-cover early return.
- (void)externalDisplayChanged:(NSNotification *)notification {}
// PRODUCTION_STORAGE_ACTIVITY_METHODS

@end
int main(void) { @autoreleasepool {
 LRGUIViewController *local=[LRGUIViewController new];
 [local viewWillAppear:NO];assert(LRStorageTestState.active);
 local->_viewerVisible=YES;[local updateStorageActivity];
 local.presentedViewController=[NSObject new];
 [local viewDidDisappear:NO];
 assert(local->_viewerVisible&&LRStorageTestState.active);
 local.presentedViewController=nil;
 LRGUIViewController *desktop=[LRGUIViewController new];
 desktop->_desktopMode=YES;desktop->_viewerVisible=YES;
 desktop->_external=YES;desktop->_client=[TestClient new];
 [desktop updateStorageActivity];
 [desktop viewDidDisappear:NO];assert(!desktop->_viewerVisible&&LRStorageTestState.active);
 [local viewDidDisappear:NO];
 assert(LRStorageTestState.active); // external survives local dismissal
 desktop->_external=NO;[desktop updateStorageActivity];assert(!LRStorageTestState.active);
 desktop->_external=YES;[desktop updateStorageActivity];assert(LRStorageTestState.active);
 desktop->_client=nil;[desktop updateStorageActivity];assert(!LRStorageTestState.active);
 desktop->_client=[TestClient new];[desktop updateStorageActivity];assert(LRStorageTestState.active);
 desktop=nil;local=nil;
 dispatch_async(dispatch_get_main_queue(),^{assert(!LRStorageTestState.active);puts("PASS: native appearance/disappearance, settings cover, external-only transport, disconnect/disable, detach, and deallocation cleanup");exit(0);});
 } dispatch_main(); }
