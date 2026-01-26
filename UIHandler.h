#ifndef UI_HANDLER_H
#define UI_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/time.h"
#include "Display.h"
#include "RotaryEncoder.h"
#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "PinConfig.h"

// UI Screens
typedef enum {
    UI_SCREEN_MAIN = 0,
    UI_SCREEN_FILE_LIST = 1,
    UI_SCREEN_INFO = 2,
    UI_SCREEN_STATUS = 3,
    UI_SCREEN_LOADING = 4,
    UI_SCREEN_NO_SD_CARD = 5,
    UI_SCREEN_SD_ERROR = 6
} UIScreen;

// SD Card error types
typedef enum {
    SD_ERROR_NONE = 0,
    SD_ERROR_NOT_PRESENT = 1,
    SD_ERROR_READ_FAILED = 2,
    SD_ERROR_EXFAT = 3,
    SD_ERROR_NTFS = 4,
    SD_ERROR_FAT12 = 5,
    SD_ERROR_FAT16 = 6,
    SD_ERROR_UNKNOWN_FS = 7
} SDErrorType;

// Display layout constants (for SSD1306 split-screen displays)
#ifndef USE_MSP1601
    #define STATUS_BAR_HEIGHT OLED_STATUS_HEIGHT  // Upper yellow section
    #define CONTENT_AREA_HEIGHT OLED_CONTENT_HEIGHT  // Lower blue section
    #define STATUS_SEPARATOR_Y OLED_STATUS_HEIGHT   // Y position for separator line
#else
    #define STATUS_BAR_HEIGHT 0   // No status bar for MSP1601
    #define CONTENT_AREA_HEIGHT LCD_HEIGHT
    #define STATUS_SEPARATOR_Y 0
#endif

// Encoder sensitivity (number of encoder steps to skip before reacting)
#define ENCODER_SENSITIVITY 2  // Number of encoder steps to ignore before UI reacts (1 = most sensitive, higher = less sensitive)

class UIHandler {
private:
    Display* display;
    RotaryEncoder* encoder;
    FloppyEmulator* floppy;
    SDCardManager* sdCard;
    
    UIScreen currentScreen;
    int selectedIndex;
    int scrollOffset;
    
    // File list state
    char fileList[512];
    uint32_t fileCount;
    int maxVisibleItems;
    
    // Loaded file name
    char loadedFileName[64];
    
    // Timing
    absolute_time_t lastUpdateTime;
    bool needsRefresh;
    
    // Encoder step counter (for sensitivity)
    int encoderStepCounter;
    EncoderDirection lastEncoderDirection;
    
    // Loading screen timeout
    absolute_time_t loadingEndTime;
    
    // Screen rendering
    void renderStatusBar();  // Always render status in upper section
    void renderMainScreen();
    void renderFileListScreen();
    void renderInfoScreen();
    void renderStatusScreen();
    void renderLoadingScreen(const char* message);
    void renderNoSDCardScreen();
    void renderSDErrorScreen();
    
    // SD Card error state
    SDErrorType sdErrorType;
    
    // Navigation
    void handleEncoderInput();
    void updateFileList();
    
public:
    UIHandler(Display* disp, RotaryEncoder* enc);
    ~UIHandler();
    
    void init();
    void setFloppyEmulator(FloppyEmulator* floppyEmu);
    void setSDCardManager(SDCardManager* sdCardMgr);
    
    void update();  // Call this periodically in main loop
    void refresh();  // Force screen refresh
    
    // SD Card status screens
    void showNoSDCard();
    void showSDError(SDErrorType errorType);
    void showMainMenu();  // Return to main menu after SD card is ready
};

#endif // UI_HANDLER_H

