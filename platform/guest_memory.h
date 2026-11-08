#ifndef GUEST_MEMORY_H
#define GUEST_MEMORY_H
#include <stdint.h>
// Keep half of the app limit for the interpreter, graphics and native UI.
static inline uint64_t guest_memory_total(uint64_t physical, uint64_t process_limit) {
    uint64_t limit = process_limit && process_limit < physical ? process_limit : physical;
    return (limit / 2) & ~UINT64_C(4095);
}
#endif
