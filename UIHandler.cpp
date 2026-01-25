#include "UIHandler.h"
#include "FAT32.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>

UIHandler::UIHandler(Display* disp, RotaryEncoder* enc) {
    display = disp;
    encoder = enc;
    floppy = nullptr;
    sdCard = nullptr;
    currentScreen = UI_SCREEN_MAIN;
    selectedIndex = 0;
    scrollOffset = 0;
    fileCount = 0;
    // Calculate max visible items: content area minus header (Files: + separator line = 10px)
    // Starting Y position: STATUS_BAR_HEIGHT + 12, each file is 8px
    // Available height: CONTENT_AREA_HEIGHT - 12 (header area)
    maxVisibleItems = (CONTENT_AREA_HEIGHT - 12) / 8;  // About 4-5 files for 48px content area
    memset(fileList, 0, sizeof(fileList));
    memset(loadedFileName, 0, sizeof(loadedFileName));
    encoderStepCounter = 0;
    lastEncoderDirection = ENCODER_NONE;
    lastUpdateTime = get_absolute_time();
    loadingEndTime = get_absolute_time();
    needsRefresh = true;
}

UIHandler::~UIHandler() {
}

void UIHandler::init() {
    display->clear();
    display->update();
    needsRefresh = true;
}

void UIHandler::setFloppyEmulator(FloppyEmulator* floppyEmu) {
    floppy = floppyEmu;
}

void UIHandler::setSDCardManager(SDCardManager* sdCardMgr) {
    sdCard = sdCardMgr;
}

void UIHandler::update() {
    // Update encoder
    encoder->update();
    
    // Handle encoder input
    handleEncoderInput();
    
    // Refresh display every 100ms or when needed
    absolute_time_t now = get_absolute_time();
    int64_t diff = absolute_time_diff_us(lastUpdateTime, now);
    
    // Check if loading screen timeout has expired
    if (currentScreen == UI_SCREEN_LOADING) {
        if (time_reached(loadingEndTime)) {
            // Timeout expired - return to file list
            currentScreen = UI_SCREEN_FILE_LIST;
            needsRefresh = true;
        }
    }
    
    if (needsRefresh || diff > 100000) {  // 100ms
        switch (currentScreen) {
            case UI_SCREEN_MAIN:
                renderMainScreen();
                break;
            case UI_SCREEN_FILE_LIST:
                renderFileListScreen();
                break;
            case UI_SCREEN_INFO:
                renderInfoScreen();
                break;
            case UI_SCREEN_STATUS:
                renderStatusScreen();
                break;
            case UI_SCREEN_LOADING:
                // Loading screen is rendered explicitly with message
                break;
        }
        display->update();
        lastUpdateTime = now;
        needsRefresh = false;
    }
}

void UIHandler::refresh() {
    needsRefresh = true;
}

