#include "CLIHandler.h"
#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "PinConfig.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Forward declarations are resolved by including the headers above

// Constructor
CLIHandler::CLIHandler(uart_inst_t* uart, uint8_t txPin, uint8_t rxPin, uint32_t baudrate) {
    uartInstance = uart;
    bufferIndex = 0;
    commandReady = false;
    argCount = 0;
    floppyEmulator = nullptr;
    sdCardManager = nullptr;
    memset(inputBuffer, 0, CLI_BUFFER_SIZE);
    memset(args, 0, sizeof(args));
}

// Initialize UART
void CLIHandler::init() {
    uart_init(uartInstance, CLI_UART_BAUDRATE);
    gpio_set_function(CLI_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(CLI_UART_RX, GPIO_FUNC_UART);
    
    sendResponse("\r\nApple II Floppy Emulator CLI\r\n");
    sendResponse("Type 'help' for commands\r\n");
    sendPrompt();
}

// Set floppy emulator reference
void CLIHandler::setFloppyEmulator(void* floppy) {
    floppyEmulator = floppy;
}

// Set SD card manager reference
void CLIHandler::setSDCardManager(void* sdCard) {
    sdCardManager = sdCard;
}

// Helper macros to cast void* to proper types
#define GET_FLOPPY() ((FloppyEmulator*)floppyEmulator)
#define GET_SD() ((SDCardManager*)sdCardManager)

// Process incoming characters
void CLIHandler::process() {
    if (uart_is_readable(uartInstance)) {
        char c = uart_getc(uartInstance);
        
        // Echo character
        uart_putc(uartInstance, c);
        
        // Handle backspace
        if (c == '\b' || c == 0x7F) {
            if (bufferIndex > 0) {
                bufferIndex--;
                inputBuffer[bufferIndex] = 0;
                uart_putc(uartInstance, ' ');
                uart_putc(uartInstance, '\b');
            }
            return;
        }
        
        // Handle enter/carriage return
        if (c == '\r' || c == '\n') {
            uart_putc(uartInstance, '\r');
            uart_putc(uartInstance, '\n');
            inputBuffer[bufferIndex] = 0;
            if (bufferIndex > 0) {
                commandReady = true;
                processCommand();
            } else {
                sendPrompt();
            }
            clearBuffer();
            return;
        }
        
        // Add character to buffer
        if (bufferIndex < CLI_BUFFER_SIZE - 1 && c >= 32 && c < 127) {
            inputBuffer[bufferIndex++] = c;
        }
    }
}

bool CLIHandler::isCommandReady() const {
    return commandReady;
}

// Process command
void CLIHandler::processCommand() {
    commandReady = false;
    parseCommand();
    
    if (argCount == 0) {
        sendPrompt();
        return;
    }
    
    const char* cmd = args[0];
    
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        handleHelp();
    }
    else if (strcmp(cmd, "load") == 0) {
        if (argCount >= 2) {
            handleLoad(args[1]);
        } else {
            sendResponse("Usage: load <filename>\r\n");
        }
    }
    else if (strcmp(cmd, "list") == 0) {
        handleList();
    }
    else if (strcmp(cmd, "info") == 0) {
        handleInfo();
    }
    else if (strcmp(cmd, "status") == 0) {
        handleStatus();
    }
    else if (strcmp(cmd, "seek") == 0) {
        if (argCount >= 2) {
            int track = atoi(args[1]);
            handleSeek(track);
        } else {
            sendResponse("Usage: seek <track>\r\n");
        }
    }
    else if (strcmp(cmd, "gpio") == 0 || strcmp(cmd, "pins") == 0) {
        handleGPIO();
    }
    else if (strcmp(cmd, "read") == 0) {
        if (argCount >= 3) {
            int track = atoi(args[1]);
            int sector = atoi(args[2]);
            handleRead(track, sector);
        } else {
            sendResponse("Usage: read <track> <sector>\r\n");
        }
    }
    else if (strcmp(cmd, "cache") == 0) {
        if (argCount >= 2) {
            int sector = atoi(args[1]);
            handleCache(sector);
        } else {
            sendResponse("Usage: cache <sector>\r\n");
        }
    }
    else if (strcmp(cmd, "cd") == 0) {
        if (argCount >= 2) {
            handleChangeDirectory(args[1]);
        } else {
            handleChangeDirectory("/");  // Go to root if no argument
        }
    }
    else if (strcmp(cmd, "pwd") == 0) {
        handlePrintWorkingDirectory();
    }
    else if (strcmp(cmd, "gpio") == 0 || strcmp(cmd, "pins") == 0) {
        handleGPIO();
    }
    else if (strcmp(cmd, "test") == 0) {
        handleTest();
    }
    else {
        char msg[64];
        snprintf(msg, sizeof(msg), "Unknown command: %s\r\n", cmd);
        sendResponse(msg);
    }
    
    sendPrompt();
}

