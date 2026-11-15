#pragma once
#include <stdbool.h>
enum { LRJITTryClassic = 1, LRJITTryPrepared = 2 };
// On older iOS, a detached debugger is not proof Classic permission was revoked.
// Prepared-region breakpoints, however, must never execute without a debugger.
static inline unsigned lr_jit_activation_paths(bool preparedOS, bool debugger,
                                                bool classicQuarantined, bool preparedQuarantined) {
    return (!preparedOS && !classicQuarantined ? LRJITTryClassic : 0u) |
        (preparedOS && debugger && !preparedQuarantined ? LRJITTryPrepared : 0u);
}
