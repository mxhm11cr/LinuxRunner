#import <CoreMIDI/CoreMIDI.h>
#include <stdatomic.h>
#import "MIDISerial.h"
#include "kernel/fs.h"
#include "kernel/errno.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "fs/path.h"
#include "util/sync.h"

NSNotificationName const LRMIDISerialChangedNotification = @"LRMIDISerialChanged";
#define LR_MIDI_SLOTS 4
#define LR_MIDI_CHUNK 48          // serial bytes per SysEx frame (56 encoded)
enum { CMD_DATA = 1, CMD_LINE = 2, CMD_MODEM = 3, CMD_HELLO = 4, CMD_BREAK = 5, CMD_PAUSE = 6, CMD_RESUME = 7 };
static const uint8_t LRHeader[] = {0xF0, 0x7D, 0x4C, 0x52};

#include "MIDIPacking.h"

// ---- Link state ----------------------------------------------------------------
@interface LRMIDILink : NSObject
@property NSInteger slot;
@property MIDIEndpointRef source, destination;
@property NSString *name, *state;
@property int version;
@property uint64_t rx, tx;
@property uint32_t baud;
@property BOOL paused;
@property NSMutableData *incoming, *outgoing;
@end
@implementation LRMIDILink
@end

static _Atomic bool LRMIDIConnected[LR_MIDI_SLOTS];
static LRMIDILink *LRMIDISlots[LR_MIDI_SLOTS];     // bridge queue only
static void LRMIDISend(int slot, uint8_t cmd, const uint8_t *payload, size_t length);
static void LRMIDIDeliver(LRMIDILink *link);

@interface LRMIDISerial ()
@property dispatch_queue_t queue;
@property MIDIClientRef client;
@property MIDIPortRef inPort, outPort;
@property NSMutableDictionary<NSNumber *, NSMutableData *> *sysex;    // per source uniqueID
@property NSMutableArray<NSDictionary *> *knownDevices;
@property BOOL rescanPending;
@end

