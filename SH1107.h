#ifndef SH1107_H
#define SH1107_H

#ifndef USE_MSP1601  // Only compile SH1107 if MSP1601 is not used

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"
#include "hardware/i2c.h"

// SH1107 Commands (similar to SSD1306 but with some differences)
#define SH1107_SETCONTRAST 0x81
#define SH1107_DISPLAYALLON_RESUME 0xA4
#define SH1107_DISPLAYALLON 0xA5
#define SH1107_NORMALDISPLAY 0xA6
#define SH1107_INVERTDISPLAY 0xA7
#define SH1107_DISPLAYOFF 0xAE
#define SH1107_DISPLAYON 0xAF
#define SH1107_SETDISPLAYOFFSET 0xD3
#define SH1107_SETCOMPINS 0xDA
#define SH1107_SETVCOMDETECT 0xDB
#define SH1107_SETDISPLAYCLOCKDIV 0xD5
#define SH1107_SETPRECHARGE 0xD9
#define SH1107_SETMULTIPLEX 0xA8
#define SH1107_SETLOWCOLUMN 0x00
#define SH1107_SETHIGHCOLUMN 0x10
#define SH1107_SETSTARTLINE 0x40
#define SH1107_MEMORYMODE 0x20
#define SH1107_COLUMNADDR 0x21
#define SH1107_PAGEADDR 0x22
#define SH1107_COMSCANINC 0xC0
#define SH1107_COMSCANDEC 0xC8
#define SH1107_SEGREMAP 0xA0
#define SH1107_CHARGEPUMP 0x8D
#define SH1107_EXTERNALVCC 0x1
#define SH1107_SWITCHCAPVCC 0x2

// SH1107 specific commands
#define SH1107_SETPAGESTART 0xB0
#define SH1107_SETCOLUMNADDRLOW 0x00
#define SH1107_SETCOLUMNADDRHIGH 0x10

class SH1107 {
private:
    i2c_inst_t* i2c;
    uint8_t address;
    uint8_t resetPin;  // Reset pin (0xFF = not used)
    uint8_t buffer[128 * 128 / 8];  // Display buffer (128x128)
    
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t* data, uint32_t len);
    
public:
    SH1107(i2c_inst_t* i2cInstance, uint8_t i2cAddress, uint8_t resetPin = 0xFF);
    ~SH1107();
    
    bool init();
    void clear();
    void display();
    void setPixel(int x, int y, bool color);
    void drawChar(int x, int y, char c, bool color = true);
    void drawString(int x, int y, const char* str, bool color = true);
    void drawLine(int x0, int y0, int x1, int y1, bool color = true);
    void drawRect(int x, int y, int w, int h, bool color = true, bool filled = false);
    void fillRect(int x, int y, int w, int h, bool color = true);
    void drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color = true);
};

#endif // USE_MSP1601

#endif // SH1107_H

