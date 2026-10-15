#pragma once

// newlib heap lock for the nRF52 core: see MallocLock.cpp (#223).

#include "PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include <stdint.h>

extern "C" {
// Heap calls (realloc/calloc/strdup/mallinfo) made from interrupt context,
// which the lock cannot protect. Must stay 0; reported as json info mallocIsr.
extern volatile uint32_t MallocLock_isrAllocCount;
}

namespace MallocLock {
// Call from loop(): logs the first ISR allocation at ERROR, once.
void reportIsrAllocs();
}

#endif  // BLINKY_PLATFORM_NRF52840
