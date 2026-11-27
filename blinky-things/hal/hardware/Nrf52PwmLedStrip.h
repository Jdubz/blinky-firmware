#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include "../interfaces/ILedStrip.h"
#include <Arduino.h>
#include <new>
#include <string.h>

/**
 * Nrf52PwmLedStrip — WS2812B driver on the nRF52840 PWM + EasyDMA. show() never waits on the wire.
 *
 * One 16-bit PWM duty value per LED bit, 1.25 us each, so a frame occupies the wire for
 * 30 us per LED (1.92 ms at 64 LEDs, 30.7 ms at 1024). A single pin can't go faster; the loop
 * must not wait on it (#14).
 *
 * Buffers (all pre-allocated, no per-frame malloc):
 *   pixels_     3 B/LED  the render target (setPixelColor), owned by the loop
 *   staged_     3 B/LED  the newest frame handed off by show(), waiting for the wire
 *   pwmBuffer_ 48 B/LED  the duty pattern EasyDMA reads, plus a 300 us reset tail
 *
 * show() copies pixels_ into staged_ (a few us; the scheduler is held so the copy can't tear)
 * and wakes the transmit task. That task (one for all strips, priority above the loop) owns
 * pwmBuffer_: when the wire is idle it encodes staged_ and starts the sequence at once, so with
 * an idle wire the frame is on the wire before show() returns, as before. When the previous
 * frame is still on the wire the frame waits in staged_; the PWM's SEQEND interrupt wakes the
 * task, which starts it the moment the wire is free. A newer show() before then replaces the
 * waiting frame (latest wins: the wire always carries the freshest render, back to back, and
 * the replaced frame is counted, `ledSuperseded`). Nothing renders at the wire's pace: the
 * loop runs at its own rate and the LEDs refresh at the wire's maximum rate, never stalling.
 *
 * Why not a second pattern buffer (render-ahead double buffering): +48 B/LED, which the heap
 * doesn't have (1024 LEDs don't fit even one, see docs), and it buys nothing a 3 B/LED staged
 * copy doesn't: the encode is ~0.5 ms at 1024 LEDs. Why not stream small chunks from an
 * interrupt: one late refill (SoftDevice radio event, flash write) corrupts the frame on the
 * LEDs; a whole-frame DMA buffer can't underrun.
 *
 * Reset (latch): the pattern ends with RESET_PERIODS low periods (300 us, the WS2812B-V5 /
 * SK6812 worst case, Adafruit_NeoPixel's canShow() and Zephyr's ws2812 default), so SEQEND
 * marks a latched frame and back-to-back frames can start immediately.
 *
 * WS2812B timing (800 kHz): PWM at 16 MHz, counter top 20 (1.25 us period),
 *   T0H = 6/20 = 375 ns (spec 250-550 ns), T1H = 13/20 = 812 ns (spec 650-950 ns).
 */
class Nrf52PwmLedStrip : public ILedStrip {
public:
    // ledType encodes the color order using the same bit layout as
    // Adafruit_NeoPixel's NEO_* constants (lower 8 bits):
    //   bits 0-1: byte offset of Blue   (0-3)
    //   bits 2-3: byte offset of Green  (0-3)
    //   bits 4-5: byte offset of Red    (0-3)
    //   bits 6-7: byte offset of White  (ignored — RGB-only)
    // Defaults to NEO_GRB (82) which is the standard WS2812B ordering.
    // Per-device override comes from the device config's `ledType` field —
    // some strips ship as raw RGB (=6), BGR (=164), etc. Without this
    // parameter the driver used to hardcode GRB and silently break.
    Nrf52PwmLedStrip(uint16_t numPixels, int16_t pin, uint32_t ledType = 82);
    ~Nrf52PwmLedStrip() override;

    // Non-copyable
    Nrf52PwmLedStrip(const Nrf52PwmLedStrip&) = delete;
    Nrf52PwmLedStrip& operator=(const Nrf52PwmLedStrip&) = delete;

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

    bool isValid() const { return pixels_ != nullptr && staged_ != nullptr && pwmBuffer_ != nullptr; }

    /// True while a frame is on the wire or waiting for it.
    bool isTransmitting() const { return onWire_ || stagedFresh_; }

    /// The transmit task's minimum free stack ever, in bytes (0 before the first begin()).
    static uint32_t txStackFreeBytes();

    // PWM interrupt entry (PWMn_IRQHandler); not for callers.
    static void onPwmIrq(uint8_t instance);

private:
    // WS2812B timing constants (800 KHz, 16 MHz PWM clock)
    static constexpr uint16_t CTOP = 20;               // Counter top: 1.25 µs
    static constexpr uint16_t T0H  = 6  | 0x8000;      // 375 ns high, bit15 = start high
    static constexpr uint16_t T1H  = 13 | 0x8000;      // 812 ns high
    static constexpr uint16_t DUTY_LOW = 0x8000;         // whole period low
    static constexpr uint32_t RESET_PERIODS = 240;     // 240 x 1.25 us = 300 us latch
    static constexpr uint8_t  PWM_IRQ_PRIORITY = 7;    // lowest app level, like the PDM; ISR only notifies

    static void txTaskMain(void*);
    void serviceFromTask();   // transmit task only
    void encodeStaged();      // transmit task only
    static int8_t findFreePwm();

    uint16_t numPixels_;
    int16_t pin_;
    uint8_t brightness_;

    // Byte offsets within each 3-byte pixel slot for R, G, B. Decoded once
    // from the NEO_* ledType constant at construction so setPixelColor()
    // doesn't have to re-parse the encoding on every pixel write.
    uint8_t rOffset_;
    uint8_t gOffset_;
    uint8_t bOffset_;

    uint8_t* pixels_;           // render target, 3 B/pixel (ordering per rOffset_/gOffset_/bOffset_)
    uint8_t* staged_;           // newest handed-off frame, same layout
    uint16_t* pwmBuffer_;       // PWM duty pattern: numPixels_ * 24 + RESET_PERIODS
    uint32_t pwmBufferLen_;     // Length of pwmBuffer_ in uint16_t elements

    NRF_PWM_Type* pwm_;         // Claimed PWM peripheral (persistent)
    int8_t pwmIndex_;           // its instance number, -1 before begin()
    bool begun_;
    bool metricsOwner_;         // the first strip begun feeds LoopMetrics' LED counters

    // Shared between the loop (show), the transmit task and the SEQEND interrupt:
    volatile bool onWire_;        // set by the task at SEQSTART, cleared by the ISR at SEQEND
    volatile bool stagedFresh_;   // staged_ holds a frame not yet started (show sets, task clears)
    volatile uint32_t handedSeq_;   // frames handed to show() so far (loop writes)
    volatile uint32_t stagedSeq_;   // seq of the frame in staged_ (loop writes)
    volatile uint32_t startedSeq_;  // seq of the newest frame started (task writes)
    volatile uint32_t startedMs_;   // millis() it started at (task writes)

    static Nrf52PwmLedStrip* volatile byPwm_[PWM_COUNT];
    static TaskHandle_t txTask_;
};

#endif // BLINKY_PLATFORM_NRF52840
