#import <UIKit/UIKit.h>
NS_ASSUME_NONNULL_BEGIN
@interface LRGUIViewController : UIViewController
#if TARGET_OS_SIMULATOR
+ (void)runExternalDisplayContract:(void (^)(NSDictionary *result))completion;
#endif
- (instancetype)initDesktop;
- (NSDictionary *)desktopSessionSummary;
/// Home's live preview: keep (or reconnect) this session's viewer while the
/// preview is visible; never starts a desktop.
- (void)setPreviewWanted:(BOOL)wanted;
/// The latest frame, only when a newer one arrived than `generation`.
- (nullable UIImage *)previewImageIfChangedSince:(uint64_t *)generation;
/// Native desktop settings use these bounded actions; no arbitrary shell command.
- (NSDictionary *)desktopControlState;
- (void)performDesktopAction:(NSString *)action;
- (void)stopDesktopSession;
- (void)launchDesktopApplication:(NSDictionary *)configuration;
+ (NSURL * _Nullable)prepareSupport:(NSError **)error;
/// Persistent workspace ownership keeps external output when its viewer closes.
+ (void)registerExternalDisplayForController:(UIViewController *)controller;
- (instancetype)initWithLaunchConfiguration:(NSDictionary *)configuration;
/// Graphics routing and per-process GPU statistics of the most recently started session.
+ (NSDictionary<NSString *, id> *)latestGraphicsSummary;
/// Compact Open Desktop diagnostic report (timeline, stages, guest/viewer/renderer state). No secrets.
+ (void)buildDesktopReport:(void (^)(NSString *text))completion;
/// Pointer sources seen (UIKit touches by type, hover, scroll, GCMouse), pointer mode and event-to-send latency.
+ (NSDictionary *)latestInputDiagnostics;
@end
/// Session graphics default (Settings): mesa, mesa-metal-resident, or mesa-metal-verify.
extern NSString *const LRGraphicsModeDefaultsKey;
extern NSString *const LRExternalDisplayEnabledKey;
NS_ASSUME_NONNULL_END
