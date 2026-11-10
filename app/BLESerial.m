#import <CoreBluetooth/CoreBluetooth.h>
#include <stdatomic.h>
#import "BLESerial.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/path.h"
#include "util/sync.h"

NSNotificationName const LRBLESerialChangedNotification = @"LRBLESerialChanged";
#define LR_BLE_SLOTS 4

// ---- Known BLE UART profiles ------------------------------------------------
typedef struct { const char *name, *service, *rx, *tx; } LRBLEProfile; // rx: we write, tx: device notifies
static const LRBLEProfile LRProfiles[] = {
    {"Nordic UART", "6E400001-B5A3-F393-E0A9-E50E24DCCA9E", "6E400002-B5A3-F393-E0A9-E50E24DCCA9E", "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"},
    {"HM-10 / CC254x", "FFE0", "FFE1", "FFE1"},
    {"Microchip transparent UART", "49535343-FE7D-4AE5-8FA9-9FAFD205E455", "49535343-8841-43F4-A8D4-ECBE34729BB3", "49535343-1E4D-4BD9-BA61-23C647249616"},
};

// Read by the Linux tty driver, which runs with tty->lock held and must never
// wait on the bridge queue (that queue takes tty->lock to deliver input).
static _Atomic bool LRSlotConnected[LR_BLE_SLOTS];
static _Atomic uint32_t LRSlotModem[LR_BLE_SLOTS];

@interface LRBLELink : NSObject
@property NSInteger slot;
@property CBPeripheral *peripheral;
@property CBCharacteristic *rx, *tx;
@property NSString *profile, *error;
@property(nonatomic) NSString *state;
@property uint64_t rxBytes, txBytes;
@property NSMutableData *outgoing, *incoming;
@property BOOL withoutResponse, writeInFlight;
@end
@implementation LRBLELink
- (void)setState:(NSString *)state {
    _state = state;
    if (_slot >= 0 && _slot < LR_BLE_SLOTS) atomic_store(&LRSlotConnected[_slot], [state isEqual:@"connected"]);
}
@end

@interface LRBLESerial () <CBCentralManagerDelegate, CBPeripheralDelegate>
@property CBCentralManager *central;
@property dispatch_queue_t queue;
@property NSMutableDictionary<NSUUID *, NSMutableDictionary *> *seen;
@property NSMutableDictionary<NSUUID *, CBPeripheral *> *peripherals;
@property(readwrite) BOOL scanning;
@end

static LRBLELink *LRSlots[LR_BLE_SLOTS];   // guarded by the bridge queue
static void LRBLEWrite(int slot, const void *bytes, size_t length);
static void LRBLEDeliverPending(LRBLELink *link);

// ---- Linux tty driver ---------------------------------------------------------
static int ble_tty_open(struct tty *tty) {
    if (tty->num < 0 || tty->num >= LR_BLE_SLOTS) return _ENXIO;
    // Like a USB serial port with nothing plugged in.
    if (!atomic_load(&LRSlotConnected[tty->num])) return _ENODEV;
    // Serial lines start raw-ish at 115200 like most boards; programs set their own.
    tty->ispeed = tty->ospeed = 115200;
    return 0;
}
static int ble_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    (void) blocking;
    int slot = tty->num;
    if (slot < 0 || slot >= LR_BLE_SLOTS || !atomic_load(&LRSlotConnected[slot])) return _EIO;
    LRBLEWrite(slot, buf, len);
    return (int) len;
}
static int ble_tty_ioctl(struct tty *tty, int cmd, void *arg) {
    int slot = tty->num;
    if (slot < 0 || slot >= LR_BLE_SLOTS) return _ENOTTY;
    switch (cmd) {
        case TIOCMGET_:
            // BLE has no modem lines: DTR/RTS read back as Linux set them;
            // carrier, DSR and CTS are reported present while the link is up.
            *(dword_t *) arg = atomic_load(&LRSlotModem[slot]) | (atomic_load(&LRSlotConnected[slot]) ? (TIOCM_CAR_ | TIOCM_DSR_ | TIOCM_CTS_) : 0);
            return 0;
        case TIOCMSET_: atomic_store(&LRSlotModem[slot], *(dword_t *) arg & (TIOCM_DTR_ | TIOCM_RTS_)); return 0;
        case TIOCMBIS_: atomic_fetch_or(&LRSlotModem[slot], *(dword_t *) arg & (TIOCM_DTR_ | TIOCM_RTS_)); return 0;
        case TIOCMBIC_: atomic_fetch_and(&LRSlotModem[slot], ~(*(dword_t *) arg)); return 0;
        case TIOCEXCL_: case TIOCNXCL_: case TCXONC_: return 0;
        case TCSBRK_: case TCSBRKP_: return 0;   // drain: writes are queued in order
    }
    return _ENOTTY;
}
static struct tty_driver_ops ble_tty_ops = { .open = ble_tty_open, .write = ble_tty_write, .ioctl = ble_tty_ioctl };
DEFINE_TTY_DRIVER(lr_ble_serial_driver, &ble_tty_ops, TTY_BLE_MAJOR, LR_BLE_SLOTS);

