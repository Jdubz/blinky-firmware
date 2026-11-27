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
 * Heap: newlib mallinfo(). The core wraps malloc/free into FreeRTOS heap_3;
 * hal/MallocLock.cpp adds the newlib lock that realloc/calloc/strdup and
 * mallinfo take, so every heap entry point is now exclusive (#223). The walk
 * still runs only on the loop task (sampleHeap(), ~1 Hz from loop()) so
 * `json info` from the BLE callback task never walks the heap; it prints the
 * cached sample. The cache is written while the scheduler is suspended, so no
 * task sees a torn update.
 */

#include "PlatformDetect.h"
#include "../inputs/SerialOut.h"  // BLINKY_SERIAL: run-time logs (#23)

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
            BLINKY_SERIAL.println(F("[ERROR] MemTelemetry::sampleHeap called off the loop task; heap stats not sampled"));
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

#elif defined(BLINKY_PLATFORM_ESP32S3)

// ESP32-S3: the same API and `json info` fields. ESP-IDF FreeRTOS counts stacks in
// bytes (StackType_t is uint8_t), so stackFreeBytes() reads the same. The heap is the
// internal-RAM heap (PSRAM is reported nowhere yet): its size, bytes in use now, and the
// most ever in use (from the minimum free ever). ESP-IDF's heap is locked for every
// task, so sampleHeap() may run on any task; the loop calls it ~1 Hz as on the nRF52840.

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace MemTelemetry {

inline TaskHandle_t& loopTaskSlot() {
    static TaskHandle_t handle = nullptr;
    return handle;
}

inline void captureLoopTask() { loopTaskSlot() = xTaskGetCurrentTaskHandle(); }

inline uint32_t stackFreeBytes(TaskHandle_t task) {
    return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task)) * sizeof(StackType_t);
}

struct HeapInfo {
    uint32_t total;
    uint32_t used;
    uint32_t peak;
    bool valid;
};

inline HeapInfo& heapCache() {
    static HeapInfo cache = {0, 0, 0, false};
    return cache;
}

inline void sampleHeap() {
    const uint32_t total = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    const uint32_t freeNow = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    const uint32_t freeMin = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    vTaskSuspendAll();
    HeapInfo& cache = heapCache();
    cache.total = total;
    cache.used = total - freeNow;
    cache.peak = total - freeMin;
    cache.valid = true;
    xTaskResumeAll();
}

inline HeapInfo heap() {
    vTaskSuspendAll();
    HeapInfo h = heapCache();
    xTaskResumeAll();
    return h;
}

}  // namespace MemTelemetry

#endif  // BLINKY_PLATFORM_NRF52840 / BLINKY_PLATFORM_ESP32S3