void UIHandler::handleEncoderInput() {
    EncoderDirection dir = encoder->getDirection();
    
    // Count encoder steps and only react after ENCODER_SENSITIVITY steps
    if (dir == ENCODER_CW) {
        // Reset counter if direction changed from CCW to CW
        if (lastEncoderDirection == ENCODER_CCW) {
            encoderStepCounter = 0;
        }
        
        encoderStepCounter++;
        if (encoderStepCounter >= ENCODER_SENSITIVITY) {
            encoderStepCounter = 0;  // Reset counter
            
            if (currentScreen == UI_SCREEN_FILE_LIST) {
                if (selectedIndex < (int)fileCount - 1) {
                    selectedIndex++;
                    if (selectedIndex >= scrollOffset + maxVisibleItems) {
                        scrollOffset = selectedIndex - maxVisibleItems + 1;
                    }
                    needsRefresh = true;
                }
            } else if (currentScreen == UI_SCREEN_MAIN) {
                // Navigate menu options (0=Files, 1=Status, 2=Info)
                if (selectedIndex < 2) {
                    selectedIndex++;
                    needsRefresh = true;
                }
            }
        }
        lastEncoderDirection = dir;
    } else if (dir == ENCODER_CCW) {
        // Reset counter if direction changed from CW to CCW
        if (lastEncoderDirection == ENCODER_CW) {
            encoderStepCounter = 0;
        }
        
        encoderStepCounter--;
        if (encoderStepCounter <= -ENCODER_SENSITIVITY) {
            encoderStepCounter = 0;  // Reset counter
            
            if (currentScreen == UI_SCREEN_FILE_LIST) {
                if (selectedIndex > 0) {
                    selectedIndex--;
                    if (selectedIndex < scrollOffset) {
                        scrollOffset = selectedIndex;
                    }
                    needsRefresh = true;
                }
            } else if (currentScreen == UI_SCREEN_MAIN) {
                // Navigate menu options (0=Files, 1=Status, 2=Info)
                if (selectedIndex > 0) {
                    selectedIndex--;
                    needsRefresh = true;
                }
            }
        }
        lastEncoderDirection = dir;
    } else if (dir == ENCODER_NONE) {
        // Don't reset counter on ENCODER_NONE - it's normal between steps
        // Only reset if encoder has been stopped for a while (handled by direction change)
    }
    
    // Handle switch press
    // Check for long press first (1 second) to return to main menu
    static bool longPressHandled = false;
    if (encoder->isSwitchHeldLong(1000)) {
        if (currentScreen != UI_SCREEN_MAIN && !longPressHandled) {
            currentScreen = UI_SCREEN_MAIN;
            selectedIndex = 0;
            needsRefresh = true;
            longPressHandled = true;
        }
    }
    
    if (encoder->isSwitchReleased()) {
        // If long press was handled, skip short press action
        if (longPressHandled) {
            longPressHandled = false;
            return;
        }
        if (currentScreen == UI_SCREEN_FILE_LIST) {
            if (sdCard && selectedIndex < (int)fileCount) {
                // Extract filename from fileList
                char* line = fileList;
                for (int i = 0; i < selectedIndex && line; i++) {
                    line = strchr(line, '\n');
                    if (line) line++;
                }
                
                if (line) {
                    char filename[64];
                    int len = 0;
                    bool isDirectory = false;
                    
                    // Extract filename and check for <DIR> marker
                    char* lineStart = line;
                    while (*line && *line != '\n' && *line != '\r' && len < 63) {
                        if (*line == '<') {
                            // Check if it's <DIR>
                            if (strncmp(line, "<DIR>", 5) == 0) {
                                isDirectory = true;
                                break;
                            }
                        }
                        filename[len++] = *line;
                        line++;
                    }
                    // Trim trailing spaces
                    while (len > 0 && filename[len - 1] == ' ') {
                        len--;
                    }
                    filename[len] = 0;
                    
                    if (len > 0) {
                        FAT32* fat32 = sdCard->getFAT32();
                        if (fat32) {
                            if (isDirectory) {
                                // Change directory
                                if (fat32->changeDirectory(filename)) {
                                    selectedIndex = 0;
                                    scrollOffset = 0;
                                    updateFileList();
                                    needsRefresh = true;
                                }
                            } else if (floppy) {
                                // Load file - show loading screen first
                                currentScreen = UI_SCREEN_LOADING;
                                renderLoadingScreen("Loading...");
                                display->update();
                                
                                // Load the file
                                uint8_t* diskImage = floppy->getDiskImage();
                                uint32_t diskSize = floppy->getDiskImageSize();
                                uint32_t bytesRead = 0;
                                
                                bool success = sdCard->loadDiskImage(filename, diskImage, diskSize, &bytesRead);
                                
                                // Show result
                                if (success) {
                                    // Call FloppyEmulator::loadDiskImage() to set initial track to 17
                                    floppy->loadDiskImage(diskImage, bytesRead);
                                    
                                    // Set SD card manager and filename in FloppyEmulator for saving tracks
                                    floppy->setSDCardManager(sdCard);
                                    floppy->setCurrentFileName(filename);
                                    
                                    // Save loaded file name
                                    strncpy(loadedFileName, filename, sizeof(loadedFileName) - 1);
                                    loadedFileName[sizeof(loadedFileName) - 1] = 0;
                                    
                                    renderLoadingScreen("OK");
                                    display->update();
                                    
                                    // Set timeout for OK message (500ms)
                                    loadingEndTime = make_timeout_time_ms(500);
                                    // Don't change screen yet - will be handled in update()
                                } else {
                                    // Clear file name on error
                                    loadedFileName[0] = 0;
                                    
                                    renderLoadingScreen("ERROR");
                                    display->update();
                                    
                                    // Set timeout for ERROR message (1 second)
                                    loadingEndTime = make_timeout_time_ms(1000);
                                    // Don't change screen yet - will be handled in update()
                                }
                                needsRefresh = true;
                            }
                        }
                    }
                }
            }
        } else if (currentScreen == UI_SCREEN_INFO || currentScreen == UI_SCREEN_STATUS) {
            currentScreen = UI_SCREEN_MAIN;
            selectedIndex = 0;  // Reset menu selection
            needsRefresh = true;
        } else if (currentScreen == UI_SCREEN_MAIN) {
            // Enter selected menu option (0=Files, 1=Status, 2=Info)
            if (selectedIndex == 0) {
                currentScreen = UI_SCREEN_FILE_LIST;
                updateFileList();
                selectedIndex = 0;
                scrollOffset = 0;
            } else if (selectedIndex == 1) {
                currentScreen = UI_SCREEN_STATUS;
            } else if (selectedIndex == 2) {
                currentScreen = UI_SCREEN_INFO;
            }
            needsRefresh = true;
        }
    }
}