// Parse command into arguments
void CLIHandler::parseCommand() {
    argCount = 0;
    char* token = strtok(inputBuffer, " \t\r\n");
    
    while (token != nullptr && argCount < CLI_MAX_ARGS) {
        args[argCount++] = token;
        token = strtok(nullptr, " \t\r\n");
    }
}

// Send response
void CLIHandler::sendResponse(const char* message) {
    uart_puts(uartInstance, message);
}

// Send prompt
void CLIHandler::sendPrompt() {
    sendResponse("> ");
}

// Clear buffer
void CLIHandler::clearBuffer() {
    bufferIndex = 0;
    memset(inputBuffer, 0, CLI_BUFFER_SIZE);
    argCount = 0;
}

// Command handlers
void CLIHandler::handleHelp() {
    sendResponse("Available commands:\r\n");
    sendResponse("  help              - Show this help\r\n");
    sendResponse("  load <file>        - Load disk image from SD card\r\n");
    sendResponse("  list               - List files in current directory\r\n");
    sendResponse("  cd <dir>           - Change directory (cd .. for parent)\r\n");
    sendResponse("  pwd                - Print current directory\r\n");
    sendResponse("  info               - Show disk image info\r\n");
    sendResponse("  status             - Show emulator status\r\n");
    sendResponse("  seek <track>     - Seek to track (0-34)\r\n");
    sendResponse("  read <t> <s>       - Read track and sector\r\n");
    sendResponse("  gpio/pins          - Show GPIO pin states\r\n");
    sendResponse("  test               - Test emulator\r\n");
}

void CLIHandler::handleLoad(const char* filename) {
    if (!sdCardManager) {
        sendResponse("SD card not initialized\r\n");
        return;
    }
    
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    sendResponse("Loading disk image...\r\n");
    
    uint8_t* diskImage = GET_FLOPPY()->getDiskImage();
    uint32_t diskSize = GET_FLOPPY()->getDiskImageSize();
    uint32_t bytesRead = 0;
    
    if (GET_SD()->loadDiskImage(filename, diskImage, diskSize, &bytesRead)) {
        // Call FloppyEmulator::loadDiskImage() to set initial track to 17
        GET_FLOPPY()->loadDiskImage(diskImage, bytesRead);
        char msg[128];
        snprintf(msg, sizeof(msg), "Loaded %u bytes from %s\r\n", bytesRead, filename);
        sendResponse(msg);
    } else {
        sendResponse("Failed to load disk image\r\n");
    }
}

void CLIHandler::handleList() {
    if (!sdCardManager) {
        sendResponse("SD card not initialized\r\n");
        return;
    }
    
    char fileList[1024];
    uint32_t fileCount = 0;
    
    if (GET_SD()->listFiles(fileList, sizeof(fileList), &fileCount)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Found %u files:\r\n", fileCount);
        sendResponse(msg);
        sendResponse(fileList);
    } else {
        sendResponse("Failed to list files or no files found\r\n");
    }
}

void CLIHandler::handleInfo() {
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    FloppyEmulator* floppy = GET_FLOPPY();
    char msg[256];
    snprintf(msg, sizeof(msg), 
        "Disk Image Info:\r\n"
        "  Size: %u bytes\r\n"
        "  Tracks: 35\r\n"
        "  Sectors per track: 16\r\n"
        "  Bytes per sector: 256\r\n"
        "  Current track: %d\r\n"
        "  At track 0: %s\r\n"
        "  Drive selected: %s\r\n",
        floppy->getDiskImageSize(),
        floppy->getCurrentTrack(),
        floppy->isAtTrack0() ? "Yes" : "No",
        floppy->isDriveSelected() ? "Yes" : "No"
    );
    sendResponse(msg);
}

void CLIHandler::handleStatus() {
    handleInfo();
    
    if (sdCardManager) {
        char msg[64];
        snprintf(msg, sizeof(msg), "SD Card: %s\r\n", 
                 GET_SD()->isInitialized() ? "Initialized" : "Not initialized");
        sendResponse(msg);
    }
}

