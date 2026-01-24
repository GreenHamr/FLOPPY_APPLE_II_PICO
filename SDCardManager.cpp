#include "SDCardManager.h"
#include "hardware/gpio.h"
#include <cstdint>
#include <stdio.h>
#include <string.h>

// Constructor
SDCardManager::SDCardManager(spi_inst_t* spi, uint8_t cs, uint8_t mosi, uint8_t miso, uint8_t sck, uint8_t detect) {
    spiInstance = spi;
    csPin = cs;
    mosiPin = mosi;
    misoPin = miso;
    sckPin = sck;
    detectPin = detect;
    initialized = false;
    cardPresent = false;
    fat32 = nullptr;
    currentBaudrate = 0;
    
    // Initialize card detect pin if provided
    if (detectPin != 0xFF) {
        gpio_init(detectPin);
        gpio_set_dir(detectPin, GPIO_IN);
        gpio_pull_up(detectPin);  // Pull-up resistor (active LOW means card present)
        cardPresent = !gpio_get(detectPin);  // Inverted: LOW = card present
    }
}

// Initialize SD card
bool SDCardManager::init(uint32_t maxBaudrate, bool verbose) {
    if (verbose) {
        printf("SD Init: Starting initialization...\r\n");
    }
    
    // Initialize SPI with low speed first
    spi_init(spiInstance, 100000);  // Start with 100kHz
    gpio_set_function(mosiPin, GPIO_FUNC_SPI);
    gpio_set_function(misoPin, GPIO_FUNC_SPI);
    gpio_set_function(sckPin, GPIO_FUNC_SPI);
    
    // Initialize CS pin
    gpio_init(csPin);
    gpio_set_dir(csPin, GPIO_OUT);
    gpio_put(csPin, 1);  // Deselect (high = inactive)
    sleep_ms(100);  // Wait longer for card to stabilize
    
    if (verbose) {
        printf("SD Init: Sending 80+ clock cycles...\r\n");
    }
    
    // Send 80+ clock cycles to initialize (card needs this to wake up)
    deselectCard();
    uint8_t dummy = 0xFF;
    for (int i = 0; i < 20; i++) {  // Increased to 20 bytes (160 clock cycles)
        spi_write_blocking(spiInstance, &dummy, 1);
    }
    sleep_ms(100);  // Longer delay
    
    if (verbose) {
        printf("SD Init: Sending CMD0 (reset)...\r\n");
    }
    
    // Send CMD0 to reset card (go to idle state)
    selectCard();
    uint8_t response = sendCommand(SD_CMD0, 0);
    deselectCard();
    sleep_ms(50);  // Longer delay after CMD0
    
    if (verbose) {
        printf("SD Init: CMD0 response = 0x%02X\r\n", response);
    }
    
    // Check if card responded with idle state
    // Response should be 0x01 (idle) or 0xFF (no response)
    if (response == 0xFF) {
        if (verbose) {
            printf("SD Init: ERROR - No response to CMD0 (card not detected)\r\n");
        }
        return false;
    }
    
    if ((response & 0xFE) != 0) {  // Any error bits set (except bit 0 = idle)
        if (verbose) {
            printf("SD Init: ERROR - CMD0 failed with error bits: 0x%02X\r\n", response);
        }
        return false;
    }
    
    // Try CMD8 (only for SDHC/SDXC cards, version 2.0+)
    // This command may fail for older cards, which is OK
    bool isSDHC = false;
    
    if (verbose) {
        printf("SD Init: Sending CMD8 (check voltage)...\r\n");
    }
    
    selectCard();
    response = sendCommand(SD_CMD8, 0x1AA);
    
    if (verbose) {
        printf("SD Init: CMD8 response = 0x%02X\r\n", response);
    }
    
    if (response == SD_R1_IDLE) {
        // Card responded, read R7 response (4 bytes)
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) {
            spi_read_blocking(spiInstance, 0xFF, &r7[i], 1);
        }
        
        if (verbose) {
            printf("SD Init: CMD8 R7 = %02X %02X %02X %02X\r\n", r7[0], r7[1], r7[2], r7[3]);
        }
        
        // Check if voltage and check pattern match
        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            isSDHC = true;  // SDHC/SDXC card
            if (verbose) {
                printf("SD Init: Detected SDHC/SDXC card\r\n");
            }
        }
    } else {
        if (verbose) {
            printf("SD Init: CMD8 not supported (old card or MMC)\r\n");
        }
    }
    deselectCard();
    sleep_ms(50);
    
    // Check if CMD8 failed with illegal command - might be MMC card
    bool isMMC = (response == 0x09);  // Illegal command response to CMD8
    
    if (isMMC && verbose) {
        printf("SD Init: Detected possible MMC card (CMD8 illegal)\r\n");
    }
    
    // Try ACMD41 to initialize card (with HCS bit for SDHC)
    uint32_t acmd41Arg = isSDHC ? 0x40000000 : 0x00000000;  // HCS bit for SDHC
    
    if (verbose) {
        printf("SD Init: Sending ACMD41 (initialize, arg=0x%08X)...\r\n", acmd41Arg);
    }
    
    // For MMC cards, try CMD1 instead of ACMD41
    if (isMMC) {
        if (verbose) {
            printf("SD Init: Trying MMC initialization with CMD1...\r\n");
        }
        selectCard();
        for (int i = 0; i < 200; i++) {
            response = sendCommand(1, 0x40FF8000);  // CMD1 for MMC (arg: voltage + busy)
            if (response == 0) {
                if (verbose && i > 0) {
                    printf("SD Init: CMD1 (MMC) succeeded after %d attempts\r\n", i + 1);
                }
                break;
            }
            if (i < 10) {
                sleep_ms(1);
            } else {
                sleep_ms(10);
            }
            if (verbose && (i == 0 || i == 9 || i == 49 || i == 99 || i == 199)) {
                printf("SD Init: CMD1 attempt %d, response = 0x%02X\r\n", i + 1, response);
            }
        }
        deselectCard();
        
        if (response == 0) {
            // MMC card initialized, skip to block size
            goto set_block_size;
        } else {
            if (verbose) {
                printf("SD Init: CMD1 failed, trying ACMD41 anyway...\r\n");
            }
        }
    }
    
    // Try ACMD41 for SD cards
    selectCard();
    for (int i = 0; i < 200; i++) {  // Increased retries
        // First send CMD55 (app command prefix)
        uint8_t cmd55Response = sendCommand(SD_CMD55, 0);
        if (verbose && i == 0) {
            printf("SD Init: CMD55 response = 0x%02X\r\n", cmd55Response);
        }
        
        if (cmd55Response != 0x01) {
            // CMD55 failed - card might not support ACMD commands
            if (verbose) {
                printf("SD Init: CMD55 failed, response = 0x%02X\r\n", cmd55Response);
            }
            deselectCard();
            break;
        }
        
        // Now send ACMD41
        response = sendCommand(SD_ACMD41, acmd41Arg);
        if (response == 0) {
            if (verbose && i > 0) {
                printf("SD Init: ACMD41 succeeded after %d attempts\r\n", i + 1);
            }
            break;  // Card is ready
        }
        if (i < 10) {
            sleep_ms(1);  // Shorter delay for first attempts
        } else {
            sleep_ms(10);  // Longer delay after initial attempts
        }
        if (verbose && (i == 0 || i == 9 || i == 49 || i == 99 || i == 199)) {
            printf("SD Init: ACMD41 attempt %d, response = 0x%02X\r\n", i + 1, response);
        }
    }
    deselectCard();
    
    if (response != 0) {
        if (verbose) {
            printf("SD Init: ACMD41 with HCS failed, trying without HCS...\r\n");
        }
        // Try without HCS bit (for older cards)
        selectCard();
        for (int i = 0; i < 100; i++) {
            // Send CMD55 first
            uint8_t cmd55Response = sendCommand(SD_CMD55, 0);
            if (cmd55Response != 0x01) {
                deselectCard();
                break;
            }
            
            response = sendCommand(SD_ACMD41, 0x00000000);
            if (response == 0) {
                if (verbose) {
                    printf("SD Init: ACMD41 succeeded without HCS after %d attempts\r\n", i + 1);
                }
                break;
            }
            sleep_ms(10);
        }
        deselectCard();
        
        if (response != 0) {
            if (verbose) {
                printf("SD Init: ERROR - ACMD41 failed, response = 0x%02X\r\n", response);
                printf("SD Init: Card may be MMC or not properly powered\r\n");
            }
            return false;
        }
    }
    