// ---- Linux tty driver ------------------------------------------------------------
static uint32_t LRDecodeBaud(struct tty *tty) {
    // termios2 (TCSETS2) carries the number; legacy TCSETS encodes Bxxxx in cflags.
    static const uint32_t table[] = {0,50,75,110,134,150,200,300,600,1200,1800,2400,4800,9600,19200,38400};
    static const uint32_t extended[] = {57600,115200,230400,460800,500000,576000,921600,1000000,1152000,1500000,2000000,2500000,3000000,3500000,4000000};
    uint32_t cbaud = tty->termios.cflags & 010017;
    if (cbaud == 010000 /* BOTHER */) return tty->ospeed;
    if (cbaud & 010000) { uint32_t i = (cbaud & 017) - 1; return i < sizeof extended / sizeof *extended ? extended[i] : tty->ospeed; }
    return table[cbaud & 017] ? table[cbaud & 017] : tty->ospeed;
}
static int midi_tty_open(struct tty *tty) {
    if (tty->num < 0 || tty->num >= LR_MIDI_SLOTS || !atomic_load(&LRMIDIConnected[tty->num])) return _ENODEV;
    tty->ispeed = tty->ospeed = 115200;
    return 0;
}
static int midi_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    (void) blocking;
    int slot = tty->num;
    if (slot < 0 || slot >= LR_MIDI_SLOTS || !atomic_load(&LRMIDIConnected[slot])) return _EIO;
    NSData *data = [NSData dataWithBytes:buf length:len];
    dispatch_async(LRMIDISerial.shared.queue, ^{
        LRMIDILink *link = LRMIDISlots[slot]; if (!link) return;
        [link.outgoing appendData:data];
        while (link.outgoing.length && !link.paused) {
            size_t n = MIN((size_t) LR_MIDI_CHUNK, link.outgoing.length);
            LRMIDISend(slot, CMD_DATA, link.outgoing.bytes, n);
            link.tx += n;
            [link.outgoing replaceBytesInRange:NSMakeRange(0, n) withBytes:NULL length:0];
        }
    });
    return (int) len;
}
static _Atomic uint32_t LRMIDIModem[LR_MIDI_SLOTS];
static void LRMIDISendModem(int slot) {
    uint8_t bits = (atomic_load(&LRMIDIModem[slot]) & TIOCM_DTR_ ? 1 : 0) | (atomic_load(&LRMIDIModem[slot]) & TIOCM_RTS_ ? 2 : 0);
    dispatch_async(LRMIDISerial.shared.queue, ^{ if (LRMIDISlots[slot]) LRMIDISend(slot, CMD_MODEM, &bits, 1); });
}
static int midi_tty_ioctl(struct tty *tty, int cmd, void *arg) {
    int slot = tty->num;
    if (slot < 0 || slot >= LR_MIDI_SLOTS) return _ENOTTY;
    switch (cmd) {
        case TIOCMGET_: *(dword_t *) arg = atomic_load(&LRMIDIModem[slot]) | (atomic_load(&LRMIDIConnected[slot]) ? (TIOCM_CAR_ | TIOCM_DSR_ | TIOCM_CTS_) : 0); return 0;
        // DTR/RTS reach the adapter, which drives its reset/boot GPIOs: this is
        // what lets esptool and avrdude reset a board into its bootloader.
        case TIOCMSET_: atomic_store(&LRMIDIModem[slot], *(dword_t *) arg & (TIOCM_DTR_ | TIOCM_RTS_)); LRMIDISendModem(slot); return 0;
        case TIOCMBIS_: atomic_fetch_or(&LRMIDIModem[slot], *(dword_t *) arg & (TIOCM_DTR_ | TIOCM_RTS_)); LRMIDISendModem(slot); return 0;
        case TIOCMBIC_: atomic_fetch_and(&LRMIDIModem[slot], ~(*(dword_t *) arg)); LRMIDISendModem(slot); return 0;
        case TCSBRK_: case TCSBRKP_: {
            uintptr_t value = (uintptr_t) arg;   // size-0 ioctl: the argument itself
            if (cmd == TCSBRK_ && value != 0) return 0;   // non-zero: drain only
            const uint8_t ms[2] = {250, 0}; NSData *payload = [NSData dataWithBytes:ms length:2];
            dispatch_async(LRMIDISerial.shared.queue, ^{ if (LRMIDISlots[slot]) LRMIDISend(slot, CMD_BREAK, payload.bytes, payload.length); });
            return 0;
        }
        case TIOCEXCL_: case TIOCNXCL_: case TCXONC_: return 0;
    }
    return _ENOTTY;
}
static void midi_tty_set_termios(struct tty *tty) {
    int slot = tty->num;
    if (slot < 0 || slot >= LR_MIDI_SLOTS) return;
    uint32_t baud = LRDecodeBaud(tty);
    uint32_t c = tty->termios.cflags;
    uint8_t line[7] = {(uint8_t) baud, (uint8_t)(baud >> 8), (uint8_t)(baud >> 16), (uint8_t)(baud >> 24),
        (uint8_t)(5 + ((c >> 4) & 3)) /* CSIZE */, (uint8_t)((c & 0400) ? ((c & 01000) ? 1 : 2) : 0) /* PARENB/PARODD */, (uint8_t)((c & 0100) ? 2 : 1) /* CSTOPB */};
    NSData *payload = [NSData dataWithBytes:line length:sizeof line];
    dispatch_async(LRMIDISerial.shared.queue, ^{
        LRMIDILink *link = LRMIDISlots[slot]; if (!link) return;
        link.baud = baud; LRMIDISend(slot, CMD_LINE, payload.bytes, payload.length);
        dispatch_async(dispatch_get_main_queue(), ^{ [NSNotificationCenter.defaultCenter postNotificationName:LRMIDISerialChangedNotification object:nil]; });
    });
}
static struct tty_driver_ops midi_tty_ops = { .open = midi_tty_open, .write = midi_tty_write, .ioctl = midi_tty_ioctl, .set_termios = midi_tty_set_termios };
DEFINE_TTY_DRIVER(lr_midi_serial_driver, &midi_tty_ops, TTY_MIDI_MAJOR, LR_MIDI_SLOTS);

