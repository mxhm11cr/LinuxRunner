#ifndef LR_KEYBOARD_POLICY_H
#define LR_KEYBOARD_POLICY_H
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <string.h>

enum { LR_KEY_SHIFT = 1, LR_KEY_CONTROL = 2, LR_KEY_ALT = 4, LR_KEY_COMMAND = 8 };

static inline uint32_t LRSingleKeyCharacter(NSString *text) {
    NSData *data = [text dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    if (data.length != 4) return 0;
    uint32_t scalar; memcpy(&scalar, data.bytes, sizeof scalar);
    return scalar >= 32 && scalar != 127 && scalar <= 0x10ffff &&
        !(scalar >= 0xd800 && scalar <= 0xdfff) ? scalar : 0;
}

static inline unsigned LRModifierForHID(unsigned hid) {
    // USB keyboard usages E0..E7: left and right Control, Shift, Alt, GUI.
    if (hid < 0xe0 || hid > 0xe7) return 0;
    const unsigned flags[] = {LR_KEY_CONTROL, LR_KEY_SHIFT, LR_KEY_ALT, LR_KEY_COMMAND};
    return flags[(hid - 0xe0) % 4];
}

static inline uint32_t LRHardwareKeySym(NSString *characters, NSString *unmodified, unsigned modifiers) {
    uint32_t scalar = 0;
    // RFB carries the intended symbol, not a physical key. Sending '1' with
    // Shift held makes x11vnc temporarily undo Shift to generate exactly '1'.
    // UIKit's layout-aware characters supplies '!' (and Caps Lock's case).
    if ((modifiers & LR_KEY_SHIFT) || !(modifiers & (LR_KEY_ALT | LR_KEY_COMMAND)))
        scalar = LRSingleKeyCharacter(characters);
    if (!scalar || ((modifiers & LR_KEY_CONTROL) && !(modifiers & LR_KEY_SHIFT))) {
        NSString *base = modifiers & LR_KEY_SHIFT ? unmodified.uppercaseString : unmodified;
        scalar = LRSingleKeyCharacter(base);
        // Control's ASCII punctuation codes encode their printable counterpart
        // independently of keyboard layout. Never infer a Shift level from a
        // US punctuation table when UIKit has no printable text.
        if ((modifiers & (LR_KEY_CONTROL | LR_KEY_SHIFT)) == (LR_KEY_CONTROL | LR_KEY_SHIFT) &&
            characters.length == 1) {
            unichar control = [characters characterAtIndex:0];
            if (control >= 0x1c && control <= 0x1f) scalar = control + 0x40;
        }
    }
    return scalar < 256 ? scalar : 0x01000000U | scalar;
}
#endif