set_block_size:
    
    sleep_ms(50);
    
    // Send CMD58 to read OCR (optional, but helps verify card)
    if (verbose) {
        printf("SD Init: Sending CMD58 (read OCR)...\r\n");
    }
    selectCard();
    response = sendCommand(SD_CMD58, 0);
    if (response == 0) {
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) {
            spi_read_blocking(spiInstance, 0xFF, &ocr[i], 1);
        }
        if (verbose) {
            printf("SD Init: OCR = %02X %02X %02X %02X\r\n", ocr[0], ocr[1], ocr[2], ocr[3]);
            if (ocr[0] & 0x80) {
                printf("SD Init: Card power up status: OK\r\n");
            }
        }
        // Check if card supports 3.3V (bit 20)
        // OCR[0] bit 7 = card power up status
    } else {
        if (verbose) {
            printf("SD Init: CMD58 failed, response = 0x%02X\r\n", response);
        }
    }
    deselectCard();
    sleep_ms(50);
    
    // Set block size to 512 bytes (CMD16) - only for standard SD cards
    // SDHC/SDXC cards have fixed 512-byte blocks
    if (!isSDHC) {
        if (verbose) {
            printf("SD Init: Sending CMD16 (set block size)...\r\n");
        }
        selectCard();
        response = sendCommand(SD_CMD16, 512);
        deselectCard();
        if (response != 0) {
            if (verbose) {
                printf("SD Init: ERROR - CMD16 failed, response = 0x%02X\r\n", response);
            }
            return false;
        }
        sleep_ms(50);
    }
    

    initialized = true;

    // Set SPI speed to specified maximum baudrate
    currentBaudrate = maxBaudrate;
    spi_set_baudrate(spiInstance, currentBaudrate);
    
    if (verbose) {
        printf("SD Init: SPI speed set to %u Hz (%.2f MHz)\r\n", 
               currentBaudrate, currentBaudrate / 1000000.0f);
    }
    
    // Initialize FAT32 filesystem
    fat32 = new FAT32(this);
    
    // Initialize FAT32
    if (!fat32->init()) {
        // FAT32 init failed, but SD card is still usable
        delete fat32;
        fat32 = nullptr;
    }
    
    return true;
}

