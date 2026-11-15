#import <Foundation/Foundation.h>
#import "RFBProtocol.h"
NS_ASSUME_NONNULL_BEGIN
// Private, authenticated loopback display only. Does not access guest memory.
@interface LRRFBClient : NSObject
@property (copy,nullable) void (^frameHandler)(NSData *bgra,uint16_t width,uint16_t height,uint64_t frameCount,uint64_t receivedBytes);
// Optional damage-aware delivery. Legacy probes/software clients remain valid.
@property (copy,nullable) void (^damageFrameHandler)(NSData *bgra,uint16_t width,uint16_t height,uint64_t frameCount,uint64_t receivedBytes,LRFrameDamage damage);
// Compact dirty pixels, rows tightly packed at damage.width*4 bytes.
@property (copy,nullable) void (^patchFrameHandler)(NSData *patch,uint16_t width,uint16_t height,uint64_t frameCount,uint64_t receivedBytes,LRFrameDamage damage);
@property (copy,nullable) void (^statusHandler)(NSString *status);
/// The picture comes from the GPU scanout: incremental requests ask for a
/// 1x1 area so the server stops encoding the screen; cursor shape, pointer
/// position, resize and clipboard messages still arrive.
@property (atomic) BOOL minimalUpdates;
// Cursor pseudo-encoding: the server's current cursor as premultiplied BGRA
// (nil when the cursor is hidden) and its hotspot; the view draws it at the
// pointer it sends. PointerPos: the server moved the pointer (e.g. a warp).
// Both are called on the decoder queue.
@property (copy,nullable) void (^cursorHandler)(NSData *_Nullable bgra,uint16_t width,uint16_t height,uint16_t hotX,uint16_t hotY);
@property (copy,nullable) void (^pointerPositionHandler)(uint16_t x,uint16_t y);
/// ServerCutText: text Linux placed on its CLIPBOARD (Latin-1 per RFB). Kept by
/// the app until the user chooses to copy it to iOS; never written implicitly.
@property (copy,nullable) void (^cutTextHandler)(NSString *text);
@property (copy,nullable) void (^inputTraceHandler)(NSString *event);
// Opt-in stage timing, called on decoder/input queues. No keysyms or pixels.
// decode_delivery_ms includes receiving the update body and callback delivery;
// input_queue_ms/write_ms stop at socket write, not guest response/display.
@property (copy,nullable) void (^transportMetricsHandler)(NSDictionary *event);
- (instancetype)initWithPort:(uint16_t)port password:(NSString *)password;
- (void)start;
- (void)stop;
- (void)key:(uint32_t)keysym down:(BOOL)down;
- (void)typeKeysyms:(const uint32_t *)symbols count:(size_t)count;
- (void)pointerX:(uint16_t)x y:(uint16_t)y buttons:(uint8_t)buttons;
// Liveness after the app returns from the background: bytes received so far,
// and a non-incremental update request (the server must answer with a frame).
- (uint64_t)receivedBytes;
- (BOOL)requestFullUpdate;
@end
NS_ASSUME_NONNULL_END
