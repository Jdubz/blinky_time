/**
 * MallocLock - close the unlocked newlib heap paths on the nRF52 core (#223).
 *
 * The Seeed nRF52 core (1.1.10) links with -Wl,--wrap=malloc,--wrap=free and
 * routes both into FreeRTOS heap_3 (pvPortMalloc / vPortFree), which suspend
 * the scheduler around the call. So plain malloc/free/new/delete are already
 * task-safe. What is NOT covered: newlib's own realloc / calloc / strdup /
 * mallinfo call the inner allocator (_malloc_r / _free_r) directly, bypassing
 * the wrap, and the core never implements newlib's __malloc_lock, so
 * libc_nano's no-op lib_a-mlock.o is linked. Arduino String grows with
 * realloc; NUS command handlers (e.g. `device show`) build Strings in the
 * Bluefruit callback task while the higher-priority BLE task can preempt and
 * malloc (ada_callback_invoke), so a realloc mid-update can be interleaved
 * with another task's malloc/free and corrupt the free list.
 *
 * nano _malloc_r / _free_r / mallinfo all take __malloc_lock, so defining
 * both symbols here makes every heap entry point mutually exclusive. The
 * sketch objects link before libc_nano, so lib_a-mlock.o is never pulled in
 * (verified in the link map: no references). Its gcc 9-2019q4 build also
 * references the __retarget_lock_* / __lock___malloc_recursive_mutex
 * symbols; a toolchain that started needing it would show up as a link
 * error, not a silent second lock.
 *
 * Lock = suspend the scheduler, the standard FreeRTOS + newlib approach:
 *  - recursive (vTaskSuspendAll nests), which newlib requires;
 *  - no task switch while the heap is mid-update; the SoftDevice and all ISRs
 *    keep running, so BLE timing is untouched;
 *  - before the scheduler starts everything is single-threaded: no-op.
 *
 * ISRs: suspending the scheduler from an ISR is illegal and can't exclude a
 * task already mid-allocation. Do NOT count on heap_3 catching ISR
 * allocations: configASSERT is compiled in only under DEBUG_NRF /
 * DEBUG_NRF_USER (FreeRTOSConfig.h), which release builds don't set, so a
 * plain malloc/free from an ISR also reaches this lock. Every ISR heap call
 * (malloc, free, realloc, calloc, strdup, mallinfo) is therefore counted here
 * (MallocLock_isrAllocCount) rather than asserted inside the allocator, and
 * loop() logs the first one at ERROR (MallocLock::reportIsrAllocs) - `json
 * info` reports the count as mallocIsr. The only ISR allocator in the core
 * is WInterrupts.c's ISR_DEFERRED path (attachInterrupt), which this
 * firmware doesn't use. The count is a diagnostic: a nested-ISR increment
 * can be lost, but any non-zero value is the signal.
 */

#include "MallocLock.h"

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

namespace MallocLock {

void reportIsrAllocs() {
    static bool logged = false;
    if (!logged && MallocLock_isrAllocCount != 0) {
        logged = true;
        Serial.print(F("[ERROR] MallocLock: heap allocation from an ISR ("));
        Serial.print(MallocLock_isrAllocCount);
        Serial.println(F(" so far) - not protected by the heap lock; see json info mallocIsr"));
    }
}

}  // namespace MallocLock

#endif  // BLINKY_PLATFORM_NRF52840