bool SDCardManager::isInitialized() const {
    return initialized;
}

void SDCardManager::deinit() {
    if (initialized) {
        // Clean up FAT32
        if (fat32) {
            delete fat32;
            fat32 = nullptr;
        }
        
        // Deselect card
        deselectCard();
        
        initialized = false;
    }
}

bool SDCardManager::isCardPresent() const {
    if (detectPin == 0xFF) {
        // No detect pin configured, assume card is always present
        return true;
    }
    return cardPresent;
}

bool SDCardManager::checkCardPresence() {
    if (detectPin == 0xFF) {
        // No detect pin configured
        return true;
    }
    
    bool newState = !gpio_get(detectPin);  // Inverted: LOW = card present
    
    if (newState != cardPresent) {
        cardPresent = newState;
        
        if (!cardPresent && initialized) {
            // Card was removed, deinitialize
            printf("SD card removed, deinitializing...\r\n");
            deinit();
        }
        
        return true;  // State changed
    }
    
    return false;  // No change
}

// Select SD card (CS low)
void SDCardManager::selectCard() {
    gpio_put(csPin, 0);
    waitForReady();
}

// Deselect SD card (CS high)
void SDCardManager::deselectCard() {
    gpio_put(csPin, 1);
    waitForReady();
}

// Wait for card to be ready
void SDCardManager::waitForReady() {
    uint8_t dummy = 0xFF;
    uint8_t response = 0;
    for (int i = 0; i < 200; i++) {  // Increased timeout
        spi_read_blocking(spiInstance, dummy, &response, 1);
        if (response == 0xFF) {
            break;
        }
    }
}

