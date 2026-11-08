#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Bluetooth LE serial links exposed to Linux as /dev/ttyBLE0-3 (a real tty:
// termios, poll, blocking reads, modem-line ioctls). Uses only public Core
// Bluetooth central APIs. iOS gives apps no generic USB host access, so USB
// serial chips (CH340, CP210x, FTDI) on Arduino/ESP32 boards cannot be opened;
// BLE UART firmware (ESP32 Nordic UART Service, HM-10 style modules) can.
extern NSNotificationName const LRBLESerialChangedNotification;

@interface LRBLESerial : NSObject
+ (instancetype)shared;
/// "unknown", "resetting", "unsupported", "unauthorized", "poweredOff", "poweredOn".
@property (readonly) NSString *bluetoothState;
@property (readonly) BOOL scanning;
/// Each: id, name, rssi (NSNumber), serial (BOOL: advertises a known UART service), profile.
@property (readonly) NSArray<NSDictionary<NSString *, id> *> *discovered;
/// Each: slot, path, name, id, state, profile, rx, tx (bytes), error.
@property (readonly) NSArray<NSDictionary<NSString *, id> *> *links;
- (void)startScan;
- (void)stopScan;
/// Connects into the first free /dev/ttyBLE slot; returns the slot or -1.
- (NSInteger)connect:(NSString *)identifier;
- (void)disconnectSlot:(NSInteger)slot;
@end

/// Registers the tty driver and creates /dev/ttyBLE0-3 in the active root.
void LRBLESerialInstall(void);

NS_ASSUME_NONNULL_END
