#include "SerialOut.h"

#if BLINKY_SERIAL_OUT

#include <string.h>

#if defined(NRF52840_XXAA)
#include <class/cdc/cdc_device.h>

// The host has the port open (DTR). Not `Serial`: its operator bool yields
// when false.
static inline bool hostAttached() { return tud_cdc_connected(); }
static inline bool inIsr() { return __get_IPSR() != 0; }
// Below the loop (TASK_PRIO_LOW); stack in words.
#define SOUT_TASK_PRIO TASK_PRIO_LOWEST
#define SOUT_TASK_STACK 384
#elif defined(ARDUINO_ARCH_ESP32)
// The host is reading the USB Serial/JTAG port (SOF seen and data taken).
static inline bool hostAttached() { return HWCDC::isConnected(); }
static inline bool inIsr() { return xPortInIsrContext(); }
// Below the loop task (priority 1 on arduino-esp32); stack in bytes.
#define SOUT_TASK_PRIO tskIDLE_PRIORITY
#define SOUT_TASK_STACK 3072
#else
#error "SerialOut: BLINKY_SERIAL_OUT set for an unhandled platform"
#endif

SerialOut& SerialOut::instance() {
    static SerialOut out;  // constructed on first use (no static-init order issue)
    return out;
}

void SerialOut::begin() {
    if (drainTask_ != nullptr) return;
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    SemaphoreHandle_t sp = xSemaphoreCreateBinary();
    if (m == nullptr || sp == nullptr) {
        Serial.println(F("[FALLBACK] SerialOut: no semaphores; USB serial writes stay blocking (#23)"));
        return;
    }
    mutex_ = m;
    space_ = sp;
    TaskHandle_t h = nullptr;
    // Below the loop (TASK_PRIO_LOW): it runs only while the loop waits
    // (vTaskDelay each iteration), and the loop preempts it the moment it
    // wakes. It blocks on task notifications, so it costs nothing idle.
    if (xTaskCreate(drainMain, "sout", SOUT_TASK_STACK, this, SOUT_TASK_PRIO, &h) != pdPASS) {
        Serial.println(F("[FALLBACK] SerialOut: drain task not started; USB serial writes stay blocking (#23)"));
        return;
    }
    drainTask_ = h;  // last: writers switch to the ring from here on
}

size_t SerialOut::write(const uint8_t* data, size_t size) {
    // Before begin(), from an ISR, or with the scheduler stopped: no mutex
    // to take, so straight to Serial as before.
    if (drainTask_ == nullptr || inIsr() ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return Serial.write(data, size);
    }
    if (size == 0) return 0;
    // No host (DTR low): discarded, as Adafruit_USBD_CDC::write() does.
    if (!hostAttached()) return 0;
    auto m = static_cast<SemaphoreHandle_t>(mutex_);
    // The whole call under the mutex, waits included, so one write's bytes
    // stay together however full the ring is.
    xSemaphoreTake(m, portMAX_DELAY);
    size_t done = 0;
    while (done < size) {
        const size_t free = RING_SIZE - used();
        if (free == 0) {
            // The host isn't draining: wait for room, as the old blocking
            // write did. Stream lines never get here (roomForLine()).
            if (!hostAttached()) break;  // host went away: discard the rest
            waiting_ = true;
            xTaskNotifyGive(static_cast<TaskHandle_t>(drainTask_));
            xSemaphoreTake(static_cast<SemaphoreHandle_t>(space_), pdMS_TO_TICKS(10));
            waiting_ = false;
            continue;
        }
        size_t k = size - done;
        if (k > free) k = free;
        const uint32_t at = head_ % RING_SIZE;
        const size_t first = (k < RING_SIZE - at) ? k : RING_SIZE - at;
        memcpy(&ring_[at], data + done, first);
        memcpy(&ring_[0], data + done + first, k - first);
        head_ = head_ + k;  // publish after the copy (single consumer reads head_)
        done += k;
        if (used() > highWater_) highWater_ = used();
    }
    xSemaphoreGive(m);
    xTaskNotifyGive(static_cast<TaskHandle_t>(drainTask_));
    return done;
}

int SerialOut::availableForWrite() {
    if (drainTask_ == nullptr) return Serial.availableForWrite();
    return static_cast<int>(RING_SIZE - used());
}

bool SerialOut::roomForLine(size_t len) const {
    if (drainTask_ == nullptr) return true;  // blocking path: always "room"
    return RING_SIZE - used() >= len;
}

void SerialOut::flush() {
    if (drainTask_ != nullptr) {
        // Bounded: before a reset the reply should reach the FIFO, but a
        // host that stopped reading must not hold the reset forever.
        const uint32_t t0 = millis();
        while (used() != 0 && hostAttached() && millis() - t0 < 200) {
            xTaskNotifyGive(static_cast<TaskHandle_t>(drainTask_));
            vTaskDelay(1);
        }
    }
    Serial.flush();
}

void SerialOut::onTxComplete() {
    if (drainTask_ != nullptr) xTaskNotifyGive(static_cast<TaskHandle_t>(drainTask_));
}

void SerialOut::drainMain(void* self) {
    static_cast<SerialOut*>(self)->drain();
}

void SerialOut::drain() {
    for (;;) {
        if (used() == 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!hostAttached()) {
            tail_ = head_;  // host gone: discard, as the blocking write did
        } else {
            const int avail = Serial.availableForWrite();
            if (avail <= 0) {
                // FIFO full: tud_cdc_tx_complete_cb wakes us (nRF52840); the
                // timeout is only a backstop (and the ESP32-S3's wake-up).
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));
                continue;
            }
            const uint32_t at = tail_ % RING_SIZE;
            size_t n = used();
            if (n > RING_SIZE - at) n = RING_SIZE - at;
            if (n > static_cast<size_t>(avail)) n = static_cast<size_t>(avail);
            const size_t w = Serial.write(&ring_[at], n);  // fits: no spin
            tail_ = tail_ + w;
            if (used() == 0) Serial.flush();  // send a short tail now
        }
        if (waiting_) xSemaphoreGive(static_cast<SemaphoreHandle_t>(space_));
    }
}

#if defined(NRF52840_XXAA)
// TinyUSB: "Invoked when space becomes available in TX buffer" (cdc_device.h,
// TU_ATTR_WEAK; the core doesn't define it). Runs on the usbd task.
extern "C" {
void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    SerialOut::instance().onTxComplete();
}
}
#endif
#endif  // BLINKY_SERIAL_OUT
