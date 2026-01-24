#ifndef FLOPPY_EMULATOR_H
#define FLOPPY_EMULATOR_H

#include <cstdint>
#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"
#include "pico/time.h"
#include "hardware/timer.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "floppy_bit_output.pio.h"
#include "floppy_irq_timer.pio.h"

// Apple II Floppy Disk Constants
#define APPLE_II_TRACKS           35      // 0-34 tracks
#define APPLE_II_SECTORS_PER_TRACK 16      // Sectors per track
#define APPLE_II_BYTES_PER_SECTOR  256     // Bytes per sector (raw data)
#define APPLE_II_STEPS_PER_TRACK   2       // Full steps per track (4-phase stepper: PH0->PH1->PH2->PH3 = 1 track)
#define APPLE_II_BIT_PERIOD_US     4       // 4 microseconds per bit period (standard Apple II Disk II: 1 bit every 4μs = 125 kbps)
#define APPLE_II_PULSE_WIDTH_US    1       // 1 microsecond pulse width for bit "1" (flux transition)
#define APPLE_II_ROTATION_TIME_MS  200     // One full rotation = 200ms (300 RPM)
#define APPLE_II_ROTATION_TIME_US  (APPLE_II_ROTATION_TIME_MS * 1000)  // 200,000 microseconds
#define APPLE_II_BITS_PER_ROTATION (APPLE_II_ROTATION_TIME_US / APPLE_II_BIT_PERIOD_US)  // 50,000 bits per rotation
#define APPLE_II_DISK_SIZE         (APPLE_II_TRACKS * APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR)
#define APPLE_II_BYTES_PER_TRACK   (APPLE_II_SECTORS_PER_TRACK * APPLE_II_BYTES_PER_SECTOR)  // 4096 bytes (raw data)
#define APPLE_II_BITS_PER_TRACK    (APPLE_II_BYTES_PER_TRACK * 8)  // 32768 bits (raw)

// Apple II Disk II track format:
// Each track has 16 sectors, each sector consists of:
// - Address field: prologue (0xD5 0xAA 0x96) + volume/track/sector/checksum + epilogue (0xDE 0xAA 0xEB)
// - Data field: sync bytes (0xFF) + prologue + 256 bytes data + epilogue + gap
// Total raw bytes per track: 6656 bytes (including address fields, sync, gaps)
// Each sector: 416 bytes (not 512 - padding is not part of track format)
// Total: 16 sectors * 416 bytes = 6656 bytes per track
// After GCR encoding: 6656 bytes (already GCR encoded in NIC format)
#define APPLE_II_RAW_TRACK_BYTES   6656    // Raw bytes per track (416 bytes per sector * 16 sectors)
#define APPLE_II_GCR_BYTES_PER_TRACK  APPLE_II_RAW_TRACK_BYTES  // Already GCR encoded in NIC format

// GCR Encoding constants
#define GCR_DATA_BITS              5       // 5 data bits
#define GCR_ENCODED_BITS           6       // 6 encoded bits
#define GCR_TABLE_SIZE             32      // 2^5 = 32 possible 5-bit values

// Stepper motor phases
typedef enum {
    STEPPER_PHASE_0 = 0,
    STEPPER_PHASE_1 = 1,
    STEPPER_PHASE_2 = 2,
    STEPPER_PHASE_3 = 3
} StepperPhase;

// Stepper motor direction
typedef enum {
    STEPPER_DIR_INWARD = -1,   // Move toward track 0
    STEPPER_DIR_OUTWARD = 1    // Move away from track 0
} StepperDirection;

// Forward declaration
class SDCardManager;

class FloppyEmulator {
private:
    // RAW disk image storage (linear array)
    uint8_t diskImage[APPLE_II_DISK_SIZE];
    
    // Stepper motor control
    uint8_t stepperPhasePins[4];  // GPIO pins for PH0, PH1, PH2, PH3
    int currentTrack;               // Current logical track position (0-34)
    int currentSector;              // Current sector position (0-15)
    int currentSectortoWrite;              // Logical sector to write (0-15) - for disk image
    int physicalSectorToWrite;             // Physical sector (0-15) - for GCR cache position
    uint32_t dmaPositionAtWriteStart;       // DMA position when write started (for sector detection)

    //int LastTrackLoaded;            // Last track loaded from disk image
    int currentStep;                // Current step position (0-3 per track)
    int physicalTrack;              // Physical track position (0-69, matches ATMegaX DII_ph_track)
    StepperPhase currentPhase;      // Current stepper phase
    StepperPhase lastPhaseOffset;   // Last phase offset for change detection (matches ATMegaX old_ofs)
    
    // Read/Write control
    uint8_t readPin;                // GPIO pin for READ data output (to controller)
    uint8_t writePin;               // GPIO pin for WRITE data input (from controller)
    uint8_t writeEnablePin;         // GPIO pin for WRITE_ENABLE (from controller)
    uint8_t driveSelPin;            // GPIO pin for DRIVE_SEL (from controller - selects this drive)
    
