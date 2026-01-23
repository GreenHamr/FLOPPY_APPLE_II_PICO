#ifndef ROTARY_ENCODER_H
#define ROTARY_ENCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"
#include "pico/time.h"

// Encoder states
typedef enum {
    ENCODER_STATE_00 = 0,  // Both low
    ENCODER_STATE_01 = 1,  // CLK low, DT high
    ENCODER_STATE_10 = 2,  // CLK high, DT low
    ENCODER_STATE_11 = 3   // Both high
} EncoderState;

// Encoder direction
typedef enum {
    ENCODER_NONE = 0,
    ENCODER_CW = 1,   // Clockwise
    ENCODER_CCW = -1  // Counter-clockwise
} EncoderDirection;

class RotaryEncoder {
private:
    uint8_t clkPin;
    uint8_t dtPin;
    uint8_t swPin;
    
    EncoderState lastState;
    EncoderState currentState;
    EncoderState stableState;  // Debounced stable state
    absolute_time_t lastDebounceTime;
    absolute_time_t lastSwitchTime;
    absolute_time_t lastChangeTime;
    absolute_time_t switchPressTime;  // Time when switch was pressed
    
    bool switchPressed;
    bool switchReleased;
    
public:
    RotaryEncoder(uint8_t clk, uint8_t dt, uint8_t sw);
    ~RotaryEncoder();
    
    void init();
    void update();  // Call this periodically to read encoder
    
    // Get rotation direction (returns ENCODER_CW, ENCODER_CCW, or ENCODER_NONE)
    EncoderDirection getDirection();
    
    // Switch/button state
    bool isSwitchPressed() const;
    bool isSwitchReleased();  // Returns true once when switch is released, then resets
    bool isSwitchHeld() const;
    bool isSwitchHeldLong(uint32_t milliseconds);  // Check if switch held for specified milliseconds
};

#endif // ROTARY_ENCODER_H

