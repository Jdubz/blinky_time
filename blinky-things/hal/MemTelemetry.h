#pragma once

/**
 * MemTelemetry - stack and heap headroom for `json info` (#186, #187).
 *
 * Stack: FreeRTOS fills each task stack with a known pattern at creation;
 * uxTaskGetStackHighWaterMark() reports the smallest headroom ever seen
 * (INCLUDE_uxTaskGetStackHighWaterMark=1 in the Seeed nRF52 core). The loop
 * task's handle is private to the core's main.cpp, so setup() (which the core
 * runs inside that task) captures it. Commands can arrive over BLE NUS and run
 * in the Bluefruit callback task, so "current task" would measure the wrong
 * stack.
 *
 * Heap: newlib mallinfo(). The core wraps malloc/free (-Wl,--wrap) into
 * FreeRTOS heap_3, which suspends the scheduler, but newlib's realloc/calloc/
 * strdup (Arduino String uses realloc) and mallinfo itself call the inner
 * allocator directly, unprotected (#223). The walk therefore runs only on the
 * loop task (sampleHeap(), ~1 Hz from loop()) with the scheduler suspended:
 * no other task can run during the walk, and the loop, the lowest-priority
 * application task, can't have preempted a higher-priority task that is
 * mid-realloc (the equal-priority record sender doesn't allocate, and time
 * slicing is off). `json info` prints the cached sample. The cache is written
 * while the scheduler is suspended, so no task sees a torn update.
 */

#include "PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include <Arduino.h>
#include <malloc.h>

extern "C" int dbgHeapTotal(void);  // core utility/debug.cpp: __HeapLimit - __HeapBase

namespace MemTelemetry {

inline TaskHandle_t& loopTaskSlot() {
    static TaskHandle_t handle = nullptr;  // POD, constant-initialized: no guard
    return handle;
}

// Call once from setup(); records the core's "loop" task.
inline void captureLoopTask() { loopTaskSlot() = xTaskGetCurrentTaskHandle(); }

// Minimum free stack ever, in bytes. nullptr = the calling task.
inline uint32_t stackFreeBytes(TaskHandle_t task) {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task)) * sizeof(StackType_t);
}

struct HeapInfo {
    uint32_t total;  // heap region size
    uint32_t used;   // bytes currently allocated
    uint32_t peak;   // bytes ever claimed from sbrk (newlib-nano never returns them)
    bool valid;      // false until the loop task has sampled once
};

inline HeapInfo& heapCache() {
    static HeapInfo cache = {0, 0, 0, false};  // POD, constant-initialized
    return cache;
}

// Loop task only (see header comment). Refuses, loudly, on any other task.
inline void sampleHeap() {
    TaskHandle_t loopTask = loopTaskSlot();
    if (!loopTask || xTaskGetCurrentTaskHandle() != loopTask) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Serial.println(F("[ERROR] MemTelemetry::sampleHeap called off the loop task; heap stats not sampled"));
        }
        return;
    }
    const uint32_t total = static_cast<uint32_t>(dbgHeapTotal());
    vTaskSuspendAll();
    struct mallinfo mi = mallinfo();
    HeapInfo& cache = heapCache();
    cache.total = total;
    cache.used = static_cast<uint32_t>(mi.uordblks);
    cache.peak = static_cast<uint32_t>(mi.arena);
    cache.valid = true;
    xTaskResumeAll();
}

// Last loop-task sample; safe from any task.
inline HeapInfo heap() {
    vTaskSuspendAll();
    HeapInfo h = heapCache();
    xTaskResumeAll();
    return h;
}

}  // namespace MemTelemetry

#endif  // BLINKY_PLATFORM_NRF52840