void UIHandler::updateFileList() {
    if (!sdCard) {
        fileCount = 0;
        return;
    }
    
    FAT32* fat32 = sdCard->getFAT32();
    if (!fat32) {
        fileCount = 0;
        return;
    }
    
    if (fat32->listFiles(fileList, sizeof(fileList), &fileCount)) {
        // File list updated
    } else {
        fileCount = 0;
    }
}

// Icon bitmaps (8x8 pixels, packed format: byte per row, MSB first, row major order)
/*
// SD card icon (8x8) - simplified rectangular card
static const uint8_t iconSD[] = {
    0xFF,  // 11111111 - top edge
    0x81,  // 10000001 - left/right edges
    0xA5,  // 10100101 - with cut corner pattern
    0xA5,  // 10100101
    0xA5,  // 10100101
    0x81,  // 10000001 - left/right edges
    0x81,  // 10000001
    0xFF   // 11111111 - bottom edge
};

// Diskette icon (8x8) - floppy disk with label
static const uint8_t iconDisk[] = {
    0x7E,  // 01111110 - top edge
    0x42,  // 01000010 - sides
    0x5A,  // 01011010 - with label lines
    0x5A,  // 01011010 - label
    0x42,  // 01000010 - sides
    0x42,  // 01000010 - sides
    0x7E,  // 01111110 - bottom edge
    0x3C   // 00111100 - metal shutter
};
*/
// SD card icon (8x8) - simplified rectangular card
// Bitmap format: each byte represents 8 pixels horizontally (MSB first)
static const uint16_t iconSD[] = {
    0x1F00,  // 00011111 - top edge
    0x3500,  // 00101010 - left/right edges
    0x7500,  // 01010101 - with cut corner pattern
    0x7F00,  // 01111111
    0x3F00,  // 00111111
    0x3F00,  // 00111111
    0x7F00,  // 01111111
    0x7F00   // 01111111
};

// Diskette icon (10x10) - floppy disk with label
// Bitmap format: each uint16_t represents one row (10 pixels in bits 15-6, MSB first)
// Bit 15 = leftmost pixel, bit 6 = rightmost pixel
static const uint16_t iconDisk[] = {
    0x0000,  // 0000000000 - top edge (row 0, bits 15-6)
    0x7F80,  // 0111111110 - top edge (row 1, bits 15-6)
    0x7380,  // 0111001110 - sides with notch (row 2)
    0x6180,  // 0110000110 - sides (row 3)
    0x6180,  // 0110000110 - sides (row 4)
    0x7380,  // 0111001110 - sides with notch (row 5)
    0x7F80,  // 0111111110 - bottom edge (row 6)
    0x7380,  // 0111001110 - metal shutter (row 7)
    0x7380,  // 0111001110 - metal shutter (row 8)
    0x0000   // 0000000000 - bottom edge (row 9)
};

