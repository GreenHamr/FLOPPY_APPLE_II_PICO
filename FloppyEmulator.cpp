#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/irq.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include <cstdint>
#include <pico/time.h>
#include <stdio.h>
#include <string.h>
#include "PinConfig.h"
#include "hardware/pwm.h"

// Static pointer to FloppyEmulator instance for timer callback and DMA IRQ
static FloppyEmulator* g_floppyEmulatorInstance = nullptr;

// Static variable to track which PIO instance is being used for write IRQ timer (set during init)
static PIO g_writeIrqTimerPio = nullptr;

static uint slice_num;

/*
// PWM IRQ handler for write timer (4μs period)
void pwm_irq_handler() {
    pwm_clear_irq(slice_num);
    if (!g_floppyEmulatorInstance) {
        return;  // Write not active - ignore IRQ
    }
    if (!g_floppyEmulatorInstance->isWriteEnabled()) {
        return;
    }
    if (!g_floppyEmulatorInstance->isDriveSelected()) {
        return;
    }
    g_floppyEmulatorInstance->handleWriteIRQTimer();
}

*/


// GPIO IRQ handler for WRITE_EN and WRITE pins (reacts to both rising and falling edges)
// This allows us to detect when the write head is activated (WRITE_EN = 1)
// and flux transitions on the WRITE pin
void writeEnableIRQHandler(uint gpio, uint32_t events) {
    if (!g_floppyEmulatorInstance) {
        printf("ERROR: writeEnableIRQHandler() - g_floppyEmulatorInstance is null\r\n");
        return;
    }

    if (!g_floppyEmulatorInstance->isDriveSelected()) {
        return;
    }
    // Check which pin triggered the IRQ
    if (gpio == GPIO_WRITE) {
        g_floppyEmulatorInstance->handleWriteIRQ(events);
    } else if (gpio == GPIO_WRITE_ENABLE) {
        // Check if this is the WRITE pin
        g_floppyEmulatorInstance->handleWriteEnableIRQ(events);
    }
}


// GCR encoding table: maps 5-bit data (0-31) to 6-bit GCR code
// GCR rule: no more than 2 consecutive zeros
static const uint8_t GCR_ENCODE_LOOKUP[32] = {
    0x0A, 0x0B, 0x12, 0x13,  // 0-3
    0x0E, 0x0F, 0x16, 0x17,  // 4-7
    0x09, 0x19, 0x1A, 0x1B,  // 8-11
    0x0D, 0x1D, 0x1E, 0x1F,  // 12-15
    0x05, 0x15, 0x25, 0x35,  // 16-19
    0x07, 0x17, 0x27, 0x37,  // 20-23
    0x06, 0x26, 0x36, 0x2A,  // 24-27
    0x2B, 0x2E, 0x2F, 0x3A   // 28-31
};

// Apple II NIC format encoding table (6-bit GCR)
static const uint8_t NIC_ENCODE_TABLE[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF
};

// NIC decode table: maps encoded byte to 6-bit value (0-63)
// Built from NIC_ENCODE_TABLE: decodeTable[encoded] = index
static uint8_t NIC_DECODE_TABLE[256];
static bool nicDecodeTableInitialized = false;

// Initialize decode table (called once at startup)
static void ensureNICDecodeTableInitialized() {
    if (!nicDecodeTableInitialized) {
        // Initialize all to invalid marker
        for (int i = 0; i < 256; i++) {
            NIC_DECODE_TABLE[i] = 0xFF;
        }
        // Build reverse lookup: for each encoded value, store its index
        for (int i = 0; i < 64; i++) {
            NIC_DECODE_TABLE[NIC_ENCODE_TABLE[i]] = i;
        }
        nicDecodeTableInitialized = true;
    }
}

// Sector scramble table (Apple II sector ordering)
static const uint8_t SECTOR_SCRAMBLE[16] = {
    0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15
};

// Flip bit tables for data encoding
static const uint8_t FLIP_BIT1[4] = { 0, 2, 1, 3 };
static const uint8_t FLIP_BIT2[4] = { 0, 8, 4, 12 };
static const uint8_t FLIP_BIT3[4] = { 0, 32, 16, 48 };

// Reverse lookup tables for FlipBit decoding
// FLIP_BIT1: value -> index mapping
static const uint8_t FLIP_BIT1_REV[4] = { 0, 2, 1, 3 };  // Same as forward (symmetric)
// FLIP_BIT2: value -> index mapping  
static const uint8_t FLIP_BIT2_REV[16] = { 0, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0 };
// FLIP_BIT3: value -> index mapping
static const uint8_t FLIP_BIT3_REV[64] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Stepper motor phase sequence for forward (outward) movement
// 4-phase full step sequence
static const uint8_t STEPPER_SEQUENCE_OUTWARD[4] = {
    0b0001,  // Phase 0
    0b0010,  // Phase 1
    0b0100,  // Phase 2
    0b1000   // Phase 3
};

// Stepper motor phase sequence for reverse (inward) movement
static const uint8_t STEPPER_SEQUENCE_INWARD[4] = {
    0b1000,  // Phase 3
    0b0100,  // Phase 2
    0b0010,  // Phase 1
    0b0001   // Phase 0
};

// Constructor
FloppyEmulator::FloppyEmulator(
    uint8_t ph0, uint8_t ph1, uint8_t ph2, uint8_t ph3,
    uint8_t read, uint8_t write, uint8_t writeEnable, uint8_t driveSel
) {
    // Initialize stepper motor pins
    stepperPhasePins[0] = ph0;
    stepperPhasePins[1] = ph1;
    stepperPhasePins[2] = ph2;
    stepperPhasePins[3] = ph3;
    
    // Initialize read/write pins
    readPin = read;
    writePin = write;
    writeEnablePin = writeEnable;
    driveSelPin = driveSel;
    
    // Initialize state
    currentTrack = 0;
    currentSector = 0;
    currentSectortoWrite = -1;
    physicalSectorToWrite = -1;
    dmaPositionAtWriteStart = 0;
    currentStep = 0;
    physicalTrack = 0;  // Physical track (0-69), logical track = physicalTrack >> 1
    currentPhase = STEPPER_PHASE_0;
    lastPhaseOffset = STEPPER_PHASE_0;  // Initialize last phase offset
    rotationPosition = 0;
    indexPulse = false;
    lastBitTime = get_absolute_time();
    timerActive = false;
    
    // Initialize write buffer state
    writeData = 0;
    writeBitCount = 0;
    writeBufferIndex = 0;
    writeSynced = false;
    lastWritePinState = 0;
    prologBuffer[0] = 0;
    prologBuffer[1] = 0;
    prologBuffer[2] = 0;
    prologBitCount = 0;
    writeBitsProcessed = 0;  // Count bits processed (skip 25th bit)
    
    // Initialize PIO/DMA state
    pio = nullptr;
    pioSm = 0;
    pioOffset = 0;
    dmaChannel = -1;
    pioDmaActive = false;
    
    // Initialize PIO IRQ timer state
    writeIrqTimerPio = nullptr;
    writeIrqTimerSm = 0;
    writeIrqTimerOffset = 0;
    writeIrqTimerActive = false;
    
    // Initialize GCR cache state
    gcrTrackCacheTrack = -1;  // Cache invalid initially
    gcrTrackCacheBits = 0;
    gcrTrackCacheDirty = false;  // Cache is clean initially
    
    // Initialize SD card and file management
    sdCardManager = nullptr;
    memset(currentFileName, 0, sizeof(currentFileName));
    
    // Clear disk image
    clearDiskImage();
    
    // Initialize GCR tables
    initializeGCRTables();
    // Initialize NIC decode table
    ensureNICDecodeTableInitialized();
}

    // Destructor
FloppyEmulator::~FloppyEmulator() {
    // Stop timer if active
    stopBitTimer();
    // Stop PIO/DMA if active
    stopPIO_DMA();
}

