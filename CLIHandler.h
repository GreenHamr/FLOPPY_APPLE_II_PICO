#ifndef CLI_HANDLER_H
#define CLI_HANDLER_H

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "PinConfig.h"
#include <stdint.h>
#include <stdbool.h>

#define CLI_BUFFER_SIZE    256
#define CLI_MAX_ARGS       16
#define CLI_MAX_FILENAME   64

class CLIHandler {
private:
    uart_inst_t* uartInstance;
    char inputBuffer[CLI_BUFFER_SIZE];
    int bufferIndex;
    bool commandReady;
    
    // Command parsing
    char* args[CLI_MAX_ARGS];
    int argCount;
    
    // Internal methods
    void processCommand();
    void parseCommand();
    void sendResponse(const char* message);
    void sendPrompt();
    void clearBuffer();
    
    // Command handlers
    void handleHelp();
    void handleLoad(const char* filename);
    void handleList();
    void handleInfo();
    void handleStatus();
    void handleSeek(int track);
    void handleRead(int track, int sector);
    void handleCache(int sector);
    void handleGPIO();
    void handleChangeDirectory(const char* dirname);
    void handlePrintWorkingDirectory();
    void handleTest();
    
    // Use void* to avoid circular dependencies
    void* floppyEmulator;
    void* sdCardManager;
    
public:
    // Constructor
    CLIHandler(uart_inst_t* uart, uint8_t txPin, uint8_t rxPin, uint32_t baudrate);
    
    // Initialization
    void init();
    
    // Set references to emulator and SD card
    void setFloppyEmulator(void* floppy);
    void setSDCardManager(void* sdCard);
    
    // Main processing loop (call periodically)
    void process();
    
    // Check if command is ready
    bool isCommandReady() const;
};

#endif // CLI_HANDLER_H

