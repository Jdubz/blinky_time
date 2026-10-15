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
 * Heap: newlib malloc via mallinfo(). The core defines no __malloc_lock, so a
 * free-list walk racing another task's malloc could follow a stale pointer;
 * the scheduler is suspended for the walk (as FreeRTOS heap_3 does around
 * malloc). ISRs keep running and never allocate.
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
};

inline HeapInfo heap() {
    vTaskSuspendAll();
    struct mallinfo mi = mallinfo();
    xTaskResumeAll();
    HeapInfo h;
    h.total = static_cast<uint32_t>(dbgHeapTotal());
    h.used = static_cast<uint32_t>(mi.uordblks);
    h.peak = static_cast<uint32_t>(mi.arena);
    return h;
}

}  // namespace MemTelemetry

#endif  // BLINKY_PLATFORM_NRF52840