// Initialize hardware
void FloppyEmulator::init() {
    // Set static instance pointer for IRQ handlers
    g_floppyEmulatorInstance = this;
    
    // Configure stepper phase pins as INPUT (we read from controller)
    // Phases are active HIGH (no pull-down resistors)
    for (int i = 0; i < 4; i++) {
        gpio_init(stepperPhasePins[i]);
        gpio_set_dir(stepperPhasePins[i], GPIO_IN);
        // No pull resistors - controller drives the pins
    }
    
    // Configure read pin as OUTPUT (we send data to controller)
    gpio_init(readPin);
    gpio_set_dir(readPin, GPIO_OUT);
    gpio_put(readPin, 0);
    
    // Configure write pin as INPUT (we receive data from controller)
    gpio_init(writePin);
    gpio_set_dir(writePin, GPIO_IN);
    gpio_pull_down(writePin);
    
    // Configure write enable pin as INPUT (from controller)
    gpio_init(writeEnablePin);
    gpio_set_dir(writeEnablePin, GPIO_IN);
    gpio_pull_down(writeEnablePin);
    
    // Configure drive select pin as INPUT (from controller)
    gpio_init(driveSelPin);
    gpio_set_dir(driveSelPin, GPIO_IN);
    gpio_pull_up(driveSelPin);  // Pull up - selected when low (active low signal)
    
    // Configure GPIO IRQ for WRITE_EN and WRITE pins to react to both rising and falling edges
    // This allows us to detect when the write head is activated (WRITE_EN = 1)
    // and flux transitions on the WRITE pin
    gpio_set_irq_callback(writeEnableIRQHandler);
    gpio_set_irq_enabled(writeEnablePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(writePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    irq_set_priority(IO_IRQ_BANK0, 0);
    irq_set_enabled(IO_IRQ_BANK0, true);
    
    // Initialize GPIO2 for debug output (write timer toggle)
    //gpio_init(2);
    //gpio_set_dir(2, GPIO_OUT);
    //gpio_put(2, 0);
    
    // Initialize position tracking
    currentTrack = 0;
    currentSector = 0;
    currentSectortoWrite = -1;
    physicalSectorToWrite = -1;
    dmaPositionAtWriteStart = 0;
    currentStep = 0;
    physicalTrack = 0;  // Physical track (0-69), logical track = physicalTrack >> 1
    currentPhase = STEPPER_PHASE_0;
    lastPhaseOffset = currentPhase;  // Sync last phase offset with current phase
    
    // Read initial stepper phase from GPIO to sync with hardware state
    // Don't update position on initial read - just sync phase
    // Phases are active HIGH
    uint8_t phaseState = 0;
    for (int i = 0; i < 4; i++) {
        if (gpio_get(stepperPhasePins[i])) {  // Active when HIGH
            phaseState |= (1 << i);
        }
    }
    
    // Find first active phase and sync
    for (int i = 0; i < 4; i++) {
        if (phaseState & (1 << i)) {
            currentPhase = (StepperPhase)i;
            break;
        }
    }
    
    // Initialize GCR track cache with test pattern (0xAA) for testing
    updateGCRTrackCache();
    
    // Initialize PIO/DMA for continuous bit output
    initPIO_DMA();
    
    // Initialize PIO IRQ timer for write bit capture
    //initWriteIRQTimer();
    
    // Initialize PWM timer for write bit capture
    initWritePWMTimer();
    
    // Start PIO/DMA immediately - it should run continuously like real hardware
    // In real Apple II floppy drive, READ pin constantly outputs data when drive is spinning
    startPIO_DMA();
}

// Initialize GCR encoding/decoding tables
void FloppyEmulator::initializeGCRTables() {
    // Initialize encode table
    for (int i = 0; i < GCR_TABLE_SIZE; i++) {
        gcrEncodeTable[i] = GCR_ENCODE_LOOKUP[i];
    }
    
    // Initialize decode table (inverse mapping)
    // First, clear decode table
    for (int i = 0; i < 256; i++) {
        gcrDecodeTable[i] = 0xFF;  // Invalid marker
    }
    
    // Build decode table from encode table
    for (int i = 0; i < GCR_TABLE_SIZE; i++) {
        uint8_t gcrCode = gcrEncodeTable[i];
        gcrDecodeTable[gcrCode] = i;
    }
}

// Encode 5-bit data to 6-bit GCR
uint8_t FloppyEmulator::encodeGCR(uint8_t data) {
    if (data < GCR_TABLE_SIZE) {
        return gcrEncodeTable[data];
    }
    return 0;  // Invalid, return 0
}

// Decode 6-bit GCR to 5-bit data
uint8_t FloppyEmulator::decodeGCR(uint8_t gcr) {
    if (gcrDecodeTable[gcr] != 0xFF) {
        return gcrDecodeTable[gcr];
    }
    return 0;  // Invalid GCR code
}

// Lookup table for phase detection - maps 4-bit phase state to phase number
// Valid states: 0x01 (PH0), 0x02 (PH1), 0x04 (PH2), 0x08 (PH3) - single phase
// Dual-phase states during transitions:
// 0x03 (PH0+PH1), 0x05 (PH0+PH2), 0x06 (PH1+PH2), 0x09 (PH0+PH3),
// 0x0A (PH1+PH3), 0x0C (PH2+PH3) - these are VALID during phase transitions
static const StepperPhase PHASE_LOOKUP[16] = {
    STEPPER_PHASE_0,  // 0x00 - invalid (no phase), but default to PH0
    STEPPER_PHASE_0,  // 0x01 - PH0 active
    STEPPER_PHASE_1,  // 0x02 - PH1 active
    STEPPER_PHASE_1,  // 0x03 - PH0+PH1 active (transition: new=PH1, old=PH0)
    STEPPER_PHASE_2,  // 0x04 - PH2 active
    STEPPER_PHASE_2,  // 0x05 - PH0+PH2 active (invalid - skip phases, ignore)
    STEPPER_PHASE_2,  // 0x06 - PH1+PH2 active (transition: new=PH2, old=PH1)
    STEPPER_PHASE_0,  // 0x07 - PH0+PH1+PH2 active (invalid, default to PH0)
    STEPPER_PHASE_3,  // 0x08 - PH3 active
    STEPPER_PHASE_3,  // 0x09 - PH0+PH3 active (transition: new=PH3, old=PH0, reverse direction)
    STEPPER_PHASE_3,  // 0x0A - PH1+PH3 active (invalid - skip phases, ignore)
    STEPPER_PHASE_0,  // 0x0B - invalid, default to PH0
    STEPPER_PHASE_3,  // 0x0C - PH2+PH3 active (transition: new=PH3, old=PH2)
    STEPPER_PHASE_0,  // 0x0D - invalid, default to PH0
    STEPPER_PHASE_0,  // 0x0E - invalid, default to PH0
    STEPPER_PHASE_0   // 0x0F - invalid, default to PH0
};

// Helper function to determine new phase from dual-phase state and current phase
// Controller behavior: when transitioning from PH1 to PH2, it first turns ON PH2, then turns OFF PH1
// So dual-phase states indicate the NEW phase that was just turned on
// Valid dual-phase transitions (based on current phase):
// - PH0: 0x03 (PH0+PH1) -> PH1 (outward), 0x09 (PH0+PH3) -> PH3 (inward)
// - PH1: 0x03 (PH0+PH1) -> PH0 (inward), 0x06 (PH1+PH2) -> PH2 (outward)
// - PH2: 0x06 (PH1+PH2) -> PH1 (inward), 0x0C (PH2+PH3) -> PH3 (outward)
// - PH3: 0x09 (PH0+PH3) -> PH0 (outward), 0x0C (PH2+PH3) -> PH2 (inward)
static StepperPhase getNewPhaseFromDualState(StepperPhase currentPhase, uint8_t dualPhaseState) {
    switch (dualPhaseState) {
        case 0x03: // PH0+PH1
            // If current is PH0, next is PH1 (outward)
            // If current is PH1, next is PH0 (inward)
            return (currentPhase == STEPPER_PHASE_0) ? STEPPER_PHASE_1 : STEPPER_PHASE_0;
        case 0x06: // PH1+PH2
            // If current is PH1, next is PH2 (outward)
            // If current is PH2, next is PH1 (inward)
            return (currentPhase == STEPPER_PHASE_1) ? STEPPER_PHASE_2 : STEPPER_PHASE_1;
        case 0x0C: // PH2+PH3
            // If current is PH2, next is PH3 (outward)
            // If current is PH3, next is PH2 (inward)
            return (currentPhase == STEPPER_PHASE_2) ? STEPPER_PHASE_3 : STEPPER_PHASE_2;
        case 0x09: // PH0+PH3
            // If current is PH0, next is PH3 (inward)
            // If current is PH3, next is PH0 (outward)
            return (currentPhase == STEPPER_PHASE_0) ? STEPPER_PHASE_3 : STEPPER_PHASE_0;
        default:
            // Invalid dual-phase state - return current phase (no change)
            return currentPhase;
    }
}

// Bit count lookup - maps phase state to number of active phases
static const uint8_t PHASE_COUNT[16] = {
    0, 1, 1, 2,  // 0x00-0x03
    1, 2, 2, 3,  // 0x04-0x07
    1, 2, 2, 3,  // 0x08-0x0B
    2, 3, 3, 4   // 0x0C-0x0F
};

// Detect stepper motor phase change from controller
// Adapted from ATMegaX code - optimized approach using gpio_get_all()
// Phases are on GPIO 6-9 (PH0-PH3), so we can read them with a single operation
// This matches the ATMegaX implementation logic exactly
void FloppyEmulator::detectStepperPhaseChange() {
    // Read all 4 phase pins at once (GPIO 6-9)
    // gpio_get_all() returns all GPIO states, shift right by GPIO_PH0 to get bits 6-9
    // Use volatile to ensure compiler doesn't optimize away reads
    volatile uint32_t gpio_all = gpio_get_all();
    uint8_t stp_pos = (gpio_all >> GPIO_PH0) & 0x0F;
    
    // NO printf() here - this runs on core1 and printf() is NOT thread-safe!
    // It will cause deadlock/blocking when called from core1
    
    // Map phase state to phase offset (matches ATMegaX logic exactly)
    // 0b0001=PH0 (ofs=0), 0b0010=PH1 (ofs=1), 0b0100=PH2 (ofs=2), 0b1000=PH3 (ofs=3)
    StepperPhase ofs;
    if (stp_pos == 0b00000001) {
        ofs = STEPPER_PHASE_0;
    } else if (stp_pos == 0b00000010) {
        ofs = STEPPER_PHASE_1;
    } else if (stp_pos == 0b00000100) {
        ofs = STEPPER_PHASE_2;
    } else if (stp_pos == 0b00001000) {
        ofs = STEPPER_PHASE_3;
    } else {
        // Invalid state (no phase or multiple phases) - don't process, keep lastPhaseOffset
        // This is expected when controller is idle or transitioning between phases
        return;
    }
    
    // We reach here only if stp_pos is valid (0x01, 0x02, 0x04, or 0x08)
    // Get local copy of lastPhaseOffset - simple read without critical section
    // Since we're on core1 and loadDiskImage() is on core0, reads are safe (atomic for single int)
    StepperPhase lastPhase = lastPhaseOffset;
    
    // Only process if phase offset changed
    if (ofs != lastPhase) {
        // Calculate direction: forward (outward) or backward (inward)
        // Matches ATMegaX logic exactly:
        // - DII_ph_track is physical track (0-69), increments/decrements with each phase change
        // - Logical track = DII_ph_track >> 1 (0-34)
        // - 2 physical steps = 1 logical track
        
        // Calculate direction based on phase sequence
        // Forward: PH0->PH1->PH2->PH3->PH0 (ofs == (old_ofs + 1) & 0x3)
        // Backward: PH0->PH3->PH2->PH1->PH0 (ofs == (old_ofs - 1) & 0x3)
        if (ofs == ((StepperPhase)((lastPhase + 1) & 0x3))) {
            // Moving forward (outward): PH0->PH1->PH2->PH3->PH0
            // Increase physical track (matches ATMegaX: DII_ph_track++)
            physicalTrack++;
        } else if (ofs == ((StepperPhase)((lastPhase - 1) & 0x3))) {
            // Moving backward (inward): PH0->PH3->PH2->PH1->PH0
            // Decrease physical track (matches ATMegaX: DII_ph_track--)
            physicalTrack--;
        }
        // If neither condition is true, it's an invalid transition (skipped phase) - ignore
        
        // Clamp physical track to valid range (0-69, matching ATMegaX)
        if (physicalTrack < 0) {
            physicalTrack = 0;
        } else if (physicalTrack > 69) {
            physicalTrack = 69;
        }
        
        // Calculate logical track from physical track (matches ATMegaX: DII_ph_track >> 1)
        currentTrack = physicalTrack >> 1;
        
        // Clamp logical track to valid range (0-34, matching Apple II Disk II)
        if (currentTrack < 0) {
            currentTrack = 0;
        } else if (currentTrack >= APPLE_II_TRACKS) {
            currentTrack = APPLE_II_TRACKS - 1;
        }
        
        // Update step position (0-1, based on physical track modulo 2)
        // Physical track 0,1 -> step 0; 2,3 -> step 1; 4,5 -> step 0; 6,7 -> step 1; etc.
        // This matches APPLE_II_STEPS_PER_TRACK = 2
        currentStep = (physicalTrack & 0x1);
        
        // Update last phase offset and current phase
        // These are simple assignments - safe on core1 (core0 won't write during normal operation)
        lastPhaseOffset = ofs;
        currentPhase = ofs;
        saveGCRCacheToDiskImage();
    }
}


// Process stepper motor (monitor controller signals)
void FloppyEmulator::processStepperMotor() {
    // Monitor phase changes from controller
    detectStepperPhaseChange();
}

// Get current track
int FloppyEmulator::getCurrentTrack() const {
    return currentTrack;
}

int FloppyEmulator::getCurrentSector() const {
    return currentSector;
}

// Set current track (for debugging/testing)
void FloppyEmulator::setCurrentTrack(int track) {
    if (track >= 0 && track < APPLE_II_TRACKS) {
        currentTrack = track;
        // Physical track = logical track * 2 (matches ATMegaX: DII_ph_track = logical * 2)
        physicalTrack = track * 2;
        currentStep = 0;  // Reset step position when manually setting track
        
        // Sync lastPhaseOffset with current phase from hardware
        uint8_t stp_pos = (gpio_get_all() >> GPIO_PH0) & 0x0F;
        if (stp_pos == 0b00000001) {
            lastPhaseOffset = STEPPER_PHASE_0;
            currentPhase = STEPPER_PHASE_0;
        } else if (stp_pos == 0b00000010) {
            lastPhaseOffset = STEPPER_PHASE_1;
            currentPhase = STEPPER_PHASE_1;
        } else if (stp_pos == 0b00000100) {
            lastPhaseOffset = STEPPER_PHASE_2;
            currentPhase = STEPPER_PHASE_2;
        } else if (stp_pos == 0b00001000) {
            lastPhaseOffset = STEPPER_PHASE_3;
            currentPhase = STEPPER_PHASE_3;
        }
    }
}

// Set current sector (for debugging/testing)
void FloppyEmulator::setCurrentSector(int sector) {
    if (sector >= 0 && sector < APPLE_II_SECTORS_PER_TRACK) {
        currentSector = sector;
    }
}

// Check if at track 0 (software tracking - no physical sensor)
bool FloppyEmulator::isAtTrack0() const {
    return (currentTrack == 0 && currentStep == 0);
}

// Calculate linear offset in disk image for track and sector
uint32_t FloppyEmulator::calculateTrackOffset(int track, int sector) {
    if (track < 0 || track >= APPLE_II_TRACKS) return 0;
    if (sector < 0 || sector >= APPLE_II_SECTORS_PER_TRACK) return 0;
    
    return (track * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR) +
           (sector * APPLE_II_BYTES_PER_SECTOR);
}
// Encode sector data to GCR format
void FloppyEmulator::encodeSectorGCR(const uint8_t* data, uint8_t* gcr, int length) {
    // GCR encoding: 5 data bits -> 6 GCR bits
    // Process in groups of 5 bytes (40 bits) -> 48 GCR bits (6 bytes)
    int gcrIndex = 0;
    
    for (int i = 0; i < length; i += 5) {
        // Take 5 bytes (40 bits) and encode to 6 bytes (48 bits)
        for (int j = 0; j < 5 && (i + j) < length; j++) {
            uint8_t byte = data[i + j];
            // Encode each 5-bit nibble
            uint8_t lowNibble = byte & 0x1F;
            uint8_t highNibble = (byte >> 5) & 0x07;  // Only 3 bits from high
            
            gcr[gcrIndex++] = encodeGCR(lowNibble);
            if (j < 4 || (i + j + 1) < length) {
                // Combine remaining bits
                uint8_t nextByte = (i + j + 1) < length ? data[i + j + 1] : 0;
                uint8_t combined = (highNibble << 2) | ((nextByte >> 6) & 0x03);
                gcr[gcrIndex++] = encodeGCR(combined);
            }
        }
    }
}

// Decode GCR format to sector data
void FloppyEmulator::decodeSectorGCR(const uint8_t* gcr, uint8_t* data, int length) {
    // GCR decoding: 6 GCR bits -> 5 data bits
    int dataIndex = 0;
    
    for (int i = 0; i < length && dataIndex < length; i += 6) {
        // Take 6 GCR bytes (48 bits) and decode to 5 bytes (40 bits)
        for (int j = 0; j < 5 && dataIndex < length; j++) {
            if (i + (j * 2) < length) {
                uint8_t gcrLow = decodeGCR(gcr[i + (j * 2)]);
                uint8_t gcrHigh = (i + (j * 2) + 1) < length ? decodeGCR(gcr[i + (j * 2) + 1]) : 0;
                
                data[dataIndex++] = (gcrLow & 0x1F) | ((gcrHigh & 0x1C) << 3);
            }
        }
    }
}

// Read sector from disk image
bool FloppyEmulator::readSector(int track, int sector, uint8_t* buffer) {
    if (track < 0 || track >= APPLE_II_TRACKS) return false;
    if (sector < 0 || sector >= APPLE_II_SECTORS_PER_TRACK) return false;
    if (buffer == nullptr) return false;
    
    uint32_t offset = calculateTrackOffset(track, sector);
    
    // Copy raw data from disk image
    for (int i = 0; i < APPLE_II_BYTES_PER_SECTOR; i++) {
        buffer[i] = diskImage[offset + i];
    }
    
    return true;
}

// Write sector to disk image
// Note: This is for CLI/debugging only. In real operation, writes come via processWriteBit()
// which respects the write enable signal from the controller.
bool FloppyEmulator::writeSector(int track, int sector, const uint8_t* buffer) {
    if (track < 0 || track >= APPLE_II_TRACKS) return false;
    if (sector < 0 || sector >= APPLE_II_SECTORS_PER_TRACK) return false;
    if (buffer == nullptr) return false;
    
    // Note: We don't check isWriteEnabled() here because this is a CLI/debug function
    // that should work regardless of controller state. Real writes via processWriteBit()
    // will respect the write enable signal.
    
    uint32_t offset = calculateTrackOffset(track, sector);
    
    // Copy raw data to disk image
    for (int i = 0; i < APPLE_II_BYTES_PER_SECTOR; i++) {
        diskImage[offset + i] = buffer[i];
    }
    
    return true;
}

// GCR bytes per sector in track cache (416 = 6656/16)
#define GCR_BYTES_PER_SECTOR_IN_CACHE  416

// Get GCR cache contents for a sector
// Uses current track from cache - each sector occupies 416 bytes in cache
bool FloppyEmulator::getGCRSectorFromCache(int sector, uint8_t* buffer, uint32_t maxLen, uint32_t* outLen) {
    if (sector < 0 || sector >= APPLE_II_SECTORS_PER_TRACK) return false;
    if (buffer == nullptr || outLen == nullptr) return false;
    
    // Check if cache is valid (for current track)
    if (gcrTrackCacheTrack < 0 || gcrTrackCacheTrack >= APPLE_II_TRACKS) {
        return false;  // Cache not initialized
    }
    
    // Calculate offset: each sector occupies 416 bytes in cache
    uint32_t offset = sector * GCR_BYTES_PER_SECTOR_IN_CACHE;
    if (offset + GCR_BYTES_PER_SECTOR_IN_CACHE > APPLE_II_GCR_BYTES_PER_TRACK) {
        return false;
    }
    
    uint32_t len = (maxLen < GCR_BYTES_PER_SECTOR_IN_CACHE) ? maxLen : GCR_BYTES_PER_SECTOR_IN_CACHE;
    for (uint32_t i = 0; i < len; i++) {
        buffer[i] = gcrTrackCache[offset + i];
    }
    *outLen = len;
    return true;
}

// Get current bit position on track
uint32_t FloppyEmulator::getCurrentBitPosition() {
    updateRotationPosition();
    return rotationPosition;
}

// Get GCR-encoded bit at raw bit position
// Apple II uses 5-and-3 GCR encoding: 5 data bits -> 6 GCR bits
// 5 bytes (40 bits) -> 6 bytes (48 bits) GCR
// Optimized version: calculate GCR bit directly from raw bit position
uint8_t FloppyEmulator::getGCRBitAtPosition(uint32_t rawBitPosition) {
    // Calculate track offset
    uint32_t trackOffset = currentTrack * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR;
    uint32_t bytesPerTrack = APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR;
    uint32_t bytePosition = rawBitPosition / 8;
    
    if (trackOffset + bytePosition >= APPLE_II_DISK_SIZE) {
        return 0;  // Out of bounds
    }
    
    // Calculate which 5-byte group (each group becomes 6 GCR bytes)
    uint32_t groupIndex = bytePosition / 5;
    uint32_t byteInGroup = bytePosition % 5;
    uint32_t bitInByte = rawBitPosition % 8;
    
    // Read the 5 bytes for this group
    uint8_t groupData[5];
    uint32_t groupStart = trackOffset + (groupIndex * 5);
    for (int i = 0; i < 5; i++) {
        uint32_t byteIdx = groupStart + i;
        if (byteIdx < APPLE_II_DISK_SIZE) {
            groupData[i] = diskImage[byteIdx];
        } else {
            groupData[i] = 0;
        }
    }
    
    // Encode to GCR: 5 bytes (40 bits) -> 6 GCR bytes (48 bits)
    // Apple II 5-and-3 GCR encoding: 5 data bits -> 6 GCR bits
    // Algorithm: Take 40 bits, split into 8 groups of 5 bits, encode each to 6 GCR bits
    // Result: 8 groups * 6 bits = 48 bits = 6 bytes
    uint8_t gcrData[6];
    
    // Build 40-bit stream from 5 bytes
    uint8_t bitStream[40];
    int bitIdx = 0;
    for (int i = 0; i < 5; i++) {
        for (int b = 7; b >= 0; b--) {
            bitStream[bitIdx++] = (groupData[i] >> b) & 0x01;
        }
    }
    
    // Encode 8 groups of 5 bits to 8 groups of 6 GCR bits
    for (int i = 0; i < 6; i++) {
        // Extract 5 bits starting at position (i * 5) from bit stream
        // But we need to map correctly: 6 GCR bytes = 48 bits
        // Each GCR byte contains 6 bits from encoding 5 data bits
        // So GCR byte i contains bits from data positions around (i * 40 / 48) * 5
        uint32_t dataBitStart = (i * 40) / 6;  // Approximate mapping
        if (dataBitStart + 5 > 40) dataBitStart = 40 - 5;  // Clamp to valid range
        
        // Extract 5 bits
        uint8_t fiveBits = 0;
        for (int b = 0; b < 5 && (dataBitStart + b) < 40; b++) {
            fiveBits = (fiveBits << 1) | bitStream[dataBitStart + b];
        }
        
        gcrData[i] = encodeGCR(fiveBits);
    }
    
    // Calculate which GCR bit corresponds to this raw bit position
    // Raw bit position within the 5-byte group (0-39)
    uint32_t rawBitInGroup = (rawBitPosition % 40);
    
    // Map to GCR bit position (0-47)
    // Linear mapping: raw bit N -> GCR bit (N * 48 / 40)
    uint32_t gcrBitPos = (rawBitInGroup * 48) / 40;
    uint32_t gcrByteIdx = gcrBitPos / 8;
    uint32_t gcrBitIdx = gcrBitPos % 8;
    
    if (gcrByteIdx < 6) {
        uint8_t gcrByte = gcrData[gcrByteIdx];
        return (gcrByte >> (7 - gcrBitIdx)) & 0x01;
    }
    
    return 0;
}

// Process read bit - output to controller via READ pin
void FloppyEmulator::processReadBit() {
    // Note: updateRotationPosition() is already called in process() before this
    // So rotationPosition is already up-to-date
    
    // Calculate which byte and bit to read from current track
    // Track data is organized as: track -> sectors -> bytes -> bits
    uint32_t bitsPerTrack = APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR * 8;
    uint32_t bitPosition = rotationPosition % bitsPerTrack;
    
    // Get GCR-encoded bit at this position
    // Apple II controller expects GCR-encoded data, not RAW data
    uint8_t gcrBit = getGCRBitAtPosition(bitPosition);
    
    // Output GCR bit to READ pin
    // Controller reads this pin at its own timing (synchronously)
    gpio_put(readPin, gcrBit);
}

// Process write bit - read from controller via WRITE pin
void FloppyEmulator::processWriteBit() {
    // Only accept writes if drive is selected and write is enabled
    if (!isDriveSelected() || !isWriteEnabled()) {
        return;
    }
    
    updateRotationPosition();
    
    // Read bit from WRITE pin (controller sends data here)
    uint8_t bit = gpio_get(writePin) ? 1 : 0;
    
    // Calculate which byte and bit to write
    uint32_t bitsPerTrack = APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR * 8;
    uint32_t bitPosition = rotationPosition % bitsPerTrack;
    int byteIndex = bitPosition / 8;
    int bitIndex = bitPosition % 8;
    
    // Write to current track
    uint32_t trackOffset = currentTrack * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR;
    if (trackOffset + byteIndex < APPLE_II_DISK_SIZE) {
        uint8_t byte = diskImage[trackOffset + byteIndex];
        if (bit) {
            byte |= (1 << (7 - bitIndex));
        } else {
            byte &= ~(1 << (7 - bitIndex));
        }
        diskImage[trackOffset + byteIndex] = byte;
    }
}

// Read one bit (for CLI/debugging - not used in real operation)
uint8_t FloppyEmulator::readBit() {
    updateRotationPosition();
    uint32_t bitsPerTrack = APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR * 8;
    uint32_t bitPosition = rotationPosition % bitsPerTrack;
    int byteIndex = bitPosition / 8;
    int bitIndex = bitPosition % 8;
    uint32_t trackOffset = currentTrack * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR;
    if (trackOffset + byteIndex < APPLE_II_DISK_SIZE) {
        uint8_t byte = diskImage[trackOffset + byteIndex];
        return (byte >> (7 - bitIndex)) & 0x01;
    }
    return 0;
}

// Write one bit directly to GCR cache at rotationPosition
// rotationPosition is in GCR bit units (0-53247), matching gcrTrackCache
// This is called from handleWriteIRQTimer() during write operations
void FloppyEmulator::writeBit(uint8_t bit) {
    if (!isWriteEnabled()) return;
    
    // Ensure GCR cache is valid for current track
    if (gcrTrackCacheTrack != currentTrack) {
        printf("updateGCRTrackCache\r\n");
        updateGCRTrackCache();
    }
    
    // rotationPosition is in GCR bits (0-53247), write directly to gcrTrackCache
    uint32_t gcrBitsPerTrack = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    uint32_t gcrBitPosition = rotationPosition % gcrBitsPerTrack;
    
    int byteIndex = gcrBitPosition / 8;
    int bitIndex = gcrBitPosition % 8;
    
    //printf("rotationPosition: %d, bit: %d\r\n", rotationPosition, bit);
    
    if (byteIndex < APPLE_II_GCR_BYTES_PER_TRACK) {
        uint8_t byte = gcrTrackCache[byteIndex];
        if (bit) {
            byte |= (1 << (7 - bitIndex));
        } else {
            byte &= ~(1 << (7 - bitIndex));
        }
        gcrTrackCache[byteIndex] = byte;
    }
}

// Load disk image from external source
void FloppyEmulator::loadDiskImage(const uint8_t* image, uint32_t size) {
    uint32_t copySize = size < APPLE_II_DISK_SIZE ? size : APPLE_II_DISK_SIZE;
    
    for (uint32_t i = 0; i < copySize; i++) {
        diskImage[i] = image[i];
    }
    
    // Clear remaining space if image is smaller
    for (uint32_t i = copySize; i < APPLE_II_DISK_SIZE; i++) {
        diskImage[i] = 0;
    }
    
    // Set initial track to 17 (0x11) for testing direction detection
    // This allows us to see if the system moves toward track 0 or track 34
    // BUT: Sync lastPhaseOffset with hardware FIRST, then set track
    // This ensures detectStepperPhaseChange() will work correctly
    
    // Sync lastPhaseOffset with current phase from hardware
    // Read current phase from GPIO to sync
    // IMPORTANT: If controller just reset, phases may be 0x00 (all LOW)
    // In this case, initialize to PHASE_0 as default - it will be synced on first valid phase change
    
    // Read GPIO state to sync phase - but don't block if controller is not ready
    // If controller just reset, phases may be 0x00 (all LOW) - that's OK, we'll sync on first valid phase
    uint32_t gpio_all = gpio_get_all();
    uint8_t stp_pos = (gpio_all >> GPIO_PH0) & 0x0F;
    
    // Determine which phase to initialize to
    StepperPhase initPhase = STEPPER_PHASE_0;  // Default - safe initial value
    
    if (stp_pos == 0b00000001) {
        initPhase = STEPPER_PHASE_0;
    } else if (stp_pos == 0b00000010) {
        initPhase = STEPPER_PHASE_1;
    } else if (stp_pos == 0b00000100) {
        initPhase = STEPPER_PHASE_2;
    } else if (stp_pos == 0b00001000) {
        initPhase = STEPPER_PHASE_3;
    }
    // If invalid state (0x00), keep initPhase = STEPPER_PHASE_0 (default)
    // detectStepperPhaseChange() will sync when controller activates first phase
    
    // CRITICAL: Use critical section ONLY for the actual updates to prevent race condition
    // Keep this section as SHORT as possible to avoid blocking
    // Note: This is called from UI handler (core1), but floppy emulation reads these on core0
    uint32_t save = save_and_disable_interrupts();
    
    // Update shared variables atomically - MINIMAL work in critical section
    lastPhaseOffset = initPhase;
    currentPhase = initPhase;
    currentTrack = 17;
    physicalTrack = 17 * 2;  // Physical track = logical track * 2 (matches ATMegaX)
    currentStep = 0;
    
    // Restore interrupts IMMEDIATELY
    restore_interrupts(save);
    
    // Invalidate GCR track cache - force regeneration with new disk image data
    // Cache will be regenerated automatically in process() when track is accessed
    gcrTrackCacheTrack = -1;
    gcrTrackCacheDirty = false;  // New disk loaded - cache is clean
    
    // NO printf() here - this may be called from core1 (UI handler) and printf() can block!
}

// Clear disk image
void FloppyEmulator::clearDiskImage() {
    for (uint32_t i = 0; i < APPLE_II_DISK_SIZE; i++) {
        diskImage[i] = 0;
    }
}

// Get pointer to disk image
uint8_t* FloppyEmulator::getDiskImage() {
    return diskImage;
}

// Get disk image size
uint32_t FloppyEmulator::getDiskImageSize() const {
    return APPLE_II_DISK_SIZE;
}

// Update rotation position (simulate disk rotation)
void FloppyEmulator::updateRotationPosition() {
    absolute_time_t now = get_absolute_time();
    int64_t diff = absolute_time_diff_us(lastBitTime, now);
    
    // Update rotation position based on time
    // rotationPosition represents GCR bit position in gcrTrackCache (for reading via PIO/DMA)
    // GCR cache has APPLE_II_GCR_BYTES_PER_TRACK bytes = 6656 * 8 = 53248 bits per track
    uint32_t gcrBitsPerTrack = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    
    // Calculate how many bits have elapsed since last update
    uint32_t bitsElapsed = diff / APPLE_II_BIT_PERIOD_US;
    
    // Update rotation position (wrap around at track length)
    // rotationPosition is now in GCR bit units to match gcrTrackCache
    rotationPosition = (rotationPosition + bitsElapsed) % gcrBitsPerTrack;
    
    // Calculate current sector from rotation position
    // Each sector is 416 bytes = 3328 bits in GCR cache
    // Sectors are laid out sequentially: sector 0 at bits 0-3327, sector 1 at bits 3328-6655, etc.
    const uint32_t GCR_BITS_PER_SECTOR = 416 * 8;  // 3328 bits per sector
    currentSector = (rotationPosition / GCR_BITS_PER_SECTOR) % APPLE_II_SECTORS_PER_TRACK;
    
    // Update lastBitTime to current time
    // This ensures continuous timing tracking
    lastBitTime = now;
}

// Update timing state
void FloppyEmulator::updateTiming() {
    // Note: updateRotationPosition() is already called in process() before this
    // So rotationPosition is already up-to-date
    
    // Update index pulse (once per rotation)
    // Index pulse occurs at rotation position 0
    // Use a small window to detect index pulse (first ~100 bits of track)
    // rotationPosition is now in GCR bits, so use GCR bits per track
    uint32_t gcrBitsPerTrack = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    indexPulse = (rotationPosition < 100) || (rotationPosition >= (gcrBitsPerTrack - 100));
}

// Synchronize to index pulse
void FloppyEmulator::syncToIndex() {
    // Wait for index pulse
    while (!indexPulse) {
        updateTiming();
        sleep_us(10);
    }
    rotationPosition = 0;
}

// Get bit period in microseconds
uint32_t FloppyEmulator::getBitPeriodUs() const {
    return APPLE_II_BIT_PERIOD_US;
}

// Check if this drive is selected by controller
bool FloppyEmulator::isDriveSelected() const {
    // Drive select is active low (inverted)
    // When selected, signal is LOW (0), when not selected, signal is HIGH (1)
    return gpio_get(driveSelPin) == 0;
}

// Check if write is enabled
bool FloppyEmulator::isWriteEnabled() const {
    return gpio_get(writeEnablePin) == 0;
}

// Set SD card manager for saving tracks
void FloppyEmulator::setSDCardManager(SDCardManager* sdCard) {
    sdCardManager = sdCard;
}

// Set current disk image filename
void FloppyEmulator::setCurrentFileName(const char* filename) {
    if (filename) {
        strncpy(currentFileName, filename, sizeof(currentFileName) - 1);
        currentFileName[sizeof(currentFileName) - 1] = 0;
    } else {
        currentFileName[0] = 0;
    }
}

// Timer callback function for precise bit timing
// This is called every 8 microseconds when timer is active
bool bitTimerCallback(repeating_timer_t *rt) {
    if (g_floppyEmulatorInstance) {
        g_floppyEmulatorInstance->processBitTimer();
    }
    return true;  // Continue repeating
}

// Process bit timer interrupt - output GCR bit at precise timing
// Timer runs at 1μs intervals to generate 1μs pulses for bit "1"
// Bit period is 4μs: bit "1" = 1μs HIGH + 3μs LOW, bit "0" = 4μs LOW
void FloppyEmulator::processBitTimer() {
    // Only output data if drive is selected
    if (!isDriveSelected()) {
        gpio_put(readPin, 0);
        bitPeriodPhase = 0;
        return;
    }
    
    // Increment bit period phase (every 1μs, wraps at 4μs)
    // Phase 0 = first 1μs (generate pulse if bit "1"), Phase 1-3 = remaining 3μs (always LOW)
    bitPeriodPhase++;
    if (bitPeriodPhase >= 4) {
        bitPeriodPhase = 0;
        
        // Start of new bit period (every 4μs) - increment rotation position
        // CRITICAL: Rotation position wraps at 50,000 bits (one rotation = 200ms at 4μs per bit)
        rotationPosition++;
        if (rotationPosition >= APPLE_II_BITS_PER_ROTATION) {
            rotationPosition = 0;
        }
        
        // TODO: Get GCR-encoded bit at current position
        // For now, output 0 to avoid blocking interrupt handler
        currentBitValue = 0;
    }
    
    // Generate pulse: if bit is "1" and phase is 0, output HIGH for first 1μs, else LOW
    gpio_put(readPin, (bitPeriodPhase == 0 && currentBitValue == 1) ? 1 : 0);
}

// Start hardware timer for precise bit-level timing
void FloppyEmulator::startBitTimer() {
    if (timerActive) {
        return;  // Already active
    }
    
    // Set static instance pointer for callback
    g_floppyEmulatorInstance = this;
    
    // Reset bit period phase
    bitPeriodPhase = 0;
    currentBitValue = 0;
    
    // Start repeating timer: every 1 microsecond (for 1μs pulse generation)
    // Negative delay means absolute time (more precise)
    // Timer runs at 1μs to generate 1μs pulses for bit "1"
    if (add_repeating_timer_us(-APPLE_II_PULSE_WIDTH_US, bitTimerCallback, nullptr, &bitTimer)) {
        timerActive = true;
    }
}

// Stop hardware timer
void FloppyEmulator::stopBitTimer() {
    if (!timerActive) {
        return;  // Already stopped
    }
    
    cancel_repeating_timer(&bitTimer);
    timerActive = false;
    bitPeriodPhase = 0;
    currentBitValue = 0;
    g_floppyEmulatorInstance = nullptr;
}

// Write value in AA format (used for address field encoding)
static void writeAAVal(uint8_t val, uint8_t* buffer, uint32_t* index) {
    buffer[(*index)++] = 0xAA | (val >> 1);
    buffer[(*index)++] = 0xAA | val;
}

// Update GCR track cache for current track (Apple II NIC format)
void FloppyEmulator::updateGCRTrackCache() {
    uint32_t gcrIndex = 0;
    uint32_t maxGcrBytes = APPLE_II_GCR_BYTES_PER_TRACK;
    uint8_t volume = 0xFE;  // Default volume
    uint8_t src[256 + 2];
    
    // Get track offset in disk image
    uint32_t trackOffset = currentTrack * APPLE_II_BYTES_PER_TRACK;
    
    // Process each sector in track (using scrambled sector order)
    // Each sector is exactly 416 bytes, total track is 6656 bytes (16 * 416)
    // Process all 16 sectors (don't use gcrIndex check - it prevents last sector from being generated)
    for (int sector = 0; sector < APPLE_II_SECTORS_PER_TRACK; sector++) {
        int scrambledSector = SECTOR_SCRAMBLE[sector];
        uint32_t sectorOffset = trackOffset + (scrambledSector * APPLE_II_BYTES_PER_SECTOR);
        
        // 22 sync bytes (0xFF)
        for (int i = 0; i < 22 && gcrIndex < maxGcrBytes; i++) {
            gcrTrackCache[gcrIndex++] = 0xFF;
        }
        
        // Sync pattern
        if (gcrIndex + 11 <= maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0x03;
            gcrTrackCache[gcrIndex++] = 0xFC;
            gcrTrackCache[gcrIndex++] = 0xFF;
            gcrTrackCache[gcrIndex++] = 0x3F;
            gcrTrackCache[gcrIndex++] = 0xCF;
            gcrTrackCache[gcrIndex++] = 0xF3;
            gcrTrackCache[gcrIndex++] = 0xFC;
            gcrTrackCache[gcrIndex++] = 0xFF;
            gcrTrackCache[gcrIndex++] = 0x3F;
            gcrTrackCache[gcrIndex++] = 0xCF;
            gcrTrackCache[gcrIndex++] = 0xF3;
            gcrTrackCache[gcrIndex++] = 0xFC;
        }
        
        // Address field prologue
        if (gcrIndex + 3 <= maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0xD5;
            gcrTrackCache[gcrIndex++] = 0xAA;
            gcrTrackCache[gcrIndex++] = 0x96;
        }
        
        // Address field: volume, track, sector, checksum (using writeAAVal format)
        if (gcrIndex + 8 <= maxGcrBytes) {
            writeAAVal(volume, gcrTrackCache, &gcrIndex);
            writeAAVal(currentTrack, gcrTrackCache, &gcrIndex);
            writeAAVal(sector, gcrTrackCache, &gcrIndex);
            writeAAVal(volume ^ currentTrack ^ sector, gcrTrackCache, &gcrIndex);
        }
        
        // Address field epilogue
        if (gcrIndex + 3 <= maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0xDE;
            gcrTrackCache[gcrIndex++] = 0xAA;
            gcrTrackCache[gcrIndex++] = 0xEB;
        }
        
        // 5 sync bytes (0xFF)
        for (int i = 0; i < 5 && gcrIndex < maxGcrBytes; i++) {
            gcrTrackCache[gcrIndex++] = 0xFF;
        }
        
        // Data field prologue
        if (gcrIndex + 3 <= maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0xD5;
            gcrTrackCache[gcrIndex++] = 0xAA;
            gcrTrackCache[gcrIndex++] = 0xAD;
        }
        
        // Read sector data
        for (int i = 0; i < 256; i++) {
            if (sectorOffset + i < APPLE_II_DISK_SIZE) {
                src[i] = diskImage[sectorOffset + i];
            } else {
                src[i] = 0;
            }
        }
        src[256] = src[257] = 0;
        
        // Encode first 86 bytes (special encoding with FlipBit tables)
        uint8_t ox = 0;
        for (int i = 0; i < 86 && gcrIndex < maxGcrBytes; i++) {
            uint8_t x = (FLIP_BIT1[src[i] & 3] | FLIP_BIT2[src[i + 86] & 3] | FLIP_BIT3[src[i + 172] & 3]);
            uint8_t encoded = NIC_ENCODE_TABLE[(x ^ ox) & 0x3F];
            gcrTrackCache[gcrIndex++] = encoded;
            ox = x;
        }
        
        // Encode all 256 bytes (standard encoding: src[i] >> 2)
        for (int i = 0; i < 256 && gcrIndex < maxGcrBytes; i++) {
            uint8_t x = (src[i] >> 2);
            uint8_t encoded = NIC_ENCODE_TABLE[(x ^ ox) & 0x3F];
            gcrTrackCache[gcrIndex++] = encoded;
            ox = x;
        }
        
        // Last byte
        if (gcrIndex < maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = NIC_ENCODE_TABLE[ox & 0x3F];
        }
        
        // Data field epilogue
        if (gcrIndex + 3 <= maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0xDE;
            gcrTrackCache[gcrIndex++] = 0xAA;
            gcrTrackCache[gcrIndex++] = 0xEB;
        }
        
        // 14 sync bytes (0xFF)
        for (int i = 0; i < 14 && gcrIndex < maxGcrBytes; i++) {
            gcrTrackCache[gcrIndex++] = 0xFF;
        }
        
        // Total per sector should be exactly 416 bytes = 6656 bytes per track (16 * 416)
        // Add 1 padding byte to reach 416 bytes per sector
        //if (gcrIndex < maxGcrBytes) {
        //    gcrTrackCache[gcrIndex++] = 0x00;
        //}
    }
    
    // Fill remaining cache with zeros if needed
    // NOTE: This should NOT execute if all 16 sectors are correctly written (16 * 416 = 6656 bytes)
    // If it does execute, it might overwrite part of the last sector, so we add a safety check
    // Only fill if we're significantly short (more than 1 byte), to avoid overwriting sector 15
    if (gcrIndex < maxGcrBytes - 1) {
        while (gcrIndex < maxGcrBytes) {
            gcrTrackCache[gcrIndex++] = 0;
        }
    }
    
    gcrTrackCacheTrack = currentTrack;
    gcrTrackCacheBits = gcrIndex * 8;  // Total GCR bits in cache
    gcrTrackCacheDirty = false;  // Cache is clean after loading
}

// Save GCR cache back to disk image
// Called before track change if the cache has been modified (dirty)
// Decodes all 16 sectors from GCR cache and writes them to the disk image
void FloppyEmulator::saveGCRCacheToDiskImage() {
    //printf("saveGCRCacheToDiskImage\r\n");
    // Only save if cache is valid and dirty
    if (gcrTrackCacheTrack < 0 || gcrTrackCacheTrack >= APPLE_II_TRACKS || !gcrTrackCacheDirty) {
        return;
    }
    
    //printf("saveGCRCacheToDiskImage: Saving track %d to disk image...\r\n", gcrTrackCacheTrack);
    
    // GCR cache sector structure (416 bytes per sector):
    // - 22 sync bytes (0xFF)
    // - 12 sync pattern bytes  
    // - 3 bytes address prologue (D5 AA 96)
    // - 8 bytes address data
    // - 3 bytes address epilogue (DE AA EB)
    // - 5 sync bytes (0xFF)
    // - 3 bytes data prologue (D5 AA AD)  <- offset 53
    // - 343 bytes NIC-encoded data        <- offset 56
    // - 3 bytes data epilogue (DE AA EB)
    // - 14 sync bytes (0xFF)
    
    const uint32_t SECTOR_SIZE = 416;
    const uint32_t DATA_PROLOGUE_OFFSET = 53;  // 22 + 12 + 3 + 8 + 3 + 5 = 53
    const uint32_t DATA_OFFSET = 56;           // DATA_PROLOGUE_OFFSET + 3
    const uint32_t DATA_FIELD_SIZE = 343;      // NIC-encoded data bytes
    
    uint32_t trackOffset = gcrTrackCacheTrack * APPLE_II_BYTES_PER_TRACK;
    int sectorsDecoded = 0;
    int sectorsSkipped = 0;
    
    for (int physicalSector = 0; physicalSector < APPLE_II_SECTORS_PER_TRACK; physicalSector++) {
        uint32_t sectorStart = physicalSector * SECTOR_SIZE;
        uint32_t dataProloguePos = sectorStart + DATA_PROLOGUE_OFFSET;
        uint32_t dataPos = sectorStart + DATA_OFFSET;
        
        // Verify data prologue (D5 AA AD)
        if (dataProloguePos + 3 > APPLE_II_GCR_BYTES_PER_TRACK) {
            printf("  Sector %d: prologue position overflow\r\n", physicalSector);
            sectorsSkipped++;
            continue;
        }
        
        if (gcrTrackCache[dataProloguePos] != 0xD5 ||
            gcrTrackCache[dataProloguePos + 1] != 0xAA ||
            gcrTrackCache[dataProloguePos + 2] != 0xAD) {
            // Data field prologue not found at expected position - skip this sector
            // This is normal for sectors that weren't written to
            sectorsSkipped++;
            continue;
        }
        
        // Verify we have enough data
        if (dataPos + DATA_FIELD_SIZE > APPLE_II_GCR_BYTES_PER_TRACK) {
            printf("  Sector %d: data overflow\r\n", physicalSector);
            sectorsSkipped++;
            continue;
        }
        
        // Decode NIC data field
        uint8_t decodedData[256];
        uint16_t decodedLength = 0;
        
        decodeNICDataField(&gcrTrackCache[dataPos], DATA_FIELD_SIZE, decodedData, &decodedLength);
        
        if (decodedLength != 256) {
            printf("  Sector %d: decode failed (got %u bytes, expected 256)\r\n", physicalSector, decodedLength);
            sectorsSkipped++;
            continue;
        }
        
        // Get logical sector from physical sector using SECTOR_SCRAMBLE
        // Physical sector N in GCR cache contains data for logical sector SECTOR_SCRAMBLE[N]
        int logicalSector = SECTOR_SCRAMBLE[physicalSector];
        
        // Calculate position in disk image
        uint32_t sectorOffset = trackOffset + (logicalSector * APPLE_II_BYTES_PER_SECTOR);
        
        // Copy decoded data to disk image
        for (int i = 0; i < 256; i++) {
            diskImage[sectorOffset + i] = decodedData[i];
        }
        
        sectorsDecoded++;
    }
    
    // Save track to SD card file if SD card manager and filename are available
    if (sdCardManager && currentFileName[0] != 0 && sectorsDecoded > 0) {
        // Get track data from diskImage (already decoded and written above)
        uint8_t* trackData = &diskImage[trackOffset];
        uint32_t trackSize = APPLE_II_BYTES_PER_TRACK;  // 4096 bytes per track
        
        // Save track to file (silently fail if error occurs)
        sdCardManager->saveTrackToFile(currentFileName, gcrTrackCacheTrack, trackData, trackSize);
    }
    
    // Mark cache as clean
    gcrTrackCacheDirty = false;
}

// DMA IRQ handler for fast restart when transfer completes
// Fast handler that resets read address and restarts transfer
void dma_irq_handler() {
    if (g_floppyEmulatorInstance) {
        g_floppyEmulatorInstance->handleDMAIRQ();
    }
}

// Initialize PIO and DMA for continuous bit output
void FloppyEmulator::initPIO_DMA() {
    // Claim a PIO instance and state machine
    pio = pio0;
    pioSm = pio_claim_unused_sm(pio, true);
    if (pioSm < 0) {
        // Try pio1 if pio0 is full
        pio = pio1;
        pioSm = pio_claim_unused_sm(pio, true);
        if (pioSm < 0) {
            // No available state machine - this should not happen
            return;
        }
    }
    
    // Load PIO program
    pioOffset = pio_add_program(pio, &floppy_bit_output_program);
    
    // Initialize PIO state machine (but DON'T start it yet - wait for DMA)
    pio_sm_config c = floppy_bit_output_program_get_default_config(pioOffset);
    sm_config_set_out_pins(&c, readPin, 1);
    sm_config_set_set_pins(&c, readPin, 1);
    sm_config_set_out_shift(&c, false, true, 8);  // Shift right, autopull after 8 bits
    
    pio_gpio_init(pio, readPin);
    pio_sm_set_consecutive_pindirs(pio, pioSm, readPin, 1, true);
    
    // Clock divider: system clock / 2MHz = 200MHz / 2MHz = 100
    // At 2MHz: 1 instruction = 0.5μs
    // PIO program timing per bit:
    //   - out pins, 1 = 0.5μs (sets bit value)
    //   - nop = 0.5μs (HIGH total = 1μs for bit "1")
    //   - set pins, 0 = 0.5μs (sets LOW)
    //   - nop [4] = 5 cycles = 2.5μs (LOW total = 3μs)
    //   - Total = 4μs per bit ✓
    float div = (float)clock_get_hz(clk_sys) / 2200000.0f;  // 2MHz clock
    sm_config_set_clkdiv(&c, div);
    
    pio_sm_init(pio, pioSm, pioOffset, &c);
    // DON'T start PIO SM yet - it will be started when DMA is ready
    pio_sm_set_enabled(pio, pioSm, false);
    
    // Claim DMA channel
    dmaChannel = dma_claim_unused_channel(true);
    if (dmaChannel < 0) {
        // No available DMA channel - this should not happen
        return;
    }
    
    // Set up DMA IRQ handler for automatic restart
    // Use IRQ0 for channels 0-3, IRQ1 for channels 4-11
    // For now, assume channel < 4 (most likely)
    if (g_floppyEmulatorInstance == nullptr) {
        g_floppyEmulatorInstance = this;  // Set instance for IRQ handler
    }
    
    // PIO/DMA will be started in init() after cache is ready
    pioDmaActive = false;
}

// Start PIO/DMA streaming from cache buffer
void FloppyEmulator::startPIO_DMA() {
    if (pioDmaActive) {
        return;  // Already active
    }
    
    if (dmaChannel < 0 || pio == nullptr) {
        return;  // PIO/DMA not initialized
    }
    
    // Configure DMA channel for circular transfer from cache to PIO TX FIFO
    // Use self-chaining for automatic restart without IRQ - seamless continuous transfer
    dmaConfig = dma_channel_get_default_config(dmaChannel);
    channel_config_set_transfer_data_size(&dmaConfig, DMA_SIZE_8);  // Transfer bytes
    channel_config_set_dreq(&dmaConfig, pio_get_dreq(pio, pioSm, true));  // DREQ from PIO TX FIFO
    channel_config_set_read_increment(&dmaConfig, true);   // Increment read address (cache buffer)
    channel_config_set_write_increment(&dmaConfig, false); // Don't increment write address (PIO FIFO)
    
    // KEY: Self-chaining - channel restarts itself automatically when transfer completes
    // IMPORTANT: When self-chaining, we need to also reset the read address
    // The chain will restart the transfer count, but we need to reset read address manually
    // OR use a second channel to reset it. For now, let's use IRQ but make it faster.
    // Actually, let's try: configure chain, but also set up to reset read address via chain trigger
    // Better: Use ring buffer mode if possible, or go back to IRQ with fast restart
    
    // For now, let's use a simpler approach: IRQ handler that just resets read address
    // and restarts - this is still fast and reliable
    // channel_config_set_chain_to(&dmaConfig, dmaChannel);  // Commented out - needs read address reset
    
    // Set up circular transfer: transfer count = cache size
    dma_channel_configure(
        dmaChannel,
        &dmaConfig,
        &pio->txf[pioSm],              // Write to PIO TX FIFO
        gcrTrackCache,                  // Read from cache buffer
        APPLE_II_GCR_BYTES_PER_TRACK,  // Transfer count
        false                           // Don't start yet - configure first
    );
    
    // Enable DMA IRQ for automatic restart when transfer completes
    // We'll use a fast IRQ handler that just resets read address and restarts
    if (dmaChannel < 4) {
        // Channels 0-3 use IRQ0
        irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
        irq_set_enabled(DMA_IRQ_0, true);
        dma_channel_set_irq0_enabled(dmaChannel, true);
    } else {
        // Channels 4-11 use IRQ1
        irq_set_exclusive_handler(DMA_IRQ_1, dma_irq_handler);
        irq_set_enabled(DMA_IRQ_1, true);
        dma_channel_set_irq1_enabled(dmaChannel, true);
    }
    
    // Start PIO state machine first (it will wait for data from DMA via pull)
    pio_sm_set_enabled(pio, pioSm, true);
    
    // Now start DMA transfer (will start feeding data to PIO FIFO)
    dma_channel_start(dmaChannel);
    
    pioDmaActive = true;
}

// Stop PIO/DMA streaming
void FloppyEmulator::stopPIO_DMA() {
    if (!pioDmaActive) {
        return;  // Already stopped
    }
    
    if (dmaChannel >= 0) {
        // Disable DMA IRQ before aborting
        if (dmaChannel < 4) {
            dma_channel_set_irq0_enabled(dmaChannel, false);
        } else {
            dma_channel_set_irq1_enabled(dmaChannel, false);
        }
        
        // Abort DMA transfer
        dma_channel_abort(dmaChannel);
    }
    
    if (pio != nullptr) {
        // Disable PIO state machine
        pio_sm_set_enabled(pio, pioSm, false);
    }
    
    // Set READ pin low when stopped
    gpio_put(readPin, 0);
    
    pioDmaActive = false;
}

// Handle DMA IRQ - fast restart by resetting read address and restarting transfer
void FloppyEmulator::handleDMAIRQ() {
    // This is called from IRQ handler, so keep it fast!
    // CRITICAL: Don't process DMA IRQ during write operation - it can interfere with write timing
    if (writeIrqTimerActive) {
        return;  // Write in progress - skip DMA restart to avoid interference
    }
    
    if (!pioDmaActive || dmaChannel < 0) {
        return;  // Not active
    }
    
    // Clear IRQ flag FIRST to avoid multiple triggers
    if (dmaChannel < 4) {
        dma_hw->ints0 = 1u << dmaChannel;
    } else {
        dma_hw->ints1 = 1u << dmaChannel;
    }
    
    // Fast restart: Just reset read address and restart transfer
    // This is faster than full dma_channel_configure - single register write
    dma_channel_set_read_addr(dmaChannel, gcrTrackCache, true);
}

// Main processing loop - react to controller signals
void FloppyEmulator::process() {
    // NO printf() here - this runs on core0 and printf() can block!
    
    // CRITICAL: During write operation, minimize processing to avoid interference
    // Only do essential operations during write to prevent timing issues
    if (writeIrqTimerActive) {
        // Write in progress - only do critical operations
        // Stepper motor tracking is still needed for rotationPosition accuracy
        processStepperMotor();
        // Update rotation position (needed for write bit positioning)
        updateRotationPosition();
        // Skip cache updates and other non-critical operations during write
        return;
    }
    
    // CRITICAL: Monitor stepper motor phase changes FIRST - HIGHEST PRIORITY
    // This MUST run even when drive is not selected - we need to track position always
    // This is the highest priority - must catch all phase changes
    // Especially important for fast machine code operations like catalog
    processStepperMotor();
    
    // PIO/DMA should run continuously - don't stop it based on drive selection
    // In real Apple II floppy drive, READ pin constantly outputs data when drive is spinning
    // Update cache if track changed (don't abort DMA - let it finish current cycle first)
    if (gcrTrackCacheTrack != currentTrack) {
        updateGCRTrackCache();
        // Don't abort DMA here - it will restart with new cache when current cycle completes
        // This avoids interruptions in the continuous stream
    }
   
    // DMA restart is now handled by IRQ handler for faster response
    // No need to poll here - IRQ handler will restart DMA immediately when it completes
    
    // Update rotation position
    updateRotationPosition();
    
    // Update timing state (index pulse, etc.)
    updateTiming();
}

//================================================================================
//================================================================================
//================================================================================
//====    WRITE функции за запис на диск
//================================================================================
//================================================================================
//================================================================================
//================================================================================
//================================================================================
//================================================================================
#define PWM_TIMER_VALUE 160  // 160
void FloppyEmulator::initWritePWMTimer() {
    // Use PWM slice 0 directly (don't need GPIO, just the timer functionality)
    // GPIO 0 maps to slice 0, but we won't actually use the GPIO output
    slice_num = 0;  // Use slice 0 for timer

    // Configure divider: 150MHz / 5.0 = 30MHz (1 cycle = 33.3ns)
    // Or 200MHz / 5.0 = 40MHz (1 cycle = 25ns)
    // PWM_TIMER_VALUE=160 * 25ns = 4μs period
    pwm_set_clkdiv(slice_num, 5.0f);
    
    // Set wrap value for 4μs period
    pwm_set_wrap(slice_num, PWM_TIMER_VALUE);

    // Configure IRQ
//    pwm_clear_irq(slice_num);
//    pwm_set_irq_enabled(slice_num, true);
//    irq_set_exclusive_handler(PWM_IRQ_WRAP, pwm_irq_handler);
//    irq_set_priority(PWM_IRQ_WRAP, 1);
//    irq_set_enabled(PWM_IRQ_WRAP, true);

    // Start timer
    pwm_set_enabled(slice_num, true);
}
//================================================================================
//================================================================================
//================================================================================
// Start PWM timer (called when write operation starts)
void FloppyEmulator::startWritePWMTimer() {
    pwm_set_enabled(slice_num, true);
}
//-----------------------------------------------

// Stop PWM timer (called when write operation ends)
void FloppyEmulator::stopWritePWMTimer() {
    pwm_set_enabled(slice_num, false);
}
//-----------------------------------------------
// Declare lastPwmCounter as extern so we can reset it
extern uint16_t lastPwmCounter;

void FloppyEmulator::resetWritePWMTimer() {
    // Reset counter to 0
    pwm_set_counter(slice_num, 0);
    pwm_set_enabled(slice_num, true);
    // Reset last counter tracking variable to prevent false wrap detection
    lastPwmCounter = 0;
}
//-----------------------------------------------
// Check if PWM timer has overflowed by checking counter value directly
// More reliable than interrupt flag which can have clearing issues
uint16_t lastPwmCounter = 0;

bool FloppyEmulator::checkPWMOverflow() {
    // Read current counter value
    uint16_t counter = pwm_get_counter(slice_num);
    
    // Detect wrap: if current counter < last counter, wrap occurred
    // (counter went from ~160 back to ~0)
    if (counter < lastPwmCounter) {
        lastPwmCounter = counter;
        return true;  // Wrap detected = 4μs elapsed
    }
    
    lastPwmCounter = counter;
    return false;
}
//-----------------------------------------------
//-----------------------------------------------
bool FloppyEmulator::floppy_write_in() {
    return gpio_get(writePin) ? 1 : 0;
}
//-----------------------------------------------
// Initialize write state when write operation starts (falling edge of WRITE_EN)
// Based on AVR code init_writing()
void FloppyEmulator::initWriting() {
    writeSynced = false;  // Not synced yet - looking for prologue D5 AA 96
    writeBitCount = 0;
    writeData = 0;
    writeBufferIndex = 0;
    prologBuffer[0] = 0;
    prologBuffer[1] = 0;
    prologBuffer[2] = 0;
    prologBitCount = 0;  // Reset prolog bit counter
    // Read initial state to detect first transition
    lastWritePinState = gpio_get(writePin) ? 1 : 0;
}

// Finalize write and save sector when write operation ends (rising edge of WRITE_EN)
// Based on AVR code end_writing() and write_back()
void FloppyEmulator::endWriting() {
    // Process any remaining data and write sector if needed
    writeBack();
}
//-----------------------------------------------
//-----------------------------------------------
void FloppyEmulator::startWritingProcedure() {
            // Falling edge detected - start the PIO IRQ timer
        // Timer will run at 4μs intervals and repeat until rising edge
        
        // CRITICAL: Avoid printf() in IRQ context - it can block!
        // Only use minimal operations here
        
        // If timer is already active, stop it first (shouldn't happen, but safety check)
        //if (writeIrqTimerActive) {
            stopWritePWMTimer();
            //}
            
            // Save DMA position at write start - this is when the controller has just read the address field
            // and is about to write the data field. We search backwards from this position to find the sector.
            currentSectortoWrite = -1;  // Logical sector (unknown until we find address field)
            physicalSectorToWrite = -1; // Physical sector (for GCR cache position)
            dmaPositionAtWriteStart = 0;
            
            if (dmaChannel >= 0 && pioDmaActive) {
                uint32_t dmaReadAddr = dma_channel_hw_addr(dmaChannel)->read_addr;
                uint32_t cacheOffset = dmaReadAddr - (uint32_t)gcrTrackCache;
                cacheOffset = cacheOffset % APPLE_II_GCR_BYTES_PER_TRACK;  // Wrap around
                dmaPositionAtWriteStart = cacheOffset;
                
                // Search backwards for address field prologue (D5 AA 96) - controller just read it
                // The controller reads address field, then enables write for data field
                // So we should find D5 AA 96 within a few hundred bytes backwards
                for (int searchBack = 0; searchBack < 450; searchBack++) {
                    int pos = (int)cacheOffset - searchBack;
                    if (pos < 0) pos += APPLE_II_GCR_BYTES_PER_TRACK;
                    
                    // Check for D5 AA 96 (address field prologue)
                    int pos1 = (pos + 1) % APPLE_II_GCR_BYTES_PER_TRACK;
                    int pos2 = (pos + 2) % APPLE_II_GCR_BYTES_PER_TRACK;
                    
                    if (gcrTrackCache[pos] == 0xD5 && 
                        gcrTrackCache[pos1] == 0xAA &&
                        gcrTrackCache[pos2] == 0x96) {
                        
                        // Found address field! Decode sector from positions +7 and +8
                        int sectorPos1 = (pos + 7) % APPLE_II_GCR_BYTES_PER_TRACK;
                        int sectorPos2 = (pos + 8) % APPLE_II_GCR_BYTES_PER_TRACK;
                        uint8_t sectorEncoded1 = gcrTrackCache[sectorPos1];
                        uint8_t sectorEncoded2 = gcrTrackCache[sectorPos2];
                        uint8_t physicalSector = ((sectorEncoded1 & 0x55) << 1) | (sectorEncoded2 & 0x55);
                        
                        // Save physical sector (for GCR cache position)
                        physicalSectorToWrite = physicalSector;
                        
                        // Convert physical sector to logical sector using SECTOR_SCRAMBLE
                        // In GCR cache: physical sector N contains data from logical sector SECTOR_SCRAMBLE[N]
                        // So when we find physical sector N, the logical sector is SECTOR_SCRAMBLE[N]
                        if (physicalSector < 16) {
                            currentSectortoWrite = SECTOR_SCRAMBLE[physicalSector];
                        } else {
                            currentSectortoWrite = physicalSector;  // Invalid, keep as-is
                        }
                        break;
                    }
                }
            }
            
            // Initialize write state without printf (printf can block in IRQ context)
            writeSynced = false;
            writeBitCount = 0;
            writeData = 0;
            writeBufferIndex = 0;
            writeBitsProcessed = 0;  // Count bits processed (skip 25th bit)
            lastWritePinState = gpio_get(writePin) ? 1 : 0;
            
            // Initialize raw bit buffer state
            rawBitData = 0;
            rawBitCount = 0;
            rawBitBufferIndex = 0;
            
            // ============================================================
            // CRITICAL: Stop ALL potentially interfering processes
            // ============================================================
            
            // 1. STOP PIO/DMA read completely (not just disable IRQ)
            if (dmaChannel >= 0 && pioDmaActive) {
                // Disable DMA IRQ first
                if (dmaChannel < 4) {
                    dma_channel_set_irq0_enabled(dmaChannel, false);
                } else {
                    dma_channel_set_irq1_enabled(dmaChannel, false);
                }
                // Abort DMA transfer
                dma_channel_abort(dmaChannel);
                // Stop PIO state machine
                if (pio != nullptr) {
                    pio_sm_set_enabled(pio, pioSm, false);
                }
            }
            
            // 2. Disable GPIO IRQs
            gpio_set_irq_enabled(writeEnablePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
            gpio_set_irq_enabled(writePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
            
            // 3. Disable PWM IRQ (we poll manually)
            irq_set_enabled(PWM_IRQ_WRAP, false);
            
            // 4. Disable DMA IRQ completely
            irq_set_enabled(DMA_IRQ_0, false);
            irq_set_enabled(DMA_IRQ_1, false);
            
            // Reset and start PWM timer for 4μs period timing
            resetWritePWMTimer();
}
//-----------------------------------------------
void FloppyEmulator::stopWritingProcedure() {
        // ============================================================
        // Write complete - restore all processes
        // ============================================================
        
        // Stop PWM timer
        stopWritePWMTimer();
        
        // Process captured data FIRST (before re-enabling interrupts)
        endWriting();
        
        // ============================================================
        // Re-enable everything that was disabled
        // ============================================================
        
        // 1. Re-enable DMA IRQs
        irq_set_enabled(DMA_IRQ_0, true);
        irq_set_enabled(DMA_IRQ_1, true);
        
        // 2. Re-enable PWM IRQ
        irq_set_enabled(PWM_IRQ_WRAP, true);
        
        // 3. Re-enable GPIO IRQs
        gpio_set_irq_enabled(writeEnablePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
        gpio_set_irq_enabled(writePin, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
        
        // 4. RESTART PIO/DMA read
        if (dmaChannel >= 0 && pioDmaActive) {
            // Re-enable DMA channel IRQ
            if (dmaChannel < 4) {
                dma_channel_set_irq0_enabled(dmaChannel, true);
            } else {
                dma_channel_set_irq1_enabled(dmaChannel, true);
            }
            
            // Restart PIO state machine
            if (pio != nullptr) {
                pio_sm_set_enabled(pio, pioSm, true);
            }
            
            // Restart DMA transfer from current position
            dma_channel_set_read_addr(dmaChannel, gcrTrackCache, true);
        }
}
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
//-----------------------------------------------
void FloppyEmulator::addBitToRAWBuffer(uint8_t bit) {
    if (rawBitBufferIndex < RAW_BIT_BUFFER_SIZE) {
        rawBitData = (rawBitData << 1) | bit;
        rawBitCount++;
        if (rawBitCount == 8) {
            // Byte complete - store in raw bit buffer
            rawBitBuffer[rawBitBufferIndex++] = rawBitData;
            rawBitData = 0;
            rawBitCount = 0;
        }
    }
}
//-----------------------------------------------

// Handle WRITE_EN GPIO IRQ - detects when write head is activated/deactivated
// Called from GPIO IRQ handler when WRITE_EN pin changes state (rising or falling edge)
// Write head is active when WRITE_EN = 1 (HIGH)
void FloppyEmulator::handleWriteEnableIRQ(uint32_t events) {
    // This is called from IRQ handler, so keep it fast!
    // events can be GPIO_IRQ_EDGE_RISE (rising edge) or GPIO_IRQ_EDGE_FALL (falling edge)
    
    // Handle PIO IRQ timer based on edge type
    if (events & GPIO_IRQ_EDGE_FALL) {
       startWritingProcedure();
    } else if (events & GPIO_IRQ_EDGE_RISE) {
       stopWritingProcedure();
    }
    
}
//..................................................................................................................................................

// Handle WRITE pin GPIO IRQ - detects flux transitions on WRITE pin
// Called from GPIO IRQ handler when WRITE pin changes state (rising or falling edge)
void FloppyEmulator::handleWriteIRQ(uint32_t events) {
    resetWritePWMTimer();
    gpio_put(14, 1);
    uint8_t currentWritePinState = gpio_get(writePin) ? 1 : 0;
    writePinChange();
    lastWritePinState = currentWritePinState;
    // Add bit to raw bit buffer (byte-by-byte collection for debugging)
    //addBitToRAWBuffer(1);
    gpio_put(14, 0);
    (void)events;  // Suppress unused variable warning
}
//..................................................................................................................................................
/*
// Handle PIO IRQ timer interrupt (called from IRQ handler)
// CRITICAL: This function runs in IRQ context - keep it FAST!
// This is called every 4μs to capture bits from WRITE pin during write operation
void FloppyEmulator::handleWriteIRQTimer() {
    // Check if write operation is still active (WRITE_EN should be LOW)
    // If WRITE_EN went HIGH, we should stop (but this is handled in GPIO IRQ handler)
    // For safety, check if timer is still active
    //hw_clear_bits(&sio_hw->gpio_clr, 1u << 2);  // Clear bit 2 (LOW)
    //gpio_put(2, 0);
    //stopWriteIRQTimer();
    gpio_put(2, !gpio_get(2));

//    stopWriteIRQTimer();
    //return;

//    sleep_us(1);

    // Read current state of WRITE pin
    uint8_t currentWritePinState = gpio_get(writePin) ? 1 : 0;
    
    //hw_clear_bits(&sio_hw->gpio_clr, 1u << 3);  // Clear bit 2 (LOW)
    //startWriteIRQTimer();
    //return;
    // Detect flux transition (pin state change) = bit "1"
    // No change = bit "0"
    //uint8_t currentBit = 0;
//    if (currentWritePinState != lastWritePinState) {
        // Flux transition detected - bit "1"
        //currentBit = 1;
//        writePinChange();
//        lastWritePinState = currentWritePinState;
//        resetWritePWMTimer();
        // Add bit to raw bit buffer (byte-by-byte collection for debugging)
//        addBitToRAWBuffer(1);
//    } else {
        hw_set_bits(&sio_hw->gpio_set, 1u << 3);  // Set bit 2 (HIGH)
        // No flux transition - bit "0"
        //currentBit = 0;
        writeIdle();
        lastWritePinState = currentWritePinState;
        // Add bit to raw bit buffer (byte-by-byte collection for debugging)
        //addBitToRAWBuffer(0);
        hw_clear_bits(&sio_hw->gpio_clr, 1u << 3);  // Clear bit 2 (LOW)
//    }
    
    // Write bit at current rotationPosition (rotationPosition is continuously updated by process() 
    // to track head position while reading, so it already points to the correct write location)
    //writeBit(currentBit);
    
    // After writing the bit, increment rotationPosition for the next bit
    // This ensures rotationPosition continues to track head position during write
    uint32_t gcrBitsPerTrack = APPLE_II_GCR_BYTES_PER_TRACK * 8;
    rotationPosition = (rotationPosition + 1) % gcrBitsPerTrack;
}
*/
//..................................................................................................................................................



// Handle flux transition (pin change) - bit "1"
// Based on AVR code write_pinchange()
void FloppyEmulator::writePinChange() {
    // Increment bit counter
    gpio_put(3, 1);
    writeBitsProcessed++;
    
    // Skip the 25th bit (bits are 1-indexed: 1st, 2nd, ..., 25th)
    if (writeBitsProcessed == 17) {
        // This is the 25th bit - skip it, don't process it
        gpio_put(3, 0);
        return;
    }
    
    // Shift left and add 1 (bit "1") - exactly like AVR code
    writeData = (writeData << 1) | 1;
    checkWriteData();
    gpio_put(3, 0);
}

// Handle no flux transition (no pin change) - bit "0"
// Based on AVR code write_idle()
void FloppyEmulator::writeIdle() {
    gpio_put(2, 1);
    // Increment bit counter
    writeBitsProcessed++;
    
    // Skip the 25th bit (bits are 1-indexed: 1st, 2nd, ..., 25th)
    if (writeBitsProcessed == 17) {
        // This is the 25th bit - skip it, don't process it
        gpio_put(2, 0);
        return;
    }
    
    // Shift left (bit "0") - exactly like AVR code
    writeData = (writeData << 1);
    checkWriteData();
    gpio_put(2, 0);
}
//=================================================================================================================
//=================================================================================================================
//=================================================================================================================
//=================================================================================================================
//=================================================================================================================
//=============== FINAL FUNCTIONS             =====================================
//=================================================================================================================
// Check if byte is complete and process sync/data
// Based on AVR code check_data()
void FloppyEmulator::checkWriteData() {
    // AVR code: static uint16_t bufsize; - we use writeBufferIndex instead
    if (!writeSynced && writeData == 0xD5) {
        // Found sync byte 0xD5! Start collecting data bytes (like AVR code)
        writeSynced = true;
        writeBufferIndex = 0;
        // Store 0xD5 in buffer immediately (like AVR code: goto push)
        if (writeBufferIndex < WRITE_BUFFER_SIZE) {
            writeBuffer[writeBufferIndex++] = writeData;
        }
        writeBitCount = 0;  // Reset bit count
        writeData = 0;      // Reset for next byte
        writeBitsProcessed=0;
        return;
    }

    if (writeSynced && writeBufferIndex < WRITE_BUFFER_SIZE) {
        // writeBitCount is already incremented in writePinChange/writeIdle
        // Just check if byte is complete now
        writeBitCount++;
        if (writeBitCount == 8) {
            // Byte complete - store in buffer (like AVR code: push label)
            writeBitCount = 0;
            writeBuffer[writeBufferIndex++] = writeData;
            //rotPosBuff[rotPosBuffIndex++] = rotationPosition;
            writeData = 0;  // Reset for next byte
        }
    }
}

// Write captured data back to disk image
// Based on AVR code write_back()
// AVR code checks: write_buffer[2] == 0xAD (data field) or 0x96 (address field)
void FloppyEmulator::writeBack() {
    
    // =========================================================================
    // UPDATE GCR CACHE with received data
    // =========================================================================
    // writeBuffer contains: D5 AA AD (prologue) + 343 bytes data + DE AA EB (epilogue)
    // We need to write this to the correct position in gcrTrackCache
    // 
    // GCR cache sector structure (416 bytes per sector):
    // - 22 sync bytes (0xFF)
    // - 12 sync pattern bytes  
    // - 3 bytes address prologue (D5 AA 96)
    // - 8 bytes address data
    // - 3 bytes address epilogue (DE AA EB)
    // - 5 sync bytes (0xFF)
    // - 3 bytes data prologue (D5 AA AD)  <- offset 53
    // - 343 bytes data
    // - 3 bytes data epilogue (DE AA EB)
    // - 14 sync bytes (0xFF)
    
    if (physicalSectorToWrite >= 0 && physicalSectorToWrite < 16 && writeBufferIndex >= 3) {
        // Check if this is a data field (D5 AA AD)
        if (writeBuffer[0] == 0xD5 && writeBuffer[1] == 0xAA && writeBuffer[2] == 0xAD) {
            // Calculate position in GCR cache
            // Each sector is 416 bytes, data field starts at offset 53
            const uint32_t SECTOR_SIZE = 416;
            const uint32_t DATA_FIELD_OFFSET = 53;  // 22 + 12 + 3 + 8 + 3 + 5 = 53
            
            uint32_t sectorStart = physicalSectorToWrite * SECTOR_SIZE;
            uint32_t dataFieldPos = sectorStart + DATA_FIELD_OFFSET;
            
            // Copy writeBuffer to GCR cache (data field: prologue + data + epilogue)
            // writeBuffer contains: D5 AA AD + 343 bytes + DE AA EB = up to 349 bytes
            uint32_t bytesToCopy = writeBufferIndex;
            if (bytesToCopy > 349) bytesToCopy = 349;  // Limit to data field size
            
            // Make sure we don't overflow the cache
            if (dataFieldPos + bytesToCopy <= APPLE_II_GCR_BYTES_PER_TRACK) {
                for (uint32_t i = 0; i < bytesToCopy; i++) {
                    gcrTrackCache[dataFieldPos + i] = writeBuffer[i];
                }
                gcrTrackCacheDirty = true;  // Mark cache as dirty - needs to be saved before track change
                printf("GCR CACHE UPDATED: Physical sector %d, logical sector %d, %u bytes at offset %u\r\n",
                       physicalSectorToWrite, currentSectortoWrite, bytesToCopy, dataFieldPos);
            } else {
                printf("ERROR: GCR cache write would overflow! pos=%u, bytes=%u\r\n", dataFieldPos, bytesToCopy);
            }
        }
    }
    
    writeSynced = false;
    writeBufferIndex = 0;
    writeBitCount = 0;
    writeData = 0;
    prologBuffer[0] = 0;
    prologBuffer[1] = 0;
    prologBuffer[2] = 0;
    prologBitCount = 0;
    printf("=== WRITE ENDED - Buffer cleared ===\r\n\r\n");
    return;

    // =========================================================================
    // DEBUG OUTPUT
    // =========================================================================
    
    // Debug: Print raw bit buffer contents (all bits collected byte-by-byte)
    /*
    printf("\r\n=== RAW BIT BUFFER DEBUG (size: %u bytes) ===\r\n", rawBitBufferIndex);
    if (rawBitBufferIndex > 0) {
        // Print all bytes in raw bit buffer
        uint16_t printSize = rawBitBufferIndex < 256 ? rawBitBufferIndex : 256;
        for (uint16_t i = 0; i < printSize; i++) {
            if (i % 16 == 0) {
                printf("\r\n%04X: ", i);
            }
            printf("%02X ", rawBitBuffer[i]);
        }
        if (rawBitBufferIndex > 256) {
            printf("\r\n... (truncated, total %u bytes)", rawBitBufferIndex);
            // Print last 32 bytes if buffer is larger
            printf("\r\nLast 32 bytes:\r\n");
            uint16_t start = rawBitBufferIndex > 32 ? rawBitBufferIndex - 32 : 0;
            for (uint16_t i = start; i < rawBitBufferIndex; i++) {
                if ((i - start) % 16 == 0) {
                    printf("%04X: ", i);
                }
                printf("%02X ", rawBitBuffer[i]);
            }
        }
        printf("\r\n");
    }
    */


    // Debug: Print write buffer contents - includes prologue (D5 AA AD at indices 0, 1, 2)
    printf("\r\n=== WRITE BUFFER DEBUG (size: %u bytes, synced: %s) ===\r\n", 
           writeBufferIndex, writeSynced ? "YES" : "NO");
    if (writeBufferIndex > 0) {
        // Print all bytes in buffer starting from index 0 (includes prologue)
        uint16_t printSize = writeBufferIndex < 256 ? writeBufferIndex : 256;
        for (uint16_t i = 0; i < printSize; i++) {
            if (i % 16 == 0) {
                printf("\r\n%04X: ", i);
            }
            printf("%02X ", writeBuffer[i]);
        }
        if (writeBufferIndex > 256) {
            printf("\r\n... (truncated, total %u bytes)", writeBufferIndex);
            // Print last 32 bytes if buffer is larger
            printf("\r\nLast 32 bytes:\r\n");
            uint16_t start = writeBufferIndex > 32 ? writeBufferIndex - 32 : 0;
            for (uint16_t i = start; i < writeBufferIndex; i++) {
                if ((i - start) % 16 == 0) {
                    printf("%04X: ", i);
                }
                printf("%02X ", writeBuffer[i]);
            }
        }
        printf("\r\n");
        
        // Print statistics
        printf("Statistics: Total bytes=%u, Bits collected=%u\r\n", writeBufferIndex, writeBitCount);
        
        // Check field type like AVR code (write_buffer[2])
        bool isDataField = false;
        bool isAddressField = false;
        if (writeBufferIndex > 2) {
            if (writeBuffer[2] == 0xAD) {
                printf("Field type: DATA (0xAD detected at index 2)\r\n");
                isDataField = true;
            } else if (writeBuffer[2] == 0x96) {
                printf("Field type: ADDRESS (0x96 detected at index 2) - FORMATTING\r\n");
                isAddressField = true;
                
                // Decode sector from AA-encoded address field (like AVR code)
                // writeBuffer[7] and writeBuffer[8] contain AA-encoded sector number
                // Format: (value >> 1) | 0xAA and value | 0xAA
                // Decode: ((buf[7] & 0x55) << 1) | (buf[8] & 0x55)
                if (writeBufferIndex > 8) {
                    uint8_t physicalSector = ((writeBuffer[7] & 0x55) << 1) | (writeBuffer[8] & 0x55);
                    printf("Decoded physical sector from address field: %d\r\n", physicalSector);
                    // Convert physical sector to logical sector using SECTOR_SCRAMBLE
                    if (physicalSector < 16) {
                        currentSectortoWrite = SECTOR_SCRAMBLE[physicalSector];
                        printf("Logical sector (after SECTOR_SCRAMBLE): %d\r\n", currentSectortoWrite);
                    } else {
                        currentSectortoWrite = physicalSector;
                    }
                }
            } else {
                printf("Field type: UNKNOWN (0x%02X at index 2, expected 0xAD or 0x96)\r\n", writeBuffer[2]);
            }
        }
        
        // Decode GCR data field if it's a data field
        if (isDataField && writeBufferIndex >= 3) {
            // Data field starts after prologue (D5 AA AD) at index 3
            // We need at least 343 bytes (86 + 256 + 1) for full decode
            uint8_t decodedData[256];
            uint16_t decodedLength = 0;
            
            // Calculate available GCR data length
            // Prologue is at indices 0, 1, 2 (D5 AA AD) - skip these, start from index 3
            // If epilogue exists at the end (DE AA EB), it should be excluded
            // User changed to -6, which might account for additional bytes
            uint16_t gcrDataLength = writeBufferIndex - 6;
            
            // Sector was already detected at write start (in handleWriteEnableIRQ)
            // by searching backwards from DMA position for address field (D5 AA 96)
            printf("current_sector_to_write: %d (detected at write start, DMA pos was %u)\r\n", 
                   currentSectortoWrite, dmaPositionAtWriteStart);

            // Debug: show what we're passing to decoder
            printf("DEBUG: Passing %u bytes to decoder (starting from writeBuffer[3], writeBufferIndex=%u)\r\n", 
                   gcrDataLength, writeBufferIndex);
            if (gcrDataLength > 0 && gcrDataLength <= 10) {
                printf("DEBUG: First few GCR bytes: ");
                for (uint16_t i = 0; i < gcrDataLength && i < 10; i++) {
                    printf("%02X ", writeBuffer[3 + i]);
                }
                printf("\r\n");
            }
            
            if (gcrDataLength >= 343) {
                decodeNICDataField(&writeBuffer[3], gcrDataLength, decodedData, &decodedLength);
                
                if (decodedLength > 0) {
                    printf("\r\n=== DECODED DATA FIELD (%u bytes) ===\r\n", decodedLength);
                    uint16_t printSize = decodedLength < 256 ? decodedLength : 256;
                    for (uint16_t i = 0; i < printSize; i++) {
                        if (i % 16 == 0) {
                            printf("\r\n%04X: ", i);
                        }
                        printf("%02X ", decodedData[i]);
                    }
                    printf("\r\n=== END DECODED DATA ===\r\n");
                } else {
                    printf("WARNING: Failed to decode data field (gcrDataLength=%u, need >=343)\r\n", gcrDataLength);
                }
            } else {
                printf("WARNING: Not enough GCR data for decoding (have %u bytes, need >=343)\r\n", gcrDataLength);
            }
        }
    } else {
        printf("ERROR: Buffer is EMPTY - no data received!\r\n");
        printf("Statistics: Bits collected=%u, writeData=0x%02X\r\n", writeBitCount, writeData);
    }
    printf("=== END WRITE BUFFER DEBUG ===\r\n\r\n");
    

//    for (int i=0;i<rotPosBuffIndex;i++) {
//        printf("%d ", rotPosBuff[i]);
//        if (i % 16 == 0) {
//            printf("\r\n");
//        }
//    }
//}
    printf("\r\n"); 
    // TODO: Parse sector data and write to disk image (like AVR code does)
    // AVR code checks write_buffer[2] == 0xAD for data field
    // AVR code checks write_buffer[2] == 0x96 for address field
    
    // Reset write state
    writeSynced = false;
    writeBufferIndex = 0;
    writeBitCount = 0;
    writeData = 0;
    prologBuffer[0] = 0;
    prologBuffer[1] = 0;
    prologBuffer[2] = 0;
    prologBitCount = 0;
    printf("=== WRITE ENDED - Buffer cleared ===\r\n\r\n");
}

// Decode NIC-encoded data field to raw sector data
// Based on reverse of encoding logic in updateGCRTrackCache()
// Encoding process:
//   1. First 86 bytes: x = FLIP_BIT1[src[i] & 3] | FLIP_BIT2[src[i + 86] & 3] | FLIP_BIT3[src[i + 172] & 3]
//      encoded = NIC_ENCODE_TABLE[(x ^ ox) & 0x3F], ox = x
//   2. All 256 bytes: x = (src[i] >> 2)
//      encoded = NIC_ENCODE_TABLE[(x ^ ox) & 0x3F], ox = x
//   3. Last byte: encoded = NIC_ENCODE_TABLE[ox & 0x3F]
void FloppyEmulator::decodeNICDataField(const uint8_t* gcrData, uint16_t gcrLength, uint8_t* decodedData, uint16_t* decodedLength) {
    ensureNICDecodeTableInitialized();
    
    // Need at least: 86 (first encoding) + 256 (standard encoding) + 1 (last byte) = 343 bytes
    if (gcrLength < 343) {
        *decodedLength = 0;
        return;
    }
    
    uint8_t ox = 0;  // Previous XOR state (starts at 0)
    uint8_t decoded6bit[343];  // Decoded 6-bit values
    
    // Step 1: Decode all GCR bytes to 6-bit values (XOR decode)
    // Encoding: encoded = NIC_ENCODE_TABLE[(x ^ ox) & 0x3F], then ox = x
    // Decoding: raw = NIC_DECODE_TABLE[encoded] = (x ^ prev_ox), then x = raw ^ prev_ox
    //           ox must be updated to DECODED value (x), not raw value!
    for (uint16_t i = 0; i < 343 && i < gcrLength; i++) {
        uint8_t encoded = gcrData[i];
        uint8_t raw = NIC_DECODE_TABLE[encoded];  // raw = (original_x ^ previous_ox)
        if (raw == 0xFF) {
            // Invalid GCR code - print diagnostic info
            printf("ERROR: Invalid GCR code 0x%02X at position %u (gcrData[%u])\r\n", encoded, i, i);
            *decodedLength = 0;
            return;
        }
        decoded6bit[i] = raw ^ ox;  // XOR decode: x = (x ^ prev_ox) ^ prev_ox = x
        ox = decoded6bit[i];  // CRITICAL: ox must be the DECODED value for next iteration!
    }
    
    // Step 2: Decode the 256 bytes from standard encoding (positions 86 to 86+256)
    // Each decoded6bit[i] (for i=86 to 86+256) represents (src[i-86] >> 2)
    // So: src[i-86] = (decoded6bit[i] << 2) | lower_2_bits
    // The lower 2 bits come from the first 86 bytes (FlipBit encoding)
    
    // Initialize decoded data with upper 6 bits
    *decodedLength = 256;
    for (uint16_t i = 0; i < 256; i++) {
        if (86 + i < 343) {
            uint8_t x = decoded6bit[86 + i];  // This is (src[i] >> 2)
            decodedData[i] = (x << 2);  // Upper 6 bits (lower 2 bits = 0 for now)
        } else {
            decodedData[i] = 0;
        }
    }
    
    // Step 3: Decode lower 2 bits from first 86 bytes using reverse FlipBit mapping
    // decoded6bit[0..85] contains: FLIP_BIT1[src[i] & 3] | FLIP_BIT2[src[i + 86] & 3] | FLIP_BIT3[src[i + 172] & 3]
    // We need to extract src[i] & 3, src[i+86] & 3, src[i+172] & 3 from this
    
    // Decode lower 2 bits for bytes 0-85, 86-171, 172-257
    for (uint16_t i = 0; i < 86 && i < 343; i++) {
        uint8_t x = decoded6bit[i];  // FLIP_BIT1[a] | FLIP_BIT2[b] | FLIP_BIT3[c]
        
        // Extract individual components using reverse lookup
        // FLIP_BIT1: {0,2,1,3} -> reverse: 0->0, 2->1, 1->2, 3->3
        uint8_t a = 0;
        uint8_t x_low = x & 0x03;
        if (x_low == 0) a = 0;
        else if (x_low == 2) a = 1;
        else if (x_low == 1) a = 2;
        else if (x_low == 3) a = 3;
        
        // FLIP_BIT2: {0,8,4,12} -> reverse: 0->0, 8->1, 4->2, 12->3
        uint8_t b = 0;
        uint8_t x_mid = x & 0x0C;
        if (x_mid == 0) b = 0;
        else if (x_mid == 8) b = 1;
        else if (x_mid == 4) b = 2;
        else if (x_mid == 12) b = 3;
        
        // FLIP_BIT3: {0,32,16,48} -> reverse: 0->0, 32->1, 16->2, 48->3
        uint8_t c = 0;
        uint8_t x_high = x & 0x30;
        if (x_high == 0) c = 0;
        else if (x_high == 32) c = 1;
        else if (x_high == 16) c = 2;
        else if (x_high == 48) c = 3;
        
        // Apply lower 2 bits to decoded data
        if (i < 256) {
            decodedData[i] = (decodedData[i] & 0xFC) | a;
        }
        if (i + 86 < 256) {
            decodedData[i + 86] = (decodedData[i + 86] & 0xFC) | b;
        }
        if (i + 172 < 256) {
            decodedData[i + 172] = (decodedData[i + 172] & 0xFC) | c;
        }
    }
}