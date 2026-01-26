#ifndef SD_CARD_MANAGER_H
#define SD_CARD_MANAGER_H

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "PinConfig.h"
#include "FAT32.h"
#include "FloppyEmulator.h"
#include <stdint.h>
#include <stdbool.h>

// SD Card commands
#define SD_CMD0            0
#define SD_CMD8            8
#define SD_CMD16           16
#define SD_CMD17           17  // Read single block
#define SD_CMD24           24  // Write single block
#define SD_CMD55           55
#define SD_CMD58           58
#define SD_ACMD41          41

// SD Card response types
#define SD_R1              1
#define SD_R1_IDLE         0x01
#define SD_R1_ILLEGAL_CMD  0x04

// SD Card block size
#define SD_BLOCK_SIZE      512

class SDCardManager {
private:
    spi_inst_t* spiInstance;
    uint8_t csPin;
    uint8_t mosiPin;
    uint8_t misoPin;
    uint8_t sckPin;
    uint8_t detectPin;  // Card detect pin (optional, 0xFF if not used)
    bool initialized;
    bool cardPresent;  // Last known card presence state
    uint32_t currentBaudrate;  // Current SPI baudrate
    
    // FAT32 filesystem
    FAT32* fat32;
    FAT32_Error lastFAT32Error;  // Store last FAT32 error (preserved even if FAT32 deleted)
    
    // Internal methods
    void selectCard();
    void deselectCard();
    uint8_t sendCommand(uint8_t cmd, uint32_t arg);
    uint8_t sendACommand(uint8_t cmd, uint32_t arg);
    void waitForReady();
    bool waitForResponse(uint8_t* response, uint32_t timeout);
    
public:
    // Constructor
    SDCardManager(spi_inst_t* spi, uint8_t cs, uint8_t mosi, uint8_t miso, uint8_t sck, uint8_t detect = 0xFF);
    
    // Initialization
    bool init(uint32_t maxBaudrate = 20000000, bool verbose = false);  // maxBaudrate in Hz (default 20MHz)
    void deinit();
    bool isInitialized() const;
    
    // Card detection
    bool isCardPresent() const;
    bool checkCardPresence();  // Check detect pin and update state
    
    // Read/Write operations
    bool readBlock(uint32_t blockAddress, uint8_t* buffer);
    bool writeBlock(uint32_t blockAddress, const uint8_t* buffer);
    
    // File operations (FAT32)
    bool readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead);
    bool fileExists(const char* filename);
    bool listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount);
    
    // Disk image loading
    bool loadDiskImage(const char* filename, uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead);
    bool saveTrackToFile(const char* filename, int track, const uint8_t* trackData, uint32_t trackSize);
    bool readTrackFromFile(const char* filename, int track, uint8_t* trackData, uint32_t trackSize);
    
    // FAT32 access
    FAT32* getFAT32() const;
    FAT32_Error getLastFAT32Error() const { return lastFAT32Error; }
    
    // Speed testing
    uint32_t testMaxReadSpeed(uint32_t testBlocks = 5, bool verbose = false);
    
    // Get current SPI speed in Hz
    uint32_t getCurrentBaudrate() const { return currentBaudrate; }
};

#endif // SD_CARD_MANAGER_H