// Send command to SD card
uint8_t SDCardManager::sendCommand(uint8_t cmd, uint32_t arg) {
    uint8_t command[6];
    command[0] = 0x40 | cmd;
    command[1] = (arg >> 24) & 0xFF;
    command[2] = (arg >> 16) & 0xFF;
    command[3] = (arg >> 8) & 0xFF;
    command[4] = arg & 0xFF;
    
    // Calculate CRC for CMD0 and CMD8
    if (cmd == SD_CMD0) {
        command[5] = 0x95;  // CRC for CMD0
    } else if (cmd == SD_CMD8) {
        command[5] = 0x87;  // CRC for CMD8 (correct CRC for arg 0x1AA)
    } else {
        command[5] = 0xFF;  // No CRC for other commands (or dummy CRC)
    }
    
    // Send command
    spi_write_blocking(spiInstance, command, 6);
    
    // Wait for response (R1 format)
    // Response starts with 0 bit, so we wait until MSB is 0
    uint8_t response = 0xFF;
    for (int i = 0; i < 20; i++) {  // Increased timeout
        spi_read_blocking(spiInstance, 0xFF, &response, 1);
        if ((response & 0x80) == 0) {  // Response bit cleared (MSB = 0)
            break;
        }
    }
    
    return response;
}

// Send application command
uint8_t SDCardManager::sendACommand(uint8_t cmd, uint32_t arg) {
    sendCommand(SD_CMD55, 0);
    return sendCommand(cmd, arg);
}

// Read a single block (512 bytes)
bool SDCardManager::readBlock(uint32_t blockAddress, uint8_t* buffer) {
    if (!initialized || buffer == nullptr) {
        return false;
    }
    
    selectCard();
    
    // Send CMD17 (read single block)
    uint8_t response = sendCommand(SD_CMD17, blockAddress);
    if (response != 0) {
        deselectCard();
        return false;
    }
    
    // Wait for data token (0xFE)
    uint8_t token = 0xFF;
    for (int i = 0; i < 2000; i++) {  // Increased timeout
        spi_read_blocking(spiInstance, 0xFF, &token, 1);
        if (token == 0xFE) {  // Data token
            break;
        }
        if (token != 0xFF) {
            // Got some response, but not data token - might be error
            break;
        }
    }
    
    if (token != 0xFE) {
        deselectCard();
        return false;
    }
    
    // Read 512 bytes
    spi_read_blocking(spiInstance, 0xFF, buffer, SD_BLOCK_SIZE);
    
    // Read CRC (2 bytes) - ignore
    uint8_t crc[2];
    spi_read_blocking(spiInstance, 0xFF, crc, 2);
    
    deselectCard();
    return true;
}

// Write a single block (512 bytes)
bool SDCardManager::writeBlock(uint32_t blockAddress, const uint8_t* buffer) {
    if (!initialized || buffer == nullptr) {
        return false;
    }
    
    selectCard();
    
    // Send CMD24 (write single block)
    uint8_t response = sendCommand(SD_CMD24, blockAddress);
    if (response != 0) {
        deselectCard();
        return false;
    }
    
    // Send data token
    uint8_t token = 0xFE;
    spi_write_blocking(spiInstance, &token, 1);
    
    // Write 512 bytes
    spi_write_blocking(spiInstance, buffer, SD_BLOCK_SIZE);
    
    // Send dummy CRC
    uint8_t crc[2] = {0xFF, 0xFF};
    spi_write_blocking(spiInstance, crc, 2);
    
    // Wait for response
    uint8_t writeResponse = 0xFF;
    for (int i = 0; i < 100; i++) {
        spi_read_blocking(spiInstance, 0xFF, &writeResponse, 1);
        if ((writeResponse & 0x1F) == 0x05) {  // Data accepted
            break;
        }
    }
    
    // Wait for write to complete
    uint8_t busy = 0;
    for (int i = 0; i < 1000; i++) {
        spi_read_blocking(spiInstance, 0xFF, &busy, 1);
        if (busy == 0xFF) {
            break;
        }
    }
    
    deselectCard();
    return ((writeResponse & 0x1F) == 0x05);
}