void LRMIDISerialInstall(void) {
    tty_drivers[TTY_MIDI_MAJOR] = &lr_midi_serial_driver;
    for (int i = 0; i < LR_MIDI_SLOTS; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/ttyMIDI%d", i);
        generic_mknodat(AT_PWD, path, S_IFCHR | 0666, dev_make(TTY_MIDI_MAJOR, i));
    }
    [LRMIDISerial.shared start];
}

static void LRMIDIDeliver(LRMIDILink *link) {
    if (!link.incoming.length) return;
    lock(&ttys_lock);
    struct tty *tty = lr_midi_serial_driver.ttys[link.slot];
    if (tty) { lock(&tty->lock); tty->refcount++; unlock(&tty->lock); }
    unlock(&ttys_lock);
    if (!tty) { [link.incoming setLength:0]; return; }   // port closed: like a real line
    ssize_t done = tty_input(tty, link.incoming.bytes, link.incoming.length, false);
    lock(&ttys_lock); tty_release(tty); unlock(&ttys_lock);
    if (done > 0) [link.incoming replaceBytesInRange:NSMakeRange(0, MIN((NSUInteger) done, link.incoming.length)) withBytes:NULL length:0];
    if (link.incoming.length) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), LRMIDISerial.shared.queue, ^{ LRMIDIDeliver(link); });
}
static void LRMIDIHangup(int slot) {
    lock(&ttys_lock);
    struct tty *tty = lr_midi_serial_driver.ttys[slot];
    if (tty) { lock(&tty->lock); tty->refcount++; tty_hangup(tty); unlock(&tty->lock); }
    unlock(&ttys_lock);
    if (tty) { lock(&ttys_lock); tty_release(tty); unlock(&ttys_lock); }
}

static void LRMIDISendTo(MIDIEndpointRef destination, uint8_t cmd, const uint8_t *payload, size_t length) {
    uint8_t frame[4 + 1 + (LR_MIDI_CHUNK / 7 + 1) * 8 + 1 + 16]; size_t n = 0;
    memcpy(frame, LRHeader, sizeof LRHeader); n = sizeof LRHeader;
    frame[n++] = cmd;
    n += lr_midi_pack(payload, length, frame + n);
    frame[n++] = 0xF7;
    Byte buffer[512]; MIDIPacketList *list = (MIDIPacketList *) buffer;
    MIDIPacket *packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, sizeof buffer, packet, 0, n, frame);
    if (packet) MIDISend(LRMIDISerial.shared.outPort, destination, list);
}
static void LRMIDISend(int slot, uint8_t cmd, const uint8_t *payload, size_t length) {
    LRMIDILink *link = LRMIDISlots[slot]; if (!link) return;
    LRMIDISendTo(link.destination, cmd, payload, length);
}

static NSString *LRMIDIName(MIDIObjectRef object) {
    CFStringRef name = NULL;
    if (MIDIObjectGetStringProperty(object, kMIDIPropertyDisplayName, &name) != noErr || !name) return @"MIDI device";
    return CFBridgingRelease(name);
}
static MIDIEntityRef LRMIDIEntity(MIDIEndpointRef endpoint) { MIDIEntityRef e = 0; MIDIEndpointGetEntity(endpoint, &e); return e; }