    // Timing and synchronization
    absolute_time_t lastBitTime;    // Last bit time for timing
    uint32_t rotationPosition;      // Current rotation position (0-65535)
    bool indexPulse;                // Index pulse state
    repeating_timer_t bitTimer;     // Hardware timer for precise bit timing
    bool timerActive;                // Timer active flag
    uint8_t bitPeriodPhase;         // Current phase in bit period (0-3, wraps at 4 for 4μs period)
    uint8_t currentBitValue;        // Current bit value being transmitted (for pulse generation)
    
    // Write buffer and state
    static const uint32_t WRITE_BUFFER_SIZE = 350;  // Write buffer size (matches AVR code)
    uint8_t writeBuffer[WRITE_BUFFER_SIZE];         // Buffer for captured write data
    uint8_t writeData;                              // Current byte being assembled (shift register)
    uint8_t writeBitCount;                          // Number of bits collected in current byte (0-8)
    uint16_t writeBufferIndex;                      // Index in write buffer (0-WRITE_BUFFER_SIZE)
    bool writeSynced;                               // True when prologue D5 AA 96 detected
    uint8_t lastWritePinState;                      // Last state of WRITE pin (for flux transition detection)
    uint8_t prologBuffer[3];                        // 3-byte buffer for prolog detection (shifted on each bit)
    uint8_t prologBitCount;                         // Number of bits collected in prolog buffer (0-24)
    uint16_t writeBitsProcessed;                    // Number of bits processed so far (for skipping 25th bit)
    
    // Temporary buffer for collecting all bits byte-by-byte (for debugging)
    static const uint32_t RAW_BIT_BUFFER_SIZE = 500;  // Temporary buffer size for raw bits
    uint8_t rawBitBuffer[RAW_BIT_BUFFER_SIZE];        // Buffer for raw bits collected byte-by-byte
    uint8_t rawBitData;                               // Current byte being assembled from raw bits
    uint8_t rawBitCount;                              // Number of bits collected in current raw byte (0-8)
    uint16_t rawBitBufferIndex;                       // Index in raw bit buffer (0-RAW_BIT_BUFFER_SIZE)
    // GCR encoding/decoding tables
    uint8_t gcrEncodeTable[GCR_TABLE_SIZE];
    uint8_t gcrDecodeTable[256];
    
    // GCR track cache - pre-encoded GCR data for current track
    // This allows fast bit access in interrupt handler without expensive GCR encoding
    uint8_t gcrTrackCache[APPLE_II_GCR_BYTES_PER_TRACK];
    int gcrTrackCacheTrack;         // Track number for which cache is valid (-1 = invalid)
    uint32_t gcrTrackCacheBits;     // Number of GCR bits in cache (48 bits per 5-byte group)
    bool gcrTrackCacheDirty;        // True if GCR cache has been modified (needs to be saved before track change)
    
    // Track change detection for delayed cache loading
    int pendingTrack;                // Track that needs to be loaded (when stable)
    absolute_time_t trackChangeTime; // Time when track last changed
    static const uint32_t TRACK_STABLE_TIMEOUT_MS = 200;  // Wait 200ms before loading track from SD
    
    // SD card and file management
    SDCardManager* sdCardManager;    // Pointer to SD card manager (for saving tracks to file)
    char currentFileName[64];        // Current disk image filename (for saving tracks)
    
    // PIO/DMA for continuous bit output
    PIO pio;                        // PIO instance (pio0 or pio1)
    uint pioSm;                     // PIO state machine index
    uint pioOffset;                 // PIO program offset
    int dmaChannel;                 // DMA channel for transferring data to PIO FIFO
    bool pioDmaActive;              // PIO/DMA active flag
    dma_channel_config dmaConfig;   // Cached DMA config for fast IRQ restart
    
    // PIO IRQ timer for write bit capture (4μs period)
    PIO writeIrqTimerPio;           // PIO instance for IRQ timer
    uint writeIrqTimerSm;           // PIO state machine for IRQ timer
    uint writeIrqTimerOffset;       // PIO program offset
    bool writeIrqTimerActive;       // IRQ timer active flag
    