// Read file using FAT32
bool SDCardManager::readFile(const char* filename, uint8_t* buffer, uint32_t maxSize, uint32_t* bytesRead) {
    if (!initialized || !buffer) {
        return false;
    }
    
    // Use FAT32 if available
    if (fat32) {
        return fat32->readFile(filename, buffer, maxSize, bytesRead);
    }
    
    // Fallback: simplified read from block 0
    if (bytesRead) {
        *bytesRead = 0;
    }
    
    if (readBlock(0, buffer)) {
        if (bytesRead) {
            *bytesRead = SD_BLOCK_SIZE < maxSize ? SD_BLOCK_SIZE : maxSize;
        }
        return true;
    }
    
    return false;
}

bool SDCardManager::fileExists(const char* filename) {
    if (!initialized) {
        return false;
    }
    
    // Use FAT32 if available
    if (fat32) {
        return fat32->fileExists(filename);
    }
    
    // Fallback: always return false
    return false;
}

// List files using FAT32
bool SDCardManager::listFiles(char* fileList, uint32_t maxSize, uint32_t* fileCount) {
    if (!initialized || !fileList) {
        return false;
    }
    
    if (fat32) {
        return fat32->listFiles(fileList, maxSize, fileCount);
    }
    
    return false;
}

// Get FAT32 instance
FAT32* SDCardManager::getFAT32() const {
    return fat32;
}

// Load disk image from SD card using FAT32
bool SDCardManager::loadDiskImage(const char* filename, uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead) {
    if (!initialized || buffer == nullptr) {
        return false;
    }
    
    // Use FAT32 if available
    if (fat32) {
        // Get file size first
        uint32_t fileSize = fat32->getFileSize(filename);
        if (fileSize == 0) {
            if (bytesRead) *bytesRead = 0;
            return false;
        }
        
        // Check if it's a .dsk file by extension
        bool isDSKFile = false;
        const char* ext = strrchr(filename, '.');
        if (ext && (ext[1] == 'd' || ext[1] == 'D') && 
                   (ext[2] == 's' || ext[2] == 'S') && 
                   (ext[3] == 'k' || ext[3] == 'K')) {
            isDSKFile = true;
        }
        
        // .dsk files are typically 143360 bytes (35 tracks * 16 sectors * 256 bytes)
        // If file is larger than expected, it might have a header (64 or 256 bytes)
        // Common headers: 64, 128, 256 bytes
        if (isDSKFile && fileSize > bufferSize) {
            // Calculate header size (difference between file size and expected RAW size)
            uint32_t headerSize = fileSize - bufferSize;
            
            // Only handle small headers (64, 128, or 256 bytes)
            if (headerSize <= 256 && headerSize > 0) {
                // To skip header, we need to read fileSize bytes (header + data)
                // But buffer is only bufferSize bytes, so we can't safely read fileSize bytes
                // FAT32::readFile will limit reading to bufferSize (min(fileSize, bufferSize))
                // So we can only read bufferSize bytes, which includes header + part of data
                
                // Solution: Read fileSize bytes by passing fileSize as maxSize
                // FAT32::readFile will read min(fileSize, bufferSize) = bufferSize bytes
                // This gives us header + partial data, but we need all data
                
                // Actually, we need to read fileSize bytes, but buffer is only bufferSize
                // This is a fundamental limitation - we can't read more than bufferSize
                
                // For now, return error - this requires either:
                // 1. FAT32::readFile with offset support, or
                // 2. A temporary buffer large enough for fileSize (too much memory)
                if (bytesRead) *bytesRead = 0;
                return false;
            } else {
                // Header too large or zero, not supported  
                if (bytesRead) *bytesRead = 0;
                return false;
            }
        }
        
        // Default: read file directly into buffer
        // This works for:
        // - .dsk files that are exactly 143360 bytes (no header, already RAW format)
        // - Any other file format (RAW format)
        uint32_t bytesReadSoFar = 0;
        if (!fat32->readFile(filename, buffer, bufferSize, &bytesReadSoFar)) {
            if (bytesRead) *bytesRead = 0;
            return false;
        }
        
        if (bytesRead) {
            *bytesRead = bytesReadSoFar;
        }
        
        return bytesReadSoFar > 0;
    } else {
        // Fallback: read raw blocks (old method)
        uint32_t blocksToRead = (bufferSize + SD_BLOCK_SIZE - 1) / SD_BLOCK_SIZE;
        uint32_t totalBytes = 0;
        
        for (uint32_t i = 0; i < blocksToRead && totalBytes < bufferSize; i++) {
            uint8_t blockBuffer[SD_BLOCK_SIZE];
            if (!readBlock(i, blockBuffer)) {
                if (bytesRead) {
                    *bytesRead = totalBytes;
                }
                return false;
            }
            
            uint32_t bytesToCopy = SD_BLOCK_SIZE;
            if (totalBytes + bytesToCopy > bufferSize) {
                bytesToCopy = bufferSize - totalBytes;
            }
            
            memcpy(buffer + totalBytes, blockBuffer, bytesToCopy);
            totalBytes += bytesToCopy;
        }
        
        if (bytesRead) {
            *bytesRead = totalBytes;
        }
        
        return true;
    }
}

