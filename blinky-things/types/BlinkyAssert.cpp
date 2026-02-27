#include "BlinkyAssert.h"

namespace BlinkyAssert {
    volatile uint16_t failCount = 0;

    void onFail(const __FlashStringHelper* msg) {
        failCount++;
        if (Serial) {
            Serial.print(F("[ASSERT] "));
            Serial.println(msg);
        }
    }
}
