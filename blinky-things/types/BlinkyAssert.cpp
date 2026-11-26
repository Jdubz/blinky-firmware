#include "BlinkyAssert.h"
#include "../inputs/SerialOut.h"  // BLINKY_SERIAL: run-time logs (#23)

namespace BlinkyAssert {
    volatile uint16_t failCount = 0;

    void onFail(const __FlashStringHelper* msg) {
        failCount++;
        if (Serial) {
            BLINKY_SERIAL.print(F("[ASSERT] "));
            BLINKY_SERIAL.println(msg);
        }
    }
}