// Save track data to file at specific track position
bool SDCardManager::saveTrackToFile(const char* filename, int track, const uint8_t* trackData, uint32_t trackSize) {
    if (!initialized || !trackData || trackSize == 0) {
        return false;
    }
    
    // Use FAT32 if available
    if (fat32) {
        // Use filename as-is - findFile() searches in current directory
        // Don't construct full path because listFiles() returns only filenames
        // and findFile() searches in the current directory context
        
        // Calculate offset in file: track * bytes per track
        // Apple II disk format: 35 tracks * 16 sectors * 256 bytes = 143360 bytes total
        // Each track is 4096 bytes (16 sectors * 256 bytes)
        const uint32_t BYTES_PER_TRACK = 16 * 256;  // 4096 bytes per track
        uint32_t offset = track * BYTES_PER_TRACK;
        
        // Write track data at calculated offset
        return fat32->writeFileAtOffset(filename, offset, trackData, trackSize);
    }
    
    return false;
}

// Read track data from file at specific track position
bool SDCardManager::readTrackFromFile(const char* filename, int track, uint8_t* trackData, uint32_t trackSize) {
    //printf("readTrackFromFile: filename='%s', track=%d, trackSize=%u\r\n", 
    //       filename ? filename : "(null)", track, trackSize);
    
    if (!initialized || !trackData || trackSize == 0) {
        printf("readTrackFromFile: Invalid parameters - initialized=%d, trackData=%p, trackSize=%u\r\n",
               initialized, trackData, trackSize);
        return false;
    }
    
    // Use FAT32 if available
    if (fat32) {
        // Use filename as-is - findFile() searches in current directory
        // Don't construct full path because listFiles() returns only filenames
        // and findFile() searches in the current directory context
        
        //printf("readTrackFromFile: Using filename='%s' (findFile will search in current directory)\r\n", filename);
        
        // Calculate offset in file: track * bytes per track
        // Apple II disk format: 35 tracks * 16 sectors * 256 bytes = 143360 bytes total
        // Each track is 4096 bytes (16 sectors * 256 bytes)
        const uint32_t BYTES_PER_TRACK = 16 * 256;  // 4096 bytes per track
        uint32_t offset = track * BYTES_PER_TRACK;
        
        //printf("readTrackFromFile: offset=%u (track %d * %u bytes)\r\n", offset, track, BYTES_PER_TRACK);
        
        // Read track data at calculated offset
        uint32_t bytesRead = 0;
        if (fat32->readFileAtOffset(filename, offset, trackData, trackSize, &bytesRead)) {
            //printf("readTrackFromFile: readFileAtOffset SUCCESS - bytesRead=%u, expected=%u\r\n",
            //       bytesRead, trackSize);
            return bytesRead == trackSize;
        } else {
            printf("readTrackFromFile: readFileAtOffset FAILED\r\n");
        }
    } else {
        printf("readTrackFromFile: FAT32 not available\r\n");
    }
    
    return false;
}