void CLIHandler::handleGPIO() {
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    char msg[256];
    
    // Stepper motor phase pins (PH0-PH3)
    sendResponse("Stepper Motor Phases:\r\n");
    for (int i = 0; i < 4; i++) {
        uint8_t pin = GPIO_PH0 + i;
        bool state = gpio_get(pin);
        snprintf(msg, sizeof(msg), "  PH%d (GPIO%d): %s\r\n", i, pin, state ? "HIGH" : "LOW");
        sendResponse(msg);
    }
    
    // Read/Write control pins
    sendResponse("Read/Write Control:\r\n");
    bool readState = gpio_get(GPIO_READ);
    bool writeState = gpio_get(GPIO_WRITE);
    bool writeEnableState = gpio_get(GPIO_WRITE_ENABLE);
    bool driveSelState = gpio_get(GPIO_DRIVE_SEL);
    
    snprintf(msg, sizeof(msg), "  READ (GPIO%d): %s\r\n", GPIO_READ, readState ? "HIGH" : "LOW");
    sendResponse(msg);
    snprintf(msg, sizeof(msg), "  WRITE (GPIO%d): %s\r\n", GPIO_WRITE, writeState ? "HIGH" : "LOW");
    sendResponse(msg);
    snprintf(msg, sizeof(msg), "  WRITE_ENABLE (GPIO%d): %s\r\n", GPIO_WRITE_ENABLE, writeEnableState ? "HIGH" : "LOW");
    sendResponse(msg);
    snprintf(msg, sizeof(msg), "  DRIVE_SEL (GPIO%d): %s\r\n", GPIO_DRIVE_SEL, driveSelState ? "HIGH" : "LOW");
    sendResponse(msg);
    
    // Calculate phase state (binary representation)
    uint8_t phaseState = 0;
    for (int i = 0; i < 4; i++) {
        if (gpio_get(GPIO_PH0 + i)) {
            phaseState |= (1 << i);
        }
    }
    snprintf(msg, sizeof(msg), "Phase State (binary): 0b%04b (0x%02X)\r\n", phaseState, phaseState);
    sendResponse(msg);
    
    // Show current track from emulator
    int currentTrack = GET_FLOPPY()->getCurrentTrack();
    snprintf(msg, sizeof(msg), "Current Track: %d\r\n", currentTrack);
    sendResponse(msg);
}

void CLIHandler::handleSeek(int track) {
    sendResponse("Note: seekTrack() is for debugging only\r\n");
    sendResponse("In normal operation, Apple II controller controls stepper motor\r\n");
    sendResponse("Current track is determined by controller's phase signals\r\n");
    
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    if (track < 0 || track >= 35) {
        sendResponse("Invalid track number (0-34)\r\n");
        return;
    }
    
    // Set the track
    GET_FLOPPY()->setCurrentTrack(track);
    
    int currentTrack = GET_FLOPPY()->getCurrentTrack();
    char msg[64];
    snprintf(msg, sizeof(msg), "Track set to: %d\r\n", currentTrack);
    sendResponse(msg);
}

void CLIHandler::handleRead(int track, int sector) {
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    if (track < 0 || track >= 35) {
        sendResponse("Track must be 0-34\r\n");
        return;
    }
    
    if (sector < 0 || sector >= 16) {
        sendResponse("Sector must be 0-15\r\n");
        return;
    }
    
    uint8_t buffer[256];
    if (GET_FLOPPY()->readSector(track, sector, buffer)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Track %d, Sector %d:\r\n", track, sector);
        sendResponse(msg);
        
        // Print first 32 bytes as hex
        for (int i = 0; i < 32; i++) {
            snprintf(msg, sizeof(msg), "%02X ", buffer[i]);
            sendResponse(msg);
            if ((i + 1) % 16 == 0) {
                sendResponse("\r\n");
            }
        }
        sendResponse("\r\n");
    } else {
        sendResponse("Failed to read sector\r\n");
    }
}

