#include "RotaryEncoder.h"
#include "hardware/gpio.h"
#include "pico/time.h"

RotaryEncoder::RotaryEncoder(uint8_t clk, uint8_t dt, uint8_t sw) {
    clkPin = clk;
    dtPin = dt;
    swPin = sw;
    lastState = ENCODER_STATE_00;
    currentState = ENCODER_STATE_00;
    stableState = ENCODER_STATE_00;
    lastDebounceTime = get_absolute_time();
    lastSwitchTime = get_absolute_time();
    lastChangeTime = get_absolute_time();
    switchPressTime = get_absolute_time();
    switchPressed = false;
    switchReleased = false;
}

RotaryEncoder::~RotaryEncoder() {
}

void RotaryEncoder::init() {
    // Configure CLK and DT pins as inputs with pull-up
    gpio_init(clkPin);
    gpio_set_dir(clkPin, GPIO_IN);
    gpio_pull_up(clkPin);
    
    gpio_init(dtPin);
    gpio_set_dir(dtPin, GPIO_IN);
    gpio_pull_up(dtPin);
    
    // Configure switch pin as input with pull-up
    gpio_init(swPin);
    gpio_set_dir(swPin, GPIO_IN);
    gpio_pull_up(swPin);
    
    // Read initial state
    uint8_t clk = gpio_get(clkPin);
    uint8_t dt = gpio_get(dtPin);
    currentState = (EncoderState)((clk << 1) | dt);
    stableState = currentState;
    lastState = currentState;
}

void RotaryEncoder::update() {
    absolute_time_t now = get_absolute_time();
    
    // Read encoder state
    uint8_t clk = gpio_get(clkPin);
    uint8_t dt = gpio_get(dtPin);
    EncoderState newState = (EncoderState)((clk << 1) | dt);
    
    // Simple debounce: only update if state is stable for 1ms
    if (newState != currentState) {
        lastChangeTime = now;
        currentState = newState;
        stableState = currentState;  // Update stable state immediately for responsiveness
    } else {
        // State is stable
        stableState = currentState;
    }
    
    // Read switch state
    bool swState = !gpio_get(swPin);  // Inverted because pull-up (low = pressed)
    
    // Debounce switch (50ms debounce)
    if (swState != switchPressed) {
        int64_t diff = absolute_time_diff_us(lastSwitchTime, now);
        if (diff > 50000) {  // 50ms
            bool wasPressed = switchPressed;
            switchPressed = swState;
            lastSwitchTime = now;
            if (switchPressed && !wasPressed) {
                // Switch was just pressed
                switchPressTime = now;
            }
            if (!switchPressed) {
                switchReleased = true;  // Switch was just released
            }
        }
    } else {
        lastSwitchTime = now;
    }
}

EncoderDirection RotaryEncoder::getDirection() {
    // State machine for quadrature decoding
    // Valid transitions (using stable state for reliable detection):
    // CW:  00 -> 01 -> 11 -> 10 -> 00
    // CCW: 00 -> 10 -> 11 -> 01 -> 00
    
    EncoderDirection dir = ENCODER_NONE;
    
    // Use stableState to avoid noise
    if (lastState != stableState) {
        // Check for valid transition using lookup table approach
        // This is more reliable than multiple if-else statements
        uint8_t transition = (lastState << 2) | stableState;
        
        switch (transition) {
            // Clockwise transitions
            case (ENCODER_STATE_00 << 2) | ENCODER_STATE_01:  // 00 -> 01
            case (ENCODER_STATE_01 << 2) | ENCODER_STATE_11:  // 01 -> 11
            case (ENCODER_STATE_11 << 2) | ENCODER_STATE_10:  // 11 -> 10
            case (ENCODER_STATE_10 << 2) | ENCODER_STATE_00:  // 10 -> 00
                dir = ENCODER_CW;
                lastState = stableState;
                break;
                
            // Counter-clockwise transitions
            case (ENCODER_STATE_00 << 2) | ENCODER_STATE_10:  // 00 -> 10
            case (ENCODER_STATE_10 << 2) | ENCODER_STATE_11:  // 10 -> 11
            case (ENCODER_STATE_11 << 2) | ENCODER_STATE_01:  // 11 -> 01
            case (ENCODER_STATE_01 << 2) | ENCODER_STATE_00:  // 01 -> 00
                dir = ENCODER_CCW;
                lastState = stableState;
                break;
                
            // Invalid transitions (noise or missed state) - ignore
            default:
                // Update lastState to current to prevent getting stuck
                lastState = stableState;
                break;
        }
    }
    
    return dir;
}

bool RotaryEncoder::isSwitchPressed() const {
    return switchPressed;
}

bool RotaryEncoder::isSwitchReleased() {
    if (switchReleased) {
        switchReleased = false;  // Reset after reading
        return true;
    }
    return false;
}

bool RotaryEncoder::isSwitchHeld() const {
    return switchPressed;
}

bool RotaryEncoder::isSwitchHeldLong(uint32_t milliseconds) {
    if (!switchPressed) {
        return false;
    }
    
    absolute_time_t now = get_absolute_time();
    int64_t diff = absolute_time_diff_us(switchPressTime, now);
    return (diff >= (int64_t)milliseconds * 1000);
}