// Test maximum read speed for SD card
// This function is completely independent - it initializes the card, tests speeds, and deinitializes
// Returns the maximum baudrate (in Hz) that can be used successfully
uint32_t SDCardManager::testMaxReadSpeed(uint32_t testBlocks, bool verbose) {
    if (verbose) {
        printf("SD Speed Test: Starting independent speed test...\r\n");
        printf("SD Speed Test: Testing %u blocks per speed\r\n", testBlocks);
    }
    
    // Save original state
    bool wasInitialized = initialized;
    
    // Initialize SPI with low speed first
    spi_init(spiInstance, 100000);  // Start with 100kHz
    gpio_set_function(mosiPin, GPIO_FUNC_SPI);
    gpio_set_function(misoPin, GPIO_FUNC_SPI);
    gpio_set_function(sckPin, GPIO_FUNC_SPI);
    
    // Initialize CS pin
    gpio_init(csPin);
    gpio_set_dir(csPin, GPIO_OUT);
    gpio_put(csPin, 1);  // Deselect (high = inactive)
    sleep_ms(100);
    
    // Send 80+ clock cycles to initialize
    deselectCard();
    uint8_t dummy = 0xFF;
    for (int i = 0; i < 20; i++) {
        spi_write_blocking(spiInstance, &dummy, 1);
    }
    sleep_ms(100);
    
    // Send CMD0 to reset card
    selectCard();
    uint8_t response = sendCommand(SD_CMD0, 0);
    deselectCard();
    sleep_ms(50);
    
    if (response == 0xFF || (response & 0xFE) != 0) {
        if (verbose) {
            printf("SD Speed Test: ERROR - CMD0 failed, response = 0x%02X\r\n", response);
        }
        // Restore original state
        if (wasInitialized) {
            initialized = true;
        }
        return 0;
    }
    
    // Try CMD8 (for SDHC/SDXC cards)
    bool isSDHC = false;
    selectCard();
    response = sendCommand(SD_CMD8, 0x1AA);
    
    if (response == SD_R1_IDLE) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) {
            spi_read_blocking(spiInstance, 0xFF, &r7[i], 1);
        }
        if (r7[2] == 0x01 && r7[3] == 0xAA) {
            isSDHC = true;
        }
    }
    deselectCard();
    sleep_ms(50);
    
    bool isMMC = (response == 0x09);
    
    // Initialize card with ACMD41 (or CMD1 for MMC)
    uint32_t acmd41Arg = isSDHC ? 0x40000000 : 0x00000000;
    
    if (isMMC) {
        selectCard();
        for (int i = 0; i < 200; i++) {
            response = sendCommand(1, 0x40FF8000);
            if (response == 0) break;
            sleep_ms(i < 10 ? 1 : 10);
        }
        deselectCard();
    } else {
        // Try ACMD41 for SD cards
        selectCard();
        for (int i = 0; i < 200; i++) {
            uint8_t cmd55Response = sendCommand(SD_CMD55, 0);
            if (cmd55Response != 0x01) {
                deselectCard();
                break;
            }
            response = sendCommand(SD_ACMD41, acmd41Arg);
            if (response == 0) break;
            sleep_ms(i < 10 ? 1 : 10);
        }
        deselectCard();
    }
    
    if (response != 0) {
        if (verbose) {
            printf("SD Speed Test: ERROR - Card initialization failed\r\n");
        }
        // Restore original state
        if (wasInitialized) {
            initialized = true;
        }
        return 0;
    }
    
    sleep_ms(50);
    
    // Set block size (only for standard SD cards)
    if (!isSDHC) {
        selectCard();
        response = sendCommand(SD_CMD16, 512);
        deselectCard();
        if (response != 0) {
            if (verbose) {
                printf("SD Speed Test: ERROR - CMD16 failed\r\n");
            }
            // Restore original state
            if (wasInitialized) {
                initialized = true;
            }
            return 0;
        }
        sleep_ms(50);
    }
    
    // Card is now initialized for testing
    // Set initialized flag so readBlock() will work
    initialized = true;
    
    if (verbose) {
        printf("SD Speed Test: Card initialized, starting speed tests...\r\n");
    }
    
    // First, test at low speed to verify card is working
    spi_set_baudrate(spiInstance, 1000000);  // 1MHz
    sleep_ms(10);
    
    uint8_t testBuffer[SD_BLOCK_SIZE];
    if (!readBlock(0, testBuffer)) {
        if (verbose) {
            printf("SD Speed Test: ERROR - Cannot read block 0 at 1MHz (card may not be ready)\r\n");
        }
        // Restore original state
        if (wasInitialized) {
            initialized = true;
        }
        return 0;
    }
    
    if (verbose) {
        printf("SD Speed Test: Verified card is readable at 1MHz\r\n");
    }
    
    // Test speeds (in Hz): 1MHz, 2MHz, 5MHz, 10MHz, 15MHz, 20MHz, 25MHz, 30MHz, 40MHz, 50MHz
    uint32_t testSpeeds[] = {
        1000000,   // 1 MHz
        2000000,   // 2 MHz
        5000000,   // 5 MHz
        10000000,  // 10 MHz
        15000000,  // 15 MHz
        20000000,  // 20 MHz
        25000000,  // 25 MHz
        30000000,  // 30 MHz
        40000000,  // 40 MHz
        50000000   // 50 MHz
    };
    
    uint32_t maxSuccessfulSpeed = 0;
    
    // Test each speed
    for (uint32_t i = 0; i < sizeof(testSpeeds) / sizeof(testSpeeds[0]); i++) {
        uint32_t testSpeed = testSpeeds[i];
        
        // Set SPI speed and wait for it to stabilize
        spi_set_baudrate(spiInstance, testSpeed);
        sleep_ms(5);  // Small delay for speed change to take effect
        
        if (verbose) {
            printf("SD Speed Test: Testing %u Hz (%.2f MHz)... ", testSpeed, testSpeed / 1000000.0f);
        }
        
        // Test reading multiple blocks
        bool allBlocksRead = true;
        for (uint32_t block = 0; block < testBlocks; block++) {
            // Try to read block (use block 0, 1, 2, etc. - these should always exist)
            if (!readBlock(block, testBuffer)) {
                allBlocksRead = false;
                if (verbose) {
                    printf("FAILED at block %u\r\n", block);
                }
                break;
            }
        }
        
        if (allBlocksRead) {
            maxSuccessfulSpeed = testSpeed;
            if (verbose) {
                printf("OK\r\n");
            }
        } else {
            // Stop testing higher speeds if this one failed
            break;
        }
    }
    
    // Re-sync with card after testing
    deselectCard();
    sleep_ms(10);
    uint8_t syncByte = 0xFF;
    for (int i = 0; i < 20; i++) {
        spi_write_blocking(spiInstance, &syncByte, 1);
    }
    waitForReady();
    sleep_ms(20);
    
    // Deinitialize card (deselect and reset SPI)
    deselectCard();
    sleep_ms(50);
    
    // Restore original state
    if (wasInitialized) {
        initialized = true;
    } else {
        initialized = false;
    }
    
    if (verbose) {
        if (maxSuccessfulSpeed > 0) {
            printf("SD Speed Test: Maximum successful speed: %u Hz (%.2f MHz)\r\n", 
                   maxSuccessfulSpeed, maxSuccessfulSpeed / 1000000.0f);
        } else {
            printf("SD Speed Test: No successful speed found\r\n");
        }
    }
    
    return maxSuccessfulSpeed;
}