void CLIHandler::handleCache(int sector) {
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    if (sector < 0 || sector >= 16) {
        sendResponse("Sector must be 0-15\r\n");
        return;
    }
    
    FloppyEmulator* floppy = GET_FLOPPY();
    int currentTrack = floppy->getCurrentTrack();
    
    // Get GCR cache for sector
    uint8_t gcrBuffer[416];
    uint32_t gcrLen = 0;
    
    bool gcrOk = floppy->getGCRSectorFromCache(sector, gcrBuffer, sizeof(gcrBuffer), &gcrLen);
    
    // Get raw disk image data for sector
    uint8_t rawBuffer[256];
    bool rawOk = floppy->readSector(currentTrack, sector, rawBuffer);
    
    // Display GCR cache
    if (gcrOk) {
        char msg[128];
        snprintf(msg, sizeof(msg), "GCR cache Track %d Sector %d (%u bytes):\r\n", currentTrack, sector, gcrLen);
        sendResponse(msg);
        
        for (uint32_t i = 0; i < gcrLen; i++) {
            snprintf(msg, sizeof(msg), "%02X ", gcrBuffer[i]);
            sendResponse(msg);
            if ((i + 1) % 16 == 0) {
                sendResponse("\r\n");
            }
        }
        if (gcrLen % 16 != 0) {
            sendResponse("\r\n");
        }
        sendResponse("\r\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to get GCR cache for sector %d (current track: %d)\r\n", sector, currentTrack);
        sendResponse(msg);
    }
    
    // Display raw disk image data
    if (rawOk) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Disk image Track %d Sector %d (256 bytes):\r\n", currentTrack, sector);
        sendResponse(msg);
        
        for (int i = 0; i < 256; i++) {
            snprintf(msg, sizeof(msg), "%02X ", rawBuffer[i]);
            sendResponse(msg);
            if ((i + 1) % 16 == 0) {
                sendResponse("\r\n");
            }
        }
        sendResponse("\r\n");
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to read disk image for Track %d Sector %d\r\n", currentTrack, sector);
        sendResponse(msg);
    }
}

void CLIHandler::handleChangeDirectory(const char* dirname) {
    if (!sdCardManager) {
        sendResponse("SD card not initialized\r\n");
        return;
    }
    
    FAT32* fat32 = GET_SD()->getFAT32();
    if (!fat32) {
        sendResponse("FAT32 not initialized\r\n");
        return;
    }
    
    if (fat32->changeDirectory(dirname)) {
        char path[256];
        if (fat32->getCurrentDirectory(path, sizeof(path))) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Changed to: %s\r\n", path);
            sendResponse(msg);
        } else {
            sendResponse("Directory changed\r\n");
        }
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "Failed to change directory: %s\r\n", dirname);
        sendResponse(msg);
    }
}

void CLIHandler::handlePrintWorkingDirectory() {
    if (!sdCardManager) {
        sendResponse("SD card not initialized\r\n");
        return;
    }
    
    FAT32* fat32 = GET_SD()->getFAT32();
    if (!fat32) {
        sendResponse("FAT32 not initialized\r\n");
        return;
    }
    
    char path[256];
    if (fat32->getCurrentDirectory(path, sizeof(path))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Current directory: %s\r\n", path);
        sendResponse(msg);
    } else {
        sendResponse("Failed to get current directory\r\n");
    }
}

void CLIHandler::handleTest() {
    sendResponse("Running emulator test...\r\n");
    
    if (!floppyEmulator) {
        sendResponse("Floppy emulator not initialized\r\n");
        return;
    }
    
    // Test write and read
    uint8_t testData[256];
    for (int i = 0; i < 256; i++) {
        testData[i] = i;
    }
    
    if (GET_FLOPPY()->writeSector(0, 0, testData)) {
        sendResponse("Write test: OK\r\n");
    } else {
        sendResponse("Write test: FAILED\r\n");
        sendResponse("Test complete\r\n");
        return;
    }
    
    uint8_t readData[256];
    if (GET_FLOPPY()->readSector(0, 0, readData)) {
        bool match = true;
        int firstMismatch = -1;
        for (int i = 0; i < 256; i++) {
            if (readData[i] != testData[i]) {
                if (firstMismatch == -1) {
                    firstMismatch = i;
                }
                match = false;
            }
        }
        
        if (match) {
            sendResponse("Read test: OK\r\n");
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Read test: DATA MISMATCH at byte %d\r\n", firstMismatch);
            sendResponse(msg);
            if (firstMismatch >= 0 && firstMismatch < 256) {
                snprintf(msg, sizeof(msg), "  Expected: 0x%02X, Got: 0x%02X\r\n", 
                        testData[firstMismatch], readData[firstMismatch]);
                sendResponse(msg);
            }
        }
    } else {
        sendResponse("Read test: FAILED\r\n");
    }
    
    sendResponse("Test complete\r\n");
}

