//
// Created by Magnus Nordlander on 2021-07-06.
//

#ifndef FIRMWARE_TIMEDLATCH_H
#define FIRMWARE_TIMEDLATCH_H

#include "pico/time.h"
#include "optional.hpp"

class TimedLatch {
public:
    TimedLatch(uint16_t thresholdMs, bool currentState);
    // Separate thresholds for switching to true (rising) and to false (falling)
    TimedLatch(uint16_t risingThresholdMs, uint16_t fallingThresholdMs, bool currentState);

    bool get() const;
    void set(bool value);
    void setImmediate(bool value);
private:
    uint16_t risingThreshold;
    uint16_t fallingThreshold;

    bool currentState;
    nonstd::optional<absolute_time_t> changingSince = nonstd::nullopt;
};


#endif //FIRMWARE_TIMEDLATCH_H
