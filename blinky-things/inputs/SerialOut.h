#pragma once

#include <Arduino.h>

// The native nRF52840 core with TinyUSB, and the ESP32-S3 with its console on the
// USB Serial/JTAG peripheral (HWCDC; not host builds).
#if defined(USE_TINYUSB) && defined(NRF52840_XXAA)
#define BLINKY_SERIAL_OUT 1
#elif defined(ARDUINO_ARCH_ESP32) && defined(ARDUINO_USB_MODE) && defined(ARDUINO_USB_CDC_ON_BOOT) && \
    ARDUINO_USB_MODE == 1 && ARDUINO_USB_CDC_ON_BOOT == 1
#define BLINKY_SERIAL_OUT 1
#else
#define BLINKY_SERIAL_OUT 0
#endif

// Where run-time log lines go: SerialOut on the nRF52840 (so they can't
// land inside a stream line it is sending), Serial elsewhere.
#if BLINKY_SERIAL_OUT
#define BLINKY_SERIAL SerialOut::instance()
#else
#define BLINKY_SERIAL Serial
#endif

#if BLINKY_SERIAL_OUT

/**
 * SerialOut - USB serial output that never blocks the render loop
 * (blinky-firmware#23).
 *
 * Why: Adafruit_USBD_CDC::write() loops until every byte is in TinyUSB's
 * TX FIFO, calling yield() while it is full. The FIFO is 256 bytes
 * (CFG_TUD_CDC_TX_BUFSIZE, tusb_config_nrf.h) and a 50-band NN stream line
 * is ~530, so every line held the loop task until the host had taken a few
 * 64-byte packets; and yield() is taskYIELD(), which hands the CPU only to
 * tasks of the loop's priority or higher, so the loop just spins.
 *
 * How: writers copy into a RAM ring (whole write calls, under a mutex) and
 * return. A drain task below the loop's priority moves the ring into the
 * FIFO, never more than tud_cdc_write_available() reports, so its writes
 * don't spin either, and sleeps on tud_cdc_tx_complete_cb() ("invoked when
 * space becomes available in TX buffer", TinyUSB cdc_device.h) while the
 * FIFO is full. Bytes and order are unchanged: one ring, one consumer.
 *
 * Nothing is lost silently:
 *   - stream lines ask roomForLine() before formatting; with no room the
 *     line is skipped and counted (streamLinesDropped(), `json info`
 *     streamDropped; NN lines also show it in their "skip" field);
 *   - any other write waits for room, as the old blocking write did, so
 *     command replies and logs are never cut;
 *   - with no host attached (DTR low) output is discarded, as before
 *     (Adafruit_USBD_CDC::write() returns 0 then).
 *
 * Before begin() (most of setup()) writes go straight to Serial.
 *
 * ESP32-S3 (HWCDC, the USB Serial/JTAG console): the same ring and drain task.
 * HWCDC::write() waits up to 100 ms per chunk, and up to ~2 s in all, while the
 * host isn't reading; the drain task writes only what availableForWrite() says
 * fits, so it doesn't wait either. "Host attached" is HWCDC::isConnected().
 */
class SerialOut : public Print {
public:
    static SerialOut& instance();

    // Start the drain task. Call once, late in setup().
    void begin();

    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t size) override;
    int availableForWrite() override;
    void flush();  // wait (bounded) until the ring is in the FIFO (Print has no virtual flush here)

    // Room for a line of `len` bytes right now. False: the host isn't
    // draining, and the caller skips the line and calls lineDropped().
    bool roomForLine(size_t len) const;
    void lineDropped() { linesDropped_++; }
    uint32_t streamLinesDropped() const { return linesDropped_; }
    uint32_t highWater() const { return highWater_; }

    static const size_t RING_SIZE = 8192;

    // TinyUSB's tx-complete callback lands here (usbd task).
    void onTxComplete();

private:
    SerialOut() = default;
    static void drainMain(void* self);
    void drain();
    size_t used() const { return head_ - tail_; }  // free-running indices

    uint8_t ring_[RING_SIZE];
    volatile uint32_t head_ = 0;  // producers (under mutex_)
    volatile uint32_t tail_ = 0;  // drain task only
    void* mutex_ = nullptr;       // SemaphoreHandle_t
    void* drainTask_ = nullptr;   // TaskHandle_t
    void* space_ = nullptr;       // SemaphoreHandle_t (binary): room freed
    volatile bool waiting_ = false;  // a writer waits on space_
    uint32_t linesDropped_ = 0;
    uint32_t highWater_ = 0;
};
#endif  // BLINKY_SERIAL_OUT
