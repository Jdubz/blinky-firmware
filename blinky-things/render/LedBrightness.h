#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../hal/interfaces/ILedStrip.h"

/**
 * LedBrightness - a runtime-only override of the LED strip's brightness, for the test box
 * (blinky_time docs/TEST_BOX.md): its chip's device config has brightness 0, so it boots and
 * idles dark, and blinky-server lights it only while a test runs.
 *
 * The device config's `brightness` stays the default: boot applies it as before, and nothing
 * changes until a `leds` command arrives. The override goes through ILedStrip::setBrightness
 * (applied as each pixel is written), so the generators, the renderer and the LED DMA run
 * exactly as they do lit: frame timing measured dark is frame timing lit.
 *
 * Never saved: a reboot, or `leds config`, returns to the device config's brightness.
 *
 * Header-only and Arduino-free, so the host unit test drives it with a MockLedStrip.
 */
class LedBrightness {
public:
    enum class Action : uint8_t { STATUS, SET, CONFIG, INVALID };

    struct Parsed {
        Action action;
        uint8_t value;  // SET only
    };

    static const char* usage() { return "Usage: leds [status] | off | config | 0-255"; }

    /// The `leds` command's arguments (the text after "leds", leading spaces allowed):
    ///   "" or "status"  report
    ///   "off"           override to 0
    ///   "config"        drop the override: the device config's brightness
    ///   "0".."255"      override to that brightness (decimal digits only)
    /// Anything else is INVALID (refused, never coerced).
    static Parsed parse(const char* args) {
        Parsed p{Action::INVALID, 0};
        if (!args) return p;
        while (*args == ' ') args++;
        char tok[8];
        size_t n = 0;
        while (args[n] && args[n] != ' ') {
            if (n + 1 >= sizeof(tok)) return p;  // too long for any valid argument
            tok[n] = args[n];
            n++;
        }
        tok[n] = '\0';
        const char* rest = args + n;
        while (*rest == ' ') rest++;
        if (*rest) return p;  // one argument at most

        if (n == 0 || strcmp(tok, "status") == 0) {
            p.action = Action::STATUS;
        } else if (strcmp(tok, "off") == 0) {
            p.action = Action::SET;
        } else if (strcmp(tok, "config") == 0) {
            p.action = Action::CONFIG;
        } else {
            long v = 0;
            for (size_t i = 0; i < n; i++) {
                if (tok[i] < '0' || tok[i] > '9') return p;
                v = v * 10 + (tok[i] - '0');
            }
            if (v > 255) return p;
            p.action = Action::SET;
            p.value = (uint8_t)v;
        }
        return p;
    }

    void begin(ILedStrip* strip) { strip_ = strip; }
    bool attached() const { return strip_ != nullptr; }

    /// Override the brightness until config() or a reboot. Never saved.
    void set(uint8_t brightness) {
        strip_->setBrightness(brightness);
        overridden_ = true;
    }

    /// Back to the device config's brightness (what a reboot would apply).
    void config(uint8_t configured) {
        strip_->setBrightness(configured);
        overridden_ = false;
    }

    bool overridden() const { return overridden_; }
    /// The brightness the strip applies now.
    uint8_t current() const { return strip_->getBrightness(); }

private:
    ILedStrip* strip_ = nullptr;
    bool overridden_ = false;
};
