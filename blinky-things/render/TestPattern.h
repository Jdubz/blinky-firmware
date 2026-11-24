#pragma once

#include <stdint.h>

#include "../types/PixelMatrix.h"

/**
 * TestPattern - static and timed patterns for the test box camera (blinky_time
 * docs/TEST_BOX_CAMERA_HARNESS_PLAN.md, "Calibration").
 *
 * Every generator animates, so the camera can't calibrate against one. While a
 * pattern is active it replaces the generator and effect output in the pixel
 * matrix; the renderer and LEDMapper still run, so a logical (x, y) lights the
 * LED the generators would light there (orientation is exercised, not bypassed).
 *
 *   SOLID  every pixel one colour: levels, LED PWM.
 *   PIXEL  one pixel lit, the rest dark: region map, orientation, dead LEDs.
 *   FLASH  every pixel on for onMs, then dark, `count` times, at irregular
 *          intervals (periodMs plus up to half of it, so the flashes don't
 *          phase-lock to the camera's frame clock). The firmware records the
 *          millis() at which each on and off frame was handed to the LEDs, so
 *          the host can put the flashes on its own clock (the serial clock
 *          sync) and compare them with the camera: the timing reference.
 *
 * Runtime state only: never saved, cleared by OFF or a reboot.
 *
 * Header-only and Arduino-free so the host unit test drives it with explicit
 * times. The caller's frame loop does:
 *   update(now)  before rendering (advances FLASH),
 *   fill(matrix) instead of the generator when active(),
 *   shown(now)   right after the LED show() (stamps the edges).
 */
class TestPattern {
public:
    enum class Mode : uint8_t { OFF, SOLID, PIXEL, FLASH };

    static constexpr int MAX_FLASHES = 256;

    struct Edge {
        uint32_t onMs;   // millis() when the first lit frame went to show()
        uint32_t offMs;  // millis() when the first dark frame after it went to show()
    };

    TestPattern() { off(); }

    void off() {
        mode_ = Mode::OFF;
        lit_ = false;
        pendingOn_ = false;
        pendingOff_ = false;
        remaining_ = 0;
        recorded_ = 0;
    }

    void solid(uint8_t r, uint8_t g, uint8_t b) {
        off();
        mode_ = Mode::SOLID;
        color_ = RGB(r, g, b);
    }

    void pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        off();
        mode_ = Mode::PIXEL;
        x_ = x;
        y_ = y;
        color_ = RGB(r, g, b);
    }

    /**
     * Start a flash train. Returns false (and changes nothing) for parameters
     * that can't work; the caller reports why. The first flash comes one period
     * after `nowMs`.
     */
    bool flash(uint8_t r, uint8_t g, uint8_t b, uint16_t onMs, uint16_t periodMs, int count,
               uint32_t nowMs) {
        if (onMs == 0 || periodMs <= onMs || count < 1 || count > MAX_FLASHES) return false;
        off();
        mode_ = Mode::FLASH;
        color_ = RGB(r, g, b);
        onMs_ = onMs;
        periodMs_ = periodMs;
        remaining_ = count;
        rng_ = nowMs ? nowMs : 0x9E3779B9u;  // xorshift needs a non-zero seed
        nextOnMs_ = nowMs + periodMs;
        return true;
    }

    /** Advance a FLASH train to `nowMs`. No-op for the other modes. */
    void update(uint32_t nowMs) {
        if (mode_ != Mode::FLASH) return;
        if (!lit_ && remaining_ > 0 && reached(nowMs, nextOnMs_)) {
            lit_ = true;
            pendingOn_ = true;
            offAtMs_ = nowMs + onMs_;  // lit for at least onMs from the frame it came on
            nextOnMs_ += periodMs_ + jitter();
            remaining_--;
        } else if (lit_ && reached(nowMs, offAtMs_)) {
            lit_ = false;
            pendingOff_ = true;
        }
    }

    /** Draw the pattern into a cleared matrix. */
    void fill(PixelMatrix& matrix) const {
        switch (mode_) {
            case Mode::OFF:
                return;
            case Mode::SOLID:
                fillAll(matrix);
                return;
            case Mode::PIXEL:
                matrix.setPixel(x_, y_, color_);
                return;
            case Mode::FLASH:
                if (lit_) fillAll(matrix);
                return;
        }
    }

    /** Called right after the LEDs were handed the frame: stamps pending edges. */
    void shown(uint32_t nowMs) {
        if (pendingOn_) {
            if (recorded_ < MAX_FLASHES) edges_[recorded_].onMs = nowMs;
            pendingOn_ = false;
        }
        if (pendingOff_) {
            if (recorded_ < MAX_FLASHES) edges_[recorded_].offMs = nowMs;
            recorded_++;
            pendingOff_ = false;
        }
    }

    bool active() const { return mode_ != Mode::OFF; }
    Mode mode() const { return mode_; }
    /** FLASH: every flash has been shown and turned off. */
    bool done() const { return mode_ == Mode::FLASH && remaining_ == 0 && !lit_ && !pendingOff_; }
    int recorded() const { return recorded_; }
    const Edge& edge(int i) const { return edges_[i]; }

    static const char* modeName(Mode m) {
        switch (m) {
            case Mode::OFF:
                return "off";
            case Mode::SOLID:
                return "solid";
            case Mode::PIXEL:
                return "pixel";
            case Mode::FLASH:
                return "flash";
        }
        return "?";  // unreachable: every Mode is handled above
    }

private:
    // Wrap-safe "now is at or past t" for millis().
    static bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

    uint16_t jitter() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return (uint16_t)(rng_ % (uint32_t)(periodMs_ / 2 + 1));
    }

    void fillAll(PixelMatrix& matrix) const { matrix.fill(color_); }

    Mode mode_ = Mode::OFF;
    RGB color_;
    int x_ = 0;
    int y_ = 0;
    uint16_t onMs_ = 0;
    uint16_t periodMs_ = 0;
    int remaining_ = 0;
    bool lit_ = false;
    bool pendingOn_ = false;
    bool pendingOff_ = false;
    uint32_t nextOnMs_ = 0;
    uint32_t offAtMs_ = 0;
    uint32_t rng_ = 1;
    int recorded_ = 0;
    Edge edges_[MAX_FLASHES] = {};
};