void UIHandler::renderStatusBar() {
    // Clear status bar area (upper 16 pixels)
    display->fillRect(0, 0, OLED_WIDTH, STATUS_BAR_HEIGHT, false);
    
    // Draw separator line
    display->drawLine(0, STATUS_SEPARATOR_Y - 1, OLED_WIDTH, STATUS_SEPARATOR_Y - 1, true);
    
    // Show track number on the left
    if (floppy) {
        char trackStr[8];
        snprintf(trackStr, sizeof(trackStr), "T:%d", floppy->getCurrentTrack());
        display->drawString(0, 2, trackStr, true);
    }
    
    // Show loaded file name in the center (if available)
    if (loadedFileName[0] != 0) {
        // Truncate filename if too long to fit
        char displayName[32];
        int nameLen = strlen(loadedFileName);
        int displayLen = nameLen;
        if (nameLen > 15) {
            // Show first 12 chars + "..."
            strncpy(displayName, loadedFileName, 12);
            displayName[12] = 0;
            strcat(displayName, "...");
            displayLen = 15;
        } else {
            strncpy(displayName, loadedFileName, sizeof(displayName) - 1);
            displayName[sizeof(displayName) - 1] = 0;
        }
        // Center the filename (approximately, each char is 6 pixels wide)
        int textWidth = displayLen * 6;
        int xPos = (OLED_WIDTH - textWidth) / 2;
        if (xPos < 30) xPos = 30;  // Don't overlap with track number
        // Make sure it doesn't overlap with icons on the right (20 pixels for icons)
        if (xPos + textWidth > OLED_WIDTH - 22) {
            xPos = OLED_WIDTH - 22 - textWidth;
        }
        display->drawString(xPos, 2, displayName, true);
    }
    
    // Show icons on the right: SD card icon, disk icon if file loaded
    int xPos = OLED_WIDTH - 10;  // Start from right (8 pixels for icon + 2 spacing)
    if (sdCard && sdCard->isInitialized()) {
        display->drawBitmap(xPos, 4, iconSD, 8, 8, true);  // SD card icon at y=4 (centered in 16px bar)
        xPos -= 10;  // Move left for disk icon (8 pixels + 2 spacing)
        if (loadedFileName[0] != 0) {
            display->drawBitmap(xPos-1, 3, iconDisk, 10, 10, !floppy->getGCRTrackCacheDirty());  // Disk icon
        }
    }
}

void UIHandler::renderMainScreen() {
    display->clear();
    
    // Always render status bar first
    renderStatusBar();
    
    // Main content in lower section (starting from STATUS_BAR_HEIGHT)
    display->drawString(0, STATUS_BAR_HEIGHT + 2, "Apple II Floppy", true);
    display->drawString(0, STATUS_BAR_HEIGHT + 10, "Emulator", true);
    
    // Draw menu options with selection marker
    char filesLine[16];
    char statusLine[16];
    char infoLine[16];
    snprintf(filesLine, sizeof(filesLine), "%sFiles", (selectedIndex == 0) ? "> " : "  ");
    snprintf(statusLine, sizeof(statusLine), "%sStatus", (selectedIndex == 1) ? "> " : "  ");
    snprintf(infoLine, sizeof(infoLine), "%sInfo", (selectedIndex == 2) ? "> " : "  ");
    
    display->drawString(0, STATUS_BAR_HEIGHT + 20, filesLine, true);
    display->drawString(0, STATUS_BAR_HEIGHT + 28, statusLine, true);
    display->drawString(0, STATUS_BAR_HEIGHT + 36, infoLine, true);
}

void UIHandler::renderFileListScreen() {
    display->clear();
    
    // Always render status bar first
    renderStatusBar();
    
    // File list content in lower section
    display->drawString(0, STATUS_BAR_HEIGHT + 2, "Files:", true);
    display->drawLine(0, STATUS_BAR_HEIGHT + 10, DISPLAY_WIDTH, STATUS_BAR_HEIGHT + 10, true);
    
    if (fileCount == 0) {
        updateFileList();
    }
    
    if (fileCount == 0) {
        display->drawString(0, STATUS_BAR_HEIGHT + 18, "No files found", true);
    } else {
        char* line = fileList;
        int visibleIndex = 0;
        int lineIndex = 0;
        
        while (line && *line && visibleIndex < maxVisibleItems) {
            char* lineStart = line;
            
            // Find end of line
            while (*line && *line != '\n' && *line != '\r') {
                line++;
            }
            
            // Check if line has content (at least one non-whitespace char)
            bool hasContent = false;
            for (char* p = lineStart; p < line; p++) {
                if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                    hasContent = true;
                    break;
                }
            }
            
            // Only process lines with content
            if (hasContent) {
                if (lineIndex >= scrollOffset) {
                    // Draw this line
                    char displayLine[21];  // 128 pixels / 6 pixels per char = ~21 chars
                    int len = 0;
                    bool isSelected = (lineIndex == selectedIndex);
                    
                    // Don't add '>' prefix - we'll use inverted background instead
                    // Copy line content
                    char* p = lineStart;
                    while (p < line && len < 20) {
                        displayLine[len++] = *p++;
                    }
                    displayLine[len] = 0;
                    
                    int yPos = STATUS_BAR_HEIGHT + 12 + visibleIndex * 8;
                    
                    // Draw inverted background rectangle for selected item
                    if (isSelected) {
                        // Fill rectangle with inverted color (white/true)
                        display->fillRect(0, yPos, DISPLAY_WIDTH, 8, true);
                        // Draw text with inverted color (black/false)
                        display->drawString(0, yPos, displayLine, false);
                    } else {
                        // Draw text with normal color (white/true)
                        display->drawString(0, yPos, displayLine, true);
                    }
                    visibleIndex++;
                }
                lineIndex++;
            }
            
            // Skip newline characters
            if (*line == '\r') line++;
            if (*line == '\n') line++;
        }
    }
}

