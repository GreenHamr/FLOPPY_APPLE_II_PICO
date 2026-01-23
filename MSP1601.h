#ifndef MSP1601_H
#define MSP1601_H

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"
#include "hardware/spi.h"

// SSD1283A/MSP1601 Commands
#define SSD1283A_NOP             0x00
#define SSD1283A_SOFT_RESET      0x01
#define SSD1283A_ENTRY_MODE      0x03
#define SSD1283A_DISPLAY_OFF     0x28
#define SSD1283A_DISPLAY_ON      0x29
#define SSD1283A_COLUMN_ADDR     0x2A
#define SSD1283A_PAGE_ADDR       0x2B
#define SSD1283A_MEMORY_WRITE    0x2C
#define SSD1283A_MEMORY_READ     0x2E
#define SSD1283A_PIXEL_FORMAT    0x3A
#define SSD1283A_WRITE_MEM_CONT  0x3C
#define SSD1283A_READ_MEM_CONT   0x3E
#define SSD1283A_SET_TEAR_SCAN   0x44
#define SSD1283A_GET_SCANLINE    0x45
#define SSD1283A_WRITE_DIS_BRIGHT 0x51
#define SSD1283A_READ_DIS_BRIGHT  0x52
#define SSD1283A_WRITE_CTRL_DISP  0x53
#define SSD1283A_READ_CTRL_DISP  0x54
#define SSD1283A_WRITE_CABC      0x55
#define SSD1283A_READ_CABC       0x56
#define SSD1283A_WRITE_CABC_MIN  0x5E
#define SSD1283A_READ_CABC_MIN   0x5F
#define SSD1283A_READ_ID1        0xDA
#define SSD1283A_READ_ID2        0xDB
#define SSD1283A_READ_ID3        0xDC

// Entry mode bits
#define SSD1283A_ENTRY_NORMAL    0x00
#define SSD1283A_ENTRY_INVERT    0x01

class MSP1601 {
private:
    spi_inst_t* spi;
    uint8_t csPin;
    uint8_t dcPin;
    uint8_t rstPin;
    uint8_t ledPin;
    uint16_t* buffer;  // RGB565 buffer (16-bit per pixel)
    
    void sendCommand(uint8_t cmd);
    void sendData(uint8_t* data, uint32_t len);
    void sendData16(uint16_t* data, uint32_t len);
    void setCS(bool state);
    void setDC(bool state);  // false = command, true = data
    void setRST(bool state);
    void setLED(bool state);
    
public:
    MSP1601(spi_inst_t* spiInstance, uint8_t cs, uint8_t dc, uint8_t rst, uint8_t led);
    ~MSP1601();
    
    bool init();
    void clear();
    void display();
    void setPixel(int x, int y, uint16_t color);  // RGB565 color
    void setPixelBW(int x, int y, bool color);   // Black/White for compatibility
    void drawChar(int x, int y, char c, uint16_t color = 0xFFFF);
    void drawString(int x, int y, const char* str, uint16_t color = 0xFFFF);
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color = 0xFFFF);
    void drawRect(int x, int y, int w, int h, uint16_t color = 0xFFFF, bool filled = false);
    void fillRect(int x, int y, int w, int h, uint16_t color = 0xFFFF);
    void drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, uint16_t color = 0xFFFF);
    
    // RGB565 color helpers
    static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b);
    static uint16_t rgb565_white() { return 0xFFFF; }
    static uint16_t rgb565_black() { return 0x0000; }
};

#endif // MSP1601_H

