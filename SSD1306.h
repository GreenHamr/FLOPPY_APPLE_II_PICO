#ifndef SSD1306_H
#define SSD1306_H

#ifndef USE_MSP1601  // Only compile SSD1306 if MSP1601 is not used

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"
#include "hardware/i2c.h"

// SSD1306/SSD1309 Commands (most commands are compatible)
#define SSD1306_SETCONTRAST 0x81
#define SSD1306_DISPLAYALLON_RESUME 0xA4
#define SSD1306_DISPLAYALLON 0xA5
#define SSD1306_NORMALDISPLAY 0xA6
#define SSD1306_INVERTDISPLAY 0xA7
#define SSD1306_DISPLAYOFF 0xAE
#define SSD1306_DISPLAYON 0xAF
#define SSD1306_SETDISPLAYOFFSET 0xD3
#define SSD1306_SETCOMPINS 0xDA
#define SSD1306_SETVCOMDETECT 0xDB
#define SSD1306_SETDISPLAYCLOCKDIV 0xD5
#define SSD1306_SETPRECHARGE 0xD9
#define SSD1306_SETMULTIPLEX 0xA8
#define SSD1306_SETLOWCOLUMN 0x00
#define SSD1306_SETHIGHCOLUMN 0x10
#define SSD1306_SETSTARTLINE 0x40
#define SSD1306_MEMORYMODE 0x20
#define SSD1306_COLUMNADDR 0x21
#define SSD1306_PAGEADDR 0x22
#define SSD1306_COMSCANINC 0xC0
#define SSD1306_COMSCANDEC 0xC8
#define SSD1306_SEGREMAP 0xA0
#define SSD1306_CHARGEPUMP 0x8D
#define SSD1306_EXTERNALVCC 0x1
#define SSD1306_SWITCHCAPVCC 0x2

// SSD1309 specific commands (if needed)
#define SSD1309_SETPRECHARGE2 0x96  // SSD1309 specific
#define SSD1309_SETVCOMH 0xDB       // Same as SSD1306_SETVCOMDETECT but different usage

class SSD1306 {
private:
    i2c_inst_t* i2c;
    uint8_t address;
    uint8_t resetPin;  // Reset pin (0xFF = not used)
    uint8_t buffer[128 * 64 / 8];  // Display buffer (128x64 for both SSD1306 and SSD1309)
    
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t* data, uint32_t len);
    
public:
    SSD1306(i2c_inst_t* i2cInstance, uint8_t i2cAddress, uint8_t resetPin = 0xFF);
    ~SSD1306();
    
    bool init();
    void clear();
    void display();
    void setPixel(int x, int y, bool color);
    void drawChar(int x, int y, char c, bool color = true);
    void drawString(int x, int y, const char* str, bool color = true);
    void drawLine(int x0, int y0, int x1, int y1, bool color = true);
    void drawRect(int x, int y, int w, int h, bool color = true, bool filled = false);
    void fillRect(int x, int y, int w, int h, bool color = true);
    void drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, bool color = true);
};

#endif // USE_MSP1601

#endif // SSD1306_H