    int lastTimeWriteCheck = get_absolute_time(); // Last time GCR cache was saved to disk image
    int lastTimeChangeTrackCheck = get_absolute_time(); // Last time GCR cache was saved to disk image
    // Internal methods
    void initializeGCRTables();
    uint8_t encodeGCR(uint8_t data);
    uint8_t decodeGCR(uint8_t gcr);
    void detectStepperPhaseChange();  // Detect phase changes from controller
    void updateStepperPosition();     // Update position based on phase changes
    void updateStepperPositionWithPhases(StepperPhase oldPhase, StepperPhase newPhase);  // Update position with explicit phases
    uint32_t calculateTrackOffset(int track, int sector);
    void updateRotationPosition();
    uint32_t getCurrentBitPosition(); // Get current bit position on track
//    uint8_t getGCRBitAtPosition(uint32_t rawBitPosition); // Get GCR-encoded bit at raw bit position
    void updateGCRTrackCache();  // Update GCR cache for current track (called when track changes)
    void initPIO_DMA();          // Initialize PIO and DMA for continuous bit output
    void startPIO_DMA();         // Start PIO/DMA streaming from cache buffer
    void stopPIO_DMA();          // Stop PIO/DMA streaming
    
public:
    void saveGCRCacheToDiskImage();  // Save GCR cache back to disk image (called before track change if dirty)
    void handleDMAIRQ();         // Handle DMA IRQ for fast restart (called from IRQ handler)
    // GPIO IRQ handler (public for IRQ handler access)
    void handleWriteEnableIRQ(uint32_t events);  // Handle WRITE_EN GPIO IRQ (called from IRQ handler)
    void handleWriteIRQ(uint32_t events);  // Handle WRITE pin GPIO IRQ (called from IRQ handler)
    // Timer interrupt methods (public for callback access)
    void processBitTimer();      // Process bit timer interrupt (called from timer callback)
    void startBitTimer();        // Start hardware timer for bit-level timing
    void stopBitTimer();         // Stop hardware timer
    

    bool getGCRTrackCacheDirty(); // Get GCR track cache dirty flag

    void initWritePWMTimer();    // Initialize PWM timer for write bit capture
    void startWritePWMTimer();   // Start PWM timer
    void stopWritePWMTimer();    // Stop PWM timer
    void resetWritePWMTimer();
    bool checkPWMOverflow();     // Check if PWM timer overflowed (like TCD0.INTFLAGS & TC0_OVFIF_bm)


    void handleWriteIRQTimer();  // Handle PIO IRQ timer interrupt (called from IRQ handler)
    void initWriting();          // Initialize write state (called when write starts)
    void endWriting();           // Finalize write and save sector (called when write ends)
    void writePinChange();       // Handle flux transition (bit "1") - shift left and add 1
    void writeIdle();            // Handle no flux transition (bit "0") - shift left
    void checkWriteData();       // Check if byte is complete and process sync/data (based on AVR check_data())
    void writeBack();            // Write captured data back to disk image
    void startWritingProcedure(); // Start writing procedure (called when write starts)
    void stopWritingProcedure(); // Stop writing procedure (called when write ends)
    void addBitToRAWBuffer(uint8_t bit); // Add bit to RAW buffer
    
    bool floppy_write_in();
    void decodeNICDataField(const uint8_t* gcrData, uint16_t gcrLength, uint8_t* decodedData, uint16_t* decodedLength);  // Decode NIC-encoded data field
    // Constructor
    FloppyEmulator(
        uint8_t ph0, uint8_t ph1, uint8_t ph2, uint8_t ph3,
        uint8_t read, uint8_t write, uint8_t writeEnable, uint8_t driveSel
    );
    
    // Destructor
    ~FloppyEmulator();
    
    // Initialization
    void init();
    
    // Stepper motor monitoring (reactive to controller signals)
    void processStepperMotor();     // Process stepper motor phase changes from controller
    int getCurrentTrack() const;
    int getCurrentSector() const;
    bool isAtTrack0() const;
    void setCurrentTrack(int track);  // Set current track (for debugging/testing)
    void setCurrentSector(int sector);  // Set current sector (for debugging/testing)
    
    // GCR encoding/decoding
    void encodeSectorGCR(const uint8_t* data, uint8_t* gcr, int length);
    void decodeSectorGCR(const uint8_t* gcr, uint8_t* data, int length);
    
    // Read/Write operations (reactive to controller)
    bool readSector(int track, int sector, uint8_t* buffer);  // For CLI/debugging
    bool writeSector(int track, int sector, const uint8_t* buffer);  // For CLI/debugging
    // Get GCR cache contents for a sector (416 bytes per sector in cache)
    // Uses current track from cache - each sector occupies 416 bytes
    bool getGCRSectorFromCache(int sector, uint8_t* buffer, uint32_t maxLen, uint32_t* outLen);
    
    // Disk image management
    void loadDiskImage(const uint8_t* image, uint32_t size);
    void clearDiskImage();
    uint8_t* getDiskImage();
    uint32_t getDiskImageSize() const;
    
    // Timing and synchronization
    void updateTiming();            // Update timing state (call periodically in main loop)
    void syncToIndex();             // Synchronize to index pulse
    uint32_t getBitPeriodUs() const;
    
    // Main processing loop (call this in main loop to react to controller)
    void process();                 // Process all controller signals and respond
    
    // Status
    bool isDriveSelected() const;  // Check if this drive is selected by controller
    bool isWriteEnabled() const;
    
    // SD card and file management
    void setSDCardManager(SDCardManager* sdCard);  // Set SD card manager for saving tracks
    void setCurrentFileName(const char* filename);  // Set current disk image filename
};

#endif // FLOPPY_EMULATOR_H

