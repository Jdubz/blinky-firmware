#pragma once
#include <stdint.h>

/**
 * ILedStrip - Abstract interface for LED strip hardware
 *
 * Provides operations needed by EffectRenderer without coupling
 * to Adafruit_NeoPixel directly. Enables unit testing with mocks.
 */
class ILedStrip {
public:
    virtual ~ILedStrip() = default;

    // Lifecycle
    virtual void begin() = 0;

    // Core operations
    virtual void show() = 0;
    virtual void setPixelColor(uint16_t index, uint8_t r, uint8_t g, uint8_t b) = 0;
    virtual void setPixelColor(uint16_t index, uint32_t color) = 0;
    virtual void clear() = 0;

    // Configuration
    virtual void setBrightness(uint8_t brightness) = 0;
    virtual uint8_t getBrightness() const = 0;

    // Information
    virtual uint16_t numPixels() const = 0;

    // Color utility
    virtual uint32_t Color(uint8_t r, uint8_t g, uint8_t b) const = 0;

    // Frame timing, for timing references (TestPattern's flash edges). Frames are numbered by
    // show() calls from 1. A driver may start a frame after show() returns (Nrf52PwmLedStrip
    // queues it while the previous one is on the wire, and a newer show() can replace it), so
    // "handed" and "started" differ:
    //   framesHanded(): the number of the frame the last show() handed off.
    //   lastStarted():  the newest frame that has begun transmitting, and the millis() it began.
    virtual uint32_t framesHanded() const = 0;
    virtual void lastStarted(uint32_t& seq, uint32_t& startMs) const = 0;
};