void LRBLESerialInstall(void) {
    tty_drivers[TTY_BLE_MAJOR] = &lr_ble_serial_driver;
    for (int i = 0; i < LR_BLE_SLOTS; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/ttyBLE%d", i);
        generic_mknodat(AT_PWD, path, S_IFCHR | 0666, dev_make(TTY_BLE_MAJOR, i));
    }
}

/// Feed received bytes into the open tty (bridge queue). Bytes are kept if
/// the line discipline buffer is full and retried; nothing is dropped while
/// Linux has the port open. With the port closed, input is discarded, as a
/// real serial line would.
static void LRBLEDeliverPending(LRBLELink *link) {
    if (!link.incoming.length) return;
    lock(&ttys_lock);
    struct tty *tty = lr_ble_serial_driver.ttys[link.slot];
    if (tty) { lock(&tty->lock); tty->refcount++; unlock(&tty->lock); }
    unlock(&ttys_lock);
    if (!tty) { [link.incoming setLength:0]; return; }
    ssize_t done = tty_input(tty, link.incoming.bytes, link.incoming.length, false);
    lock(&ttys_lock); tty_release(tty); unlock(&ttys_lock);
    if (done > 0) [link.incoming replaceBytesInRange:NSMakeRange(0, MIN((NSUInteger) done, link.incoming.length)) withBytes:NULL length:0];
    if (link.incoming.length) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), LRBLESerial.shared.queue, ^{ LRBLEDeliverPending(link); });
    }
}

static void LRBLEHangup(int slot) {
    lock(&ttys_lock);
    struct tty *tty = lr_ble_serial_driver.ttys[slot];
    if (tty) { lock(&tty->lock); tty->refcount++; tty_hangup(tty); unlock(&tty->lock); }
    unlock(&ttys_lock);
    if (tty) { lock(&ttys_lock); tty_release(tty); unlock(&ttys_lock); }
}

