/**
 * MallocLock - make newlib's heap task-safe on the nRF52 core (#223).
 *
 * The Seeed nRF52 core (1.1.10) never implements newlib's __malloc_lock /
 * __malloc_unlock, so libc_nano's no-op versions (lib_a-mlock.o) are linked
 * and malloc/free/mallinfo are unprotected. Several tasks allocate after
 * boot: the loop task (config save, LittleFS file buffers), the Bluefruit
 * callback task (NUS command handlers: ArduinoJson, String, OTA staging,
 * `record start`'s 32 KB ring and sender task), and the BLE task (the core's
 * ada_callback queue mallocs per deferred event). Two of those overlapping in
 * time can corrupt the free list.
 *
 * Defining both symbols here replaces mlock.o (it holds only these two).
 *
 * Lock = suspend the scheduler, the standard FreeRTOS + newlib approach:
 *  - recursive (vTaskSuspendAll nests), which newlib requires;
 *  - no task switch while the heap is mid-update; the SoftDevice and all ISRs
 *    keep running, so BLE timing is untouched;
 *  - before the scheduler starts everything is single-threaded: no-op.
 *
 * ISRs: suspending the scheduler from an ISR is illegal and cannot exclude a
 * task that is already mid-malloc, so an ISR allocation can't be made safe
 * here. The only ISR allocator in the core is WInterrupts.c's ISR_DEFERRED
 * path (attachInterrupt), which this firmware doesn't use. An ISR allocation
 * is counted (MallocLock_isrAllocCount, reported in `json info`) rather than
 * asserted: a halt inside malloc would be worse on a sealed device.
 */

#include "PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include <Arduino.h>
#include <reent.h>

extern "C" {

volatile uint32_t MallocLock_isrAllocCount = 0;

void __malloc_lock(struct _reent* r) {
    (void)r;
    if (isInISR()) {
        MallocLock_isrAllocCount++;
        return;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    vTaskSuspendAll();
}

void __malloc_unlock(struct _reent* r) {
    (void)r;
    if (isInISR()) return;
    // Scheduler state can't change between a lock and its unlock (both run
    // inside one malloc/free call on the same task), so this mirrors lock.
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) return;
    xTaskResumeAll();
}

}  // extern "C"

#endif  // BLINKY_PLATFORM_NRF52840