void UIHandler::renderInfoScreen() {
    display->clear();
    
    // Always render status bar first
    renderStatusBar();
    
    // Info content in lower section
    display->drawString(0, STATUS_BAR_HEIGHT + 2, "Disk Info", true);
    display->drawLine(0, STATUS_BAR_HEIGHT + 10, DISPLAY_WIDTH, STATUS_BAR_HEIGHT + 10, true);
    
    if (floppy) {
        int yPos = STATUS_BAR_HEIGHT + 14;
        
        // File type and name
        const char* fileName = floppy->getCurrentFileName();
        DiskFileType fileType = floppy->getCurrentFileType();
        
        if (fileName && fileName[0] != 0) {
            // Show file type
            char typeStr[32];
            const char* typeName = (fileType == DISK_FILE_TYPE_NIC) ? "NIC" : "DSK";
            snprintf(typeStr, sizeof(typeStr), "Type: %s", typeName);
            display->drawString(0, yPos, typeStr, true);
            yPos += 8;
            
            // Show file name (truncate if too long)
            char nameStr[64];
            const char* nameOnly = strrchr(fileName, '/');
            if (nameOnly) {
                nameOnly++;  // Skip '/'
            } else {
                nameOnly = fileName;
            }
            // Truncate to fit display width (approximately 20 chars)
            int nameLen = strlen(nameOnly);
            if (nameLen > 20) {
                snprintf(nameStr, sizeof(nameStr), "%.17s...", nameOnly);
            } else {
                snprintf(nameStr, sizeof(nameStr), "%s", nameOnly);
            }
            display->drawString(0, yPos, nameStr, true);
            yPos += 8;
        } else {
            display->drawString(0, yPos, "No file loaded", true);
            yPos += 8;
        }
        
        // Current track
        char trackStr[32];
        snprintf(trackStr, sizeof(trackStr), "Track: %d/34", floppy->getCurrentTrack());
        display->drawString(0, yPos, trackStr, true);
        yPos += 8;
        
        // Disk size
        char sizeStr[32];
        snprintf(sizeStr, sizeof(sizeStr), "Size: %u KB", floppy->getDiskImageSize() / 1024);
        display->drawString(0, yPos, sizeStr, true);
        yPos += 8;
        
        // Cache status (if dirty)
        if (floppy->getGCRTrackCacheDirty()) {
            display->drawString(0, yPos, "Modified", true);
        }
    }
}

void UIHandler::renderStatusScreen() {
    display->clear();
    
    // Always render status bar first (already shows basic status)
    renderStatusBar();
    
    // Detailed status in lower section
    display->drawString(0, STATUS_BAR_HEIGHT + 2, "Status Details", true);
    display->drawLine(0, STATUS_BAR_HEIGHT + 10, DISPLAY_WIDTH, STATUS_BAR_HEIGHT + 10, true);
    
    if (floppy) {
        char trackStr[32];
        snprintf(trackStr, sizeof(trackStr), "Track: %d", floppy->getCurrentTrack());
        display->drawString(0, STATUS_BAR_HEIGHT + 14, trackStr, true);
        
        char selStr[32];
        snprintf(selStr, sizeof(selStr), "Selected: %s", floppy->isDriveSelected() ? "Yes" : "No");
        display->drawString(0, STATUS_BAR_HEIGHT + 22, selStr, true);
        
        char track0Str[32];
        snprintf(track0Str, sizeof(track0Str), "Track 0: %s", floppy->isAtTrack0() ? "Yes" : "No");
        display->drawString(0, STATUS_BAR_HEIGHT + 30, track0Str, true);
    }
    
    if (sdCard) {
        char sdStr[32];
        snprintf(sdStr, sizeof(sdStr), "SD Card: %s", sdCard->isInitialized() ? "OK" : "Fail");
        display->drawString(0, STATUS_BAR_HEIGHT + 38, sdStr, true);
    }
}

void UIHandler::renderLoadingScreen(const char* message) {
    display->clear();
    
    // Always render status bar first
    renderStatusBar();
    
    // Loading message in center of content area
    int yPos = STATUS_BAR_HEIGHT + (CONTENT_AREA_HEIGHT / 2) - 4;
    display->drawString(0, yPos, message, true);
}