@implementation LRMIDISerial
+ (instancetype)shared { static LRMIDISerial *s; static dispatch_once_t once; dispatch_once(&once, ^{ s = [LRMIDISerial new]; }); return s; }
- (instancetype)init {
    if ((self = [super init])) { _queue = dispatch_queue_create("linuxrunner.midi-serial", DISPATCH_QUEUE_SERIAL); _sysex = [NSMutableDictionary new]; _knownDevices = [NSMutableArray new]; }
    return self;
}
- (void)changed { dispatch_async(dispatch_get_main_queue(), ^{ [NSNotificationCenter.defaultCenter postNotificationName:LRMIDISerialChangedNotification object:self]; }); }
- (void)start {
    dispatch_async(_queue, ^{
        if (self.client) return;
        __weak typeof(self) weak = self;
        MIDIClientRef client = 0;
        if (MIDIClientCreateWithBlock(CFSTR("LinuxRunner serial"), &client, ^(const MIDINotification *note) {
            if (note->messageID == kMIDIMsgSetupChanged) [weak rescan];
        }) != noErr) return;
        self.client = client;
        MIDIPortRef inPort = 0, outPort = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        MIDIInputPortCreateWithBlock(client, CFSTR("LinuxRunner serial in"), &inPort, ^(const MIDIPacketList *list, void *ref) {
            MIDIUniqueID source = (MIDIUniqueID)(intptr_t) ref;
            NSMutableData *bytes = [NSMutableData new];
            const MIDIPacket *packet = &list->packet[0];
            for (UInt32 i = 0; i < list->numPackets; i++) { [bytes appendBytes:packet->data length:packet->length]; packet = MIDIPacketNext(packet); }
            dispatch_async(weak.queue, ^{ [weak received:bytes from:source]; });
        });
#pragma clang diagnostic pop
        MIDIOutputPortCreate(client, CFSTR("LinuxRunner serial out"), &outPort);
        self.inPort = inPort; self.outPort = outPort;
        [self scan];
    });
}
- (void)rescan {
    dispatch_async(_queue, ^{
        if (self.rescanPending) return;
        self.rescanPending = YES;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), self.queue, ^{ self.rescanPending = NO; [self scan]; });
    });
}
/// Connect every source, say HELLO to every destination; only endpoints that
/// answer with the LinuxRunner signature become serial ports.
- (void)scan {
    [_knownDevices removeAllObjects];
    NSMutableSet *present = [NSMutableSet new];
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
        MIDIEndpointRef source = MIDIGetSource(i); MIDIUniqueID uid = 0;
        MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &uid);
        [present addObject:@(uid)];
        MIDIPortConnectSource(_inPort, source, (void *)(intptr_t) uid);
        BOOL adapter = NO;
        for (int s = 0; s < LR_MIDI_SLOTS; s++) if (LRMIDISlots[s] && LRMIDISlots[s].source == source) adapter = YES;
        [_knownDevices addObject:@{@"name": LRMIDIName(source), @"adapter": @(adapter)}];
    }
    // Adapters whose USB device went away: hang up their Linux port.
    for (int s = 0; s < LR_MIDI_SLOTS; s++) {
        LRMIDILink *link = LRMIDISlots[s]; if (!link) continue;
        MIDIUniqueID uid = 0; MIDIObjectGetIntegerProperty(link.source, kMIDIPropertyUniqueID, &uid);
        if (![present containsObject:@(uid)] || MIDIObjectGetIntegerProperty(link.source, kMIDIPropertyUniqueID, &uid) != noErr) {
            atomic_store(&LRMIDIConnected[s], false); LRMIDISlots[s] = nil; LRMIDIHangup(s);
        }
    }
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) LRMIDISendTo(MIDIGetDestination(i), CMD_HELLO, NULL, 0);
    [self changed];
}
- (void)received:(NSData *)bytes from:(MIDIUniqueID)source {
    NSMutableData *buffer = _sysex[@(source)] ?: (_sysex[@(source)] = [NSMutableData new]);
    const uint8_t *b = bytes.bytes;
    for (NSUInteger i = 0; i < bytes.length; i++) {
        if (b[i] == 0xF0) [buffer setLength:0];
        if (b[i] >= 0x80 && b[i] != 0xF0 && b[i] != 0xF7 && b[i] < 0xF8) { [buffer setLength:0]; continue; }   // other messages
        if (b[i] >= 0xF8) continue;                                                                     // real-time bytes may interleave
        [buffer appendBytes:&b[i] length:1];
        if (b[i] == 0xF7) { [self frame:buffer from:source]; [buffer setLength:0]; }
        if (buffer.length > 4096) [buffer setLength:0];
    }
}
- (void)frame:(NSData *)frame from:(MIDIUniqueID)uid {
    const uint8_t *f = frame.bytes;
    if (frame.length < 6 || memcmp(f, LRHeader, sizeof LRHeader)) return;
    uint8_t cmd = f[4];
    uint8_t payload[512]; size_t n = lr_midi_unpack(f + 5, frame.length - 6, payload);
    MIDIEndpointRef source = 0;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) { MIDIUniqueID u = 0; MIDIObjectGetIntegerProperty(MIDIGetSource(i), kMIDIPropertyUniqueID, &u); if (u == uid) source = MIDIGetSource(i); }
    if (!source) return;
    LRMIDILink *link = nil;
    for (int s = 0; s < LR_MIDI_SLOTS; s++) if (LRMIDISlots[s].source == source) link = LRMIDISlots[s];
    if (cmd == CMD_HELLO) {
        if (link) return;
        // Pair with the destination of the same USB entity.
        MIDIEndpointRef destination = 0;
        for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); i++) if (LRMIDIEntity(MIDIGetDestination(i)) == LRMIDIEntity(source)) destination = MIDIGetDestination(i);
        if (!destination) return;
        int slot = -1; for (int s = 0; s < LR_MIDI_SLOTS; s++) if (!LRMIDISlots[s]) { slot = s; break; }
        if (slot < 0) return;
        link = [LRMIDILink new]; link.slot = slot; link.source = source; link.destination = destination;
        link.version = n ? payload[0] : 0;
        link.name = n > 1 ? [[NSString alloc] initWithBytes:payload + 1 length:n - 1 encoding:NSASCIIStringEncoding] : LRMIDIName(source);
        link.state = @"connected"; link.incoming = [NSMutableData new]; link.outgoing = [NSMutableData new]; link.baud = 115200;
        LRMIDISlots[slot] = link; atomic_store(&LRMIDIModem[slot], 0); atomic_store(&LRMIDIConnected[slot], true);
        [self changed];
        return;
    }
    if (!link) return;
    if (cmd == CMD_DATA && n) { link.rx += n; [link.incoming appendBytes:payload length:n]; LRMIDIDeliver(link); }
    else if (cmd == CMD_PAUSE) link.paused = YES;
    else if (cmd == CMD_RESUME) {
        link.paused = NO; int slot = (int) link.slot;
        while (link.outgoing.length && !link.paused) { size_t k = MIN((size_t) LR_MIDI_CHUNK, link.outgoing.length); LRMIDISend(slot, CMD_DATA, link.outgoing.bytes, k); link.tx += k; [link.outgoing replaceBytesInRange:NSMakeRange(0, k) withBytes:NULL length:0]; }
    }
}
- (NSArray *)links {
    __block NSMutableArray *list = [NSMutableArray new];
    dispatch_sync(_queue, ^{
        for (int s = 0; s < LR_MIDI_SLOTS; s++) { LRMIDILink *l = LRMIDISlots[s]; if (!l) continue;
            [list addObject:@{@"slot": @(s), @"path": [NSString stringWithFormat:@"/dev/ttyMIDI%d", s], @"name": l.name ?: @"", @"state": l.state ?: @"",
                              @"version": @(l.version), @"rx": @(l.rx), @"tx": @(l.tx), @"baud": @(l.baud)}]; }
    });
    return list;
}
- (NSArray *)devices { __block NSArray *copy; dispatch_sync(_queue, ^{ copy = [self.knownDevices copy]; }); return copy; }
@end