// ---- Core Bluetooth central -------------------------------------------------------
@implementation LRBLESerial
+ (instancetype)shared {
    static LRBLESerial *shared; static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [LRBLESerial new]; });
    return shared;
}
- (instancetype)init {
    if ((self = [super init])) {
        _queue = dispatch_queue_create("linuxrunner.ble-serial", DISPATCH_QUEUE_SERIAL);
        _seen = [NSMutableDictionary new]; _peripherals = [NSMutableDictionary new];
    }
    return self;
}
- (void)ensureCentral {
    // Created on first use so the Bluetooth permission prompt appears only
    // when the user opens the Bluetooth serial screen.
    if (!_central) _central = [[CBCentralManager alloc] initWithDelegate:self queue:_queue options:@{CBCentralManagerOptionShowPowerAlertKey: @YES}];
}
- (void)changed { dispatch_async(dispatch_get_main_queue(), ^{ [NSNotificationCenter.defaultCenter postNotificationName:LRBLESerialChangedNotification object:self]; }); }
- (NSString *)bluetoothState {
    if (!_central) return @"unknown";
    switch (_central.state) {
        case CBManagerStatePoweredOn: return @"poweredOn";
        case CBManagerStatePoweredOff: return @"poweredOff";
        case CBManagerStateUnauthorized: return @"unauthorized";
        case CBManagerStateUnsupported: return @"unsupported";
        case CBManagerStateResetting: return @"resetting";
        default: return @"unknown";
    }
}
- (void)startScan {
    dispatch_async(_queue, ^{
        [self ensureCentral];
        self.scanning = YES;
        if (self.central.state == CBManagerStatePoweredOn)
            [self.central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
        [self changed];
    });
}
- (void)stopScan { dispatch_async(_queue, ^{ self.scanning = NO; [self.central stopScan]; [self changed]; }); }
- (NSArray *)discovered {
    __block NSArray *list;
    dispatch_sync(_queue, ^{
        list = [[self.seen allValues] sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            if ([a[@"serial"] boolValue] != [b[@"serial"] boolValue]) return [a[@"serial"] boolValue] ? NSOrderedAscending : NSOrderedDescending;
            return [b[@"rssi"] compare:a[@"rssi"]];
        }];
    });
    return list;
}
- (NSArray *)links {
    __block NSMutableArray *list = [NSMutableArray new];
    dispatch_sync(_queue, ^{
        for (int i = 0; i < LR_BLE_SLOTS; i++) { LRBLELink *l = LRSlots[i]; if (!l) continue;
            [list addObject:@{@"slot": @(i), @"path": [NSString stringWithFormat:@"/dev/ttyBLE%d", i], @"name": l.peripheral.name ?: @"Bluetooth device",
                              @"id": l.peripheral.identifier.UUIDString, @"state": l.state ?: @"", @"profile": l.profile ?: @"", @"rx": @(l.rxBytes), @"tx": @(l.txBytes), @"error": l.error ?: @""}]; }
    });
    return list;
}
- (NSInteger)connect:(NSString *)identifier {
    __block NSInteger slot = -1;
    dispatch_sync(_queue, ^{
        CBPeripheral *p = self.peripherals[[[NSUUID alloc] initWithUUIDString:identifier]];
        if (!p) return;
        for (int i = 0; i < LR_BLE_SLOTS; i++) if (LRSlots[i] && [LRSlots[i].peripheral.identifier isEqual:p.identifier]) {
            slot = i;
            // Same /dev/ttyBLE path after a drop: reconnect in place.
            if (![@[@"connected", @"connecting", @"discovering"] containsObject:LRSlots[i].state]) {
                LRSlots[i].state = @"connecting"; LRSlots[i].error = @""; p.delegate = self; [self.central connectPeripheral:p options:nil]; [self changed];
            }
            return;
        }
        for (int i = 0; i < LR_BLE_SLOTS; i++) if (!LRSlots[i]) { slot = i; break; }
        if (slot < 0) return;
        LRBLELink *link = [LRBLELink new]; link.slot = slot; link.peripheral = p; link.state = @"connecting";
        link.outgoing = [NSMutableData new]; link.incoming = [NSMutableData new]; atomic_store(&LRSlotModem[slot], TIOCM_DTR_ | TIOCM_RTS_);
        LRSlots[slot] = link; p.delegate = self;
        [self.central connectPeripheral:p options:nil];
        [self changed];
    });
    return slot;
}
- (void)disconnectSlot:(NSInteger)slot {
    dispatch_async(_queue, ^{
        if (slot < 0 || slot >= LR_BLE_SLOTS || !LRSlots[slot]) return;
        LRBLELink *link = LRSlots[slot]; link.state = @"disconnected"; LRSlots[slot] = nil;
        [self.central cancelPeripheralConnection:link.peripheral];
        LRBLEHangup((int) slot);
        [self changed];
    });
}
- (LRBLELink *)linkFor:(CBPeripheral *)p {
    for (int i = 0; i < LR_BLE_SLOTS; i++) if (LRSlots[i].peripheral == p) return LRSlots[i];
    return nil;
}
// CBCentralManagerDelegate
- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBManagerStatePoweredOn && _scanning) [central scanForPeripheralsWithServices:nil options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
    if (central.state != CBManagerStatePoweredOn) {
        for (int i = 0; i < LR_BLE_SLOTS; i++) if (LRSlots[i]) { LRSlots[i].state = @"disconnected"; LRSlots[i].error = @"Bluetooth is off"; LRBLEHangup(i); }
    }
    [self changed];
}
- (void)centralManager:(CBCentralManager *)central didDiscoverPeripheral:(CBPeripheral *)p advertisementData:(NSDictionary *)ad RSSI:(NSNumber *)rssi {
    NSArray<CBUUID *> *services = ad[CBAdvertisementDataServiceUUIDsKey] ?: @[];
    NSString *profile = @"";
    for (size_t i = 0; i < sizeof LRProfiles / sizeof *LRProfiles; i++)
        if ([services containsObject:[CBUUID UUIDWithString:@(LRProfiles[i].service)]]) { profile = @(LRProfiles[i].name); break; }
    NSString *name = p.name ?: ad[CBAdvertisementDataLocalNameKey];
    if (!name && !profile.length) return;   // anonymous beacons are not serial ports
    _peripherals[p.identifier] = p;
    _seen[p.identifier] = [@{@"id": p.identifier.UUIDString, @"name": name ?: @"Unnamed device", @"rssi": rssi, @"serial": @(profile.length > 0), @"profile": profile} mutableCopy];
    [self changed];
}
- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)p {
    LRBLELink *link = [self linkFor:p]; if (!link) { [central cancelPeripheralConnection:p]; return; }
    link.state = @"discovering"; [self changed];
    NSMutableArray *uuids = [NSMutableArray new];
    for (size_t i = 0; i < sizeof LRProfiles / sizeof *LRProfiles; i++) [uuids addObject:[CBUUID UUIDWithString:@(LRProfiles[i].service)]];
    [p discoverServices:uuids];
}
- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link) return;
    link.state = @"failed"; link.error = error.localizedDescription ?: @"Connection failed"; [self changed];
}
- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link) return;
    link.state = @"disconnected"; link.error = error.localizedDescription ?: @"";
    LRBLEHangup((int) link.slot);
    [self changed];
}
// CBPeripheralDelegate
- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link) return;
    for (CBService *service in p.services)
        for (size_t i = 0; i < sizeof LRProfiles / sizeof *LRProfiles; i++)
            if ([service.UUID isEqual:[CBUUID UUIDWithString:@(LRProfiles[i].service)]]) {
                link.profile = @(LRProfiles[i].name);
                [p discoverCharacteristics:@[[CBUUID UUIDWithString:@(LRProfiles[i].rx)], [CBUUID UUIDWithString:@(LRProfiles[i].tx)]] forService:service];
                return;
            }
    link.state = @"failed"; link.error = @"This device has no Bluetooth serial (UART) service LinuxRunner supports.";
    [_central cancelPeripheralConnection:p]; [self changed];
}
- (void)peripheral:(CBPeripheral *)p didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link) return;
    for (size_t i = 0; i < sizeof LRProfiles / sizeof *LRProfiles; i++) {
        if (![service.UUID isEqual:[CBUUID UUIDWithString:@(LRProfiles[i].service)]]) continue;
        for (CBCharacteristic *c in service.characteristics) {
            if ([c.UUID isEqual:[CBUUID UUIDWithString:@(LRProfiles[i].rx)]]) link.rx = c;
            if ([c.UUID isEqual:[CBUUID UUIDWithString:@(LRProfiles[i].tx)]]) link.tx = c;
        }
    }
    if (!link.rx || !link.tx) { link.state = @"failed"; link.error = @"The serial service is missing its data characteristics."; [_central cancelPeripheralConnection:p]; [self changed]; return; }
    link.withoutResponse = (link.rx.properties & CBCharacteristicPropertyWriteWithoutResponse) != 0;
    [p setNotifyValue:YES forCharacteristic:link.tx];
}
- (void)peripheral:(CBPeripheral *)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link || c != link.tx) return;
    if (error) { link.state = @"failed"; link.error = error.localizedDescription; [self changed]; return; }
    link.state = @"connected"; link.error = @""; [self changed];
}
- (void)peripheral:(CBPeripheral *)p didUpdateValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link || c != link.tx || !c.value.length) return;
    link.rxBytes += c.value.length;
    [link.incoming appendData:c.value];
    LRBLEDeliverPending(link);
}
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral *)p { LRBLELink *link = [self linkFor:p]; if (link) [self pump:link]; }
- (void)peripheral:(CBPeripheral *)p didWriteValueForCharacteristic:(CBCharacteristic *)c error:(NSError *)error {
    LRBLELink *link = [self linkFor:p]; if (!link) return;
    link.writeInFlight = NO;
    if (error) link.error = error.localizedDescription;
    [self pump:link];
}
/// Sends queued Linux output in order, in chunks the link accepts.
- (void)pump:(LRBLELink *)link {
    while (link.outgoing.length && [link.state isEqual:@"connected"]) {
        CBCharacteristicWriteType type = link.withoutResponse ? CBCharacteristicWriteWithoutResponse : CBCharacteristicWriteWithResponse;
        if (link.withoutResponse ? !link.peripheral.canSendWriteWithoutResponse : link.writeInFlight) return;
        NSUInteger max = MAX(20, [link.peripheral maximumWriteValueLengthForType:type]);
        NSData *chunk = [link.outgoing subdataWithRange:NSMakeRange(0, MIN(max, link.outgoing.length))];
        [link.outgoing replaceBytesInRange:NSMakeRange(0, chunk.length) withBytes:NULL length:0];
        [link.peripheral writeValue:chunk forCharacteristic:link.rx type:type];
        link.txBytes += chunk.length;
        if (!link.withoutResponse) { link.writeInFlight = YES; return; }
    }
}
@end

static void LRBLEWrite(int slot, const void *bytes, size_t length) {
    NSData *data = [NSData dataWithBytes:bytes length:length];
    dispatch_async(LRBLESerial.shared.queue, ^{
        LRBLELink *link = LRSlots[slot]; if (!link) return;
        [link.outgoing appendData:data];
        [LRBLESerial.shared pump:link];
    });
}
