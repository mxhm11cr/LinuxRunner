#import "KeyboardPolicy.h"
#include <stdio.h>

int main(void) { @autoreleasepool {
    unsigned failures = 0;
#define CHECK(label, expression) do { if (!(expression)) { puts("FAIL " label); failures++; } } while (0)
    CHECK("plain digit", LRHardwareKeySym(@"1", @"1", 0) == '1');
    CHECK("Shift digit", LRHardwareKeySym(@"!", @"1", LR_KEY_SHIFT) == '!');
    CHECK("Shift letter", LRHardwareKeySym(@"A", @"a", LR_KEY_SHIFT) == 'A');
    CHECK("Shift punctuation", LRHardwareKeySym(@"?", @"/", LR_KEY_SHIFT) == '?');
    CHECK("Caps Lock case", LRHardwareKeySym(@"A", @"a", 0) == 'A');
    CHECK("layout-aware Shift", LRHardwareKeySym(@"+", @"1", LR_KEY_SHIFT) == '+');
    CHECK("Control letter", LRHardwareKeySym(@"\x01", @"a", LR_KEY_CONTROL) == 'a');
    CHECK("Control Shift letter", LRHardwareKeySym(@"\x03", @"c", LR_KEY_CONTROL | LR_KEY_SHIFT) == 'C');
    CHECK("Control Shift punctuation", LRHardwareKeySym(@"\x1e", @"6", LR_KEY_CONTROL | LR_KEY_SHIFT) == '^');
    CHECK("Option shortcut", LRHardwareKeySym(@"ƒ", @"f", LR_KEY_ALT) == 'f');
    CHECK("Option Shift preserves layout text", LRHardwareKeySym(@"Å", @"a", LR_KEY_ALT | LR_KEY_SHIFT) == 0xc5);
    CHECK("German Shift Command punctuation", LRHardwareKeySym(@"/", @"7", LR_KEY_COMMAND | LR_KEY_SHIFT) == '/');
    CHECK("layout Shift Option punctuation", LRHardwareKeySym(@".", @";", LR_KEY_ALT | LR_KEY_SHIFT) == '.');
    CHECK("unknown Shift level is not guessed as US", LRHardwareKeySym(@"", @"7", LR_KEY_COMMAND | LR_KEY_SHIFT) == '7');
    CHECK("Unicode", LRHardwareKeySym(@"λ", @"λ", 0) == (0x01000000 | 0x3bb));
    CHECK("composed text goes to text input", LRHardwareKeySym(@"ab", @"", 0) == 0);
    CHECK("left Shift HID", LRModifierForHID(0xe1) == LR_KEY_SHIFT);
    CHECK("right Shift HID", LRModifierForHID(0xe5) == LR_KEY_SHIFT);
    CHECK("ordinary HID", LRModifierForHID(0x1e) == 0);
    printf("KEYBOARD_POLICY %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
} }
