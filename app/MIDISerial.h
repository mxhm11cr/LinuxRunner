#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// USB serial for iPhone/iPad through a class-compliant USB-MIDI adapter.
//
// iOS gives apps no access to USB serial chips (CH340, CP210x, FTDI, CDC-ACM),
// but it supports class-compliant USB MIDI devices through the public CoreMIDI
// framework. A microcontroller with native USB (RP2040, ESP32-S2/S3, ATmega32U4,
// Teensy, STM32) running the LinuxRunner serial-over-MIDI firmware appears as a
// MIDI device; serial bytes travel in System Exclusive messages. It can be the
// target board itself, or a cheap adapter wired to another board's UART (and,
// through two GPIOs, its reset/boot lines, so esptool/avrdude can flash it).
// Each adapter becomes /dev/ttyMIDI0-3 in Linux.
//
// Wire protocol (docs/usb-midi-serial/PROTOCOL.md):
//   F0 7D 4C 52 <cmd> <payload, 8-to-7 packed> F7
//   7D = MIDI non-commercial manufacturer ID; 4C 52 = "LR".
//   cmd 01 DATA   both ways, serial bytes
//   cmd 02 LINE   host->adapter: baud (u32 LE), data bits, parity (0 none 1 odd 2 even), stop bits
//   cmd 03 MODEM  host->adapter: bit0 DTR, bit1 RTS (asserted = 1)
//   cmd 04 HELLO  host->adapter: query; adapter->host: version byte, then its name (ASCII)
//   cmd 05 BREAK  host->adapter: duration in ms (u16 LE)
//   cmd 06 PAUSE / 07 RESUME  adapter->host: flow control for its UART buffer
extern NSNotificationName const LRMIDISerialChangedNotification;

@interface LRMIDISerial : NSObject
+ (instancetype)shared;
/// Starts CoreMIDI (no permission prompt) and looks for adapters; repeated on USB changes.
- (void)start;
- (void)rescan;
/// Each: slot, path, name, state, version, rx, tx, baud.
@property (readonly) NSArray<NSDictionary<NSString *, id> *> *links;
/// Every MIDI device iOS reports, adapter or not: name, adapter (BOOL).
@property (readonly) NSArray<NSDictionary<NSString *, id> *> *devices;
@end

void LRMIDISerialInstall(void);


NS_ASSUME_NONNULL_END
