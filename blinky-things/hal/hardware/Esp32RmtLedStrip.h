#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include "../interfaces/ILedStrip.h"
#include <Arduino.h>
#include <driver/rmt_tx.h>

/**
 * Esp32RmtLedStrip — WS2812B driver on the ESP32-S3's RMT peripheral. show() never waits
 * on the wire: the ESP32-S3 counterpart of Nrf52PwmLedStrip (#14), same contract.
 *
 * The wire time is the same 30 us per LED (1.25 us per bit), and the loop must not wait on
 * it. The RMT encodes the frame from a byte buffer in hardware time; a bytes encoder turns
 * each bit into a high/low symbol pair and a copy encoder appends the 300 us latch, so the
 * transaction-done interrupt marks a latched frame (as SEQEND does on the nRF52840).
 *
 * Buffers (3 B/LED each, internal RAM, allocated once):
 *   pixels_  the render target (setPixelColor), owned by the loop
 *   staged_  the newest frame handed off by show(), waiting for the wire
 *   wire_    the frame the RMT is reading (a transaction reads its buffer until done)
 *
 * show() copies pixels_ into staged_ under a spinlock and notifies the transmit task. That
 * task (above the loop, on the loop's core) starts staged_ at once when the wire is idle,
 * else the frame waits; the done interrupt notifies the task, which starts the newest
 * waiting frame (latest wins; a replaced frame counts as `ledSuperseded`). With the DMA
 * back end (one RMT TX channel on the S3 has it) the encoder refills the RMT memory in
 * larger blocks, so a late interrupt is less likely to stretch a bit; the second strand of
 * a two-strand config runs without DMA.
 *
 * `pin` is a GPIO number (on the XIAO ESP32-S3, D0 = GPIO1), unlike the nRF52840's
 * Arduino pin numbers. `ledType` is Adafruit_NeoPixel's NEO_* color order, RGB only.
 *
 * WS2812B timing at a 10 MHz RMT resolution: T0H 0.4 us / T0L 0.85 us, T1H 0.8 us /
 * T1L 0.45 us (datasheet: +-150 ns), latch 300 us (WS2812B-V5 / SK6812 worst case).
 */
class Esp32RmtLedStrip : public ILedStrip {
public:
    Esp32RmtLedStrip(uint16_t numPixels, int16_t pin, uint32_t ledType = 82);
    ~Esp32RmtLedStrip() override;

    Esp32RmtLedStrip(const Esp32RmtLedStrip&) = delete;
    Esp32RmtLedStrip& operator=(const Esp32RmtLedStrip&) = delete;

    // ILedStrip interface
    void begin() override;
    void show() override;
    void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void setPixelColor(uint16_t index, uint32_t color) override;
    void clear() override;
    void setBrightness(uint8_t brightness) override;
    uint8_t getBrightness() const override;
    uint16_t numPixels() const override;
    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) const override;
    uint32_t framesHanded() const override { return handedSeq_; }
    void lastStarted(uint32_t& seq, uint32_t& startMs) const override;

    bool isValid() const { return pixels_ != nullptr && staged_ != nullptr && wire_ != nullptr; }
    bool isBegun() const { return begun_; }

    /// The transmit task's minimum free stack ever, in bytes (0 before the first begin()).
    static uint32_t txStackFreeBytes();

private:
    static constexpr uint32_t RMT_RESOLUTION_HZ = 10000000;  // 0.1 us ticks
    static constexpr uint32_t RESET_US = 300;
    static constexpr int MAX_STRIPS = 4;                       // RMT TX channels on the S3

    static void txTaskMain(void*);
    static bool onTransDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t*, void* ctx);
    void serviceFromTask();

    uint16_t numPixels_;
    int16_t pin_;
    uint8_t brightness_;
    uint8_t rOffset_;
    uint8_t gOffset_;
    uint8_t bOffset_;

    uint8_t* pixels_;
    uint8_t* staged_;
    uint8_t* wire_;

    rmt_channel_handle_t chan_;
    rmt_encoder_handle_t encoder_;
    bool dma_;
    bool begun_;
    bool metricsOwner_;
    bool txErrorLogged_;

    // Shared between the loop (show), the transmit task and the done interrupt, under lock_.
    portMUX_TYPE lock_;
    volatile bool onWire_;
    volatile bool stagedFresh_;
    volatile uint32_t handedSeq_;
    volatile uint32_t stagedSeq_;
    volatile uint32_t startedSeq_;
    volatile uint32_t startedMs_;

    static Esp32RmtLedStrip* volatile strips_[MAX_STRIPS];
    static TaskHandle_t txTask_;
};

#endif // BLINKY_PLATFORM_ESP32S3
