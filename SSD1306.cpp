#include "SSD1306.h"

#ifndef USE_MSP1601  // Only compile SSD1306 if MSP1601 is not used

#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>
#include <stdlib.h>

// 5x7 font (simplified, only ASCII 32-126)
static const uint8_t font5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // !
    {0x00, 0x07, 0x00, 0x07, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50}, // &
    {0x00, 0x00, 0x07, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00}, // ;
    {0x08, 0x14, 0x22, 0x41, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14}, // =
    {0x00, 0x41, 0x22, 0x14, 0x08}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06}, // ?
    {0x32, 0x49, 0x59, 0x51, 0x3E}, // @
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // [
    {0x02, 0x04, 0x08, 0x10, 0x20}, // backslash
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ]
    {0x04, 0x02, 0x01, 0x02, 0x04}, // ^
    {0x40, 0x40, 0x40, 0x40, 0x40}, // _
    {0x00, 0x01, 0x02, 0x04, 0x00}, // `
    {0x20, 0x54, 0x54, 0x54, 0x78}, // a
    {0x7F, 0x48, 0x44, 0x44, 0x38}, // b
    {0x38, 0x44, 0x44, 0x44, 0x20}, // c
    {0x38, 0x44, 0x44, 0x48, 0x7F}, // d
    {0x38, 0x54, 0x54, 0x54, 0x18}, // e
    {0x08, 0x7E, 0x09, 0x01, 0x02}, // f
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // g
    {0x7F, 0x08, 0x04, 0x04, 0x78}, // h
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // i
    {0x20, 0x40, 0x44, 0x3D, 0x00}, // j
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // k
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // l
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // m
    {0x7C, 0x08, 0x04, 0x04, 0x78}, // n
    {0x38, 0x44, 0x44, 0x44, 0x38}, // o
    {0x7C, 0x14, 0x14, 0x14, 0x08}, // p
    {0x08, 0x14, 0x14, 0x18, 0x7C}, // q
    {0x7C, 0x08, 0x04, 0x04, 0x08}, // r
    {0x48, 0x54, 0x54, 0x54, 0x20}, // s
    {0x04, 0x3F, 0x44, 0x40, 0x20}, // t
    {0x3C, 0x40, 0x40, 0x20, 0x7C}, // u
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // v
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // w
    {0x44, 0x28, 0x10, 0x28, 0x44}, // x
    {0x0C, 0x50, 0x50, 0x50, 0x3C}, // y
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // z
    {0x00, 0x08, 0x36, 0x41, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00}, // }
    {0x08, 0x08, 0x2A, 0x1C, 0x08}, // ~
};

SSD1306::SSD1306(i2c_inst_t* i2cInstance, uint8_t i2cAddress, uint8_t resetPin) {
    i2c = i2cInstance;
    address = i2cAddress;
    this->resetPin = resetPin;
    memset(buffer, 0, sizeof(buffer));
}

SSD1306::~SSD1306() {
}

void SSD1306::sendCommand(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd};  // Co = 0, D/C = 0 (command)
    i2c_write_timeout_us(i2c, address, data, 2, false, 5000);  // 5ms timeout
}

void SSD1306::sendData(uint8_t* data, uint32_t len) {
    // Use continuous data mode for better performance
    // Co = 0, D/C = 1, then send data in chunks (I2C has practical limits)
    const uint32_t chunkSize = 128;  // Send up to 128 bytes at a time
    uint8_t chunk[chunkSize + 1];
    chunk[0] = 0x40;  // Co = 0, D/C = 1 (data, continuous mode)
    
    for (uint32_t i = 0; i < len; i += chunkSize) {
        uint32_t chunkLen = (len - i < chunkSize) ? (len - i) : chunkSize;
        // Copy data to chunk
        for (uint32_t j = 0; j < chunkLen; j++) {
            chunk[j + 1] = data[i + j];
        }
        // Send chunk with timeout
        i2c_write_timeout_us(i2c, address, chunk, chunkLen + 1, false, 20000);  // 20ms timeout per chunk
    }
}

bool SSD1306::init() {
    // Initialize I2C (should be done in main)
    
    // Initialize reset pin if used
    if (resetPin != 0xFF) {
        gpio_init(resetPin);
        gpio_set_dir(resetPin, GPIO_OUT);
        gpio_put(resetPin, 1);  // Reset inactive (HIGH)
        sleep_ms(10);
    }
    
    // Hardware reset if reset pin is available
    if (resetPin != 0xFF) {
        gpio_put(resetPin, 0);  // Reset active (LOW)
        sleep_ms(50);  // Longer reset pulse
        gpio_put(resetPin, 1);  // Reset inactive (HIGH)
        sleep_ms(100);  // Wait after reset
    }
    
    // Send initialization sequence
    
#ifdef USE_SSD1309
    // SSD1309 initialization sequence (2.42" display)
    sendCommand(SSD1306_DISPLAYOFF);
    sleep_ms(10);  // Delay after display off
    
    sendCommand(SSD1306_SETDISPLAYCLOCKDIV);
    sendCommand(0x80);
    sendCommand(SSD1306_SETMULTIPLEX);
    sendCommand(OLED_HEIGHT - 1);
    sendCommand(SSD1306_SETDISPLAYOFFSET);
    sendCommand(0x00);
    sendCommand(SSD1306_SETSTARTLINE | 0x0);
    
    sendCommand(SSD1306_CHARGEPUMP);
    sendCommand(0x14);  // Enable charge pump
    sleep_ms(10);  // Delay after charge pump enable
    
    sendCommand(SSD1306_MEMORYMODE);
    sendCommand(0x00);  // Horizontal addressing mode
    sendCommand(SSD1306_SEGREMAP | 0x1);
    sendCommand(SSD1306_COMSCANDEC);
    sendCommand(SSD1306_SETCOMPINS);
    sendCommand(0x12);  // Alternative COM pin configuration for 64px height
    sendCommand(SSD1306_SETCONTRAST);
    sendCommand(0xFF);  // Higher contrast for SSD1309
    sendCommand(SSD1306_SETPRECHARGE);
    sendCommand(0xF1);
    sendCommand(SSD1306_SETVCOMDETECT);
    sendCommand(0x40);
    sendCommand(SSD1306_DISPLAYALLON_RESUME);
    sendCommand(SSD1306_NORMALDISPLAY);
    
    sleep_ms(100);  // Delay before display on
    sendCommand(SSD1306_DISPLAYON);
    sleep_ms(10);  // Delay after display on
#else
    // SSD1306 initialization sequence (original)
    sendCommand(SSD1306_DISPLAYOFF);
    sendCommand(SSD1306_SETDISPLAYCLOCKDIV);
    sendCommand(0x80);
    sendCommand(SSD1306_SETMULTIPLEX);
    sendCommand(OLED_HEIGHT - 1);
    sendCommand(SSD1306_SETDISPLAYOFFSET);
    sendCommand(0x00);
    sendCommand(SSD1306_SETSTARTLINE | 0x0);
    sendCommand(SSD1306_CHARGEPUMP);
    sendCommand(0x14);
    sendCommand(SSD1306_MEMORYMODE);
    sendCommand(0x00);
    sendCommand(SSD1306_SEGREMAP | 0x1);
    sendCommand(SSD1306_COMSCANDEC);
    sendCommand(SSD1306_SETCOMPINS);
    sendCommand(0x12);
    sendCommand(SSD1306_SETCONTRAST);
    sendCommand(0xCF);
    sendCommand(SSD1306_SETPRECHARGE);
    sendCommand(0xF1);
    sendCommand(SSD1306_SETVCOMDETECT);
    sendCommand(0x40);
    sendCommand(SSD1306_DISPLAYALLON_RESUME);
    sendCommand(SSD1306_NORMALDISPLAY);
    sendCommand(SSD1306_DISPLAYON);
#endif
    
    clear();
    display();
    
    return true;
}

void SSD1306::clear() {
    memset(buffer, 0, sizeof(buffer));
}

void SSD1306::display() {
    sendCommand(SSD1306_COLUMNADDR);
    sendCommand(0);
    sendCommand(OLED_WIDTH - 1);
    sendCommand(SSD1306_PAGEADDR);
    sendCommand(0);
    sendCommand((OLED_HEIGHT / 8) - 1);
    
    sendData(buffer, sizeof(buffer));
}

void SSD1306::setPixel(int x, int y, bool color) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    
    int page = y / 8;
    int bit = y % 8;
    
    if (color) {
        buffer[page * OLED_WIDTH + x] |= (1 << bit);
    } else {
        buffer[page * OLED_WIDTH + x] &= ~(1 << bit);
    }
}

void SSD1306::drawChar(int x, int y, char c, bool color) {
    if (c < 32 || c > 126) c = 32;  // Use space for invalid chars
    
    const uint8_t* fontData = font5x7[c - 32];
    
    for (int col = 0; col < 5; col++) {
        uint8_t colData = fontData[col];
        for (int row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                setPixel(x + col, y + row, color);
            }
        }
    }
}

void SSD1306::drawString(int x, int y, const char* str, bool color) {
    int pos = x;
    while (*str && pos < OLED_WIDTH - 5) {
        drawChar(pos, y, *str++, color);
        pos += 6;  // 5 pixels + 1 space
    }
}

void SSD1306::drawLine(int x0, int y0, int x1, int y1, bool color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    
    int x = x0, y = y0;
    
    while (true) {
        setPixel(x, y, color);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

void SSD1306::drawRect(int x, int y, int w, int h, bool color, bool filled) {
    if (filled) {
        fillRect(x, y, w, h, color);
    } else {
        drawLine(x, y, x + w - 1, y, color);
        drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
        drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
        drawLine(x, y + h - 1, x, y, color);
    }
}

void SSD1306::fillRect(int x, int y, int w, int h, bool color) {
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            setPixel(x + i, y + j, color);
        }
    }
}

void SSD1306::drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color) {
    // Each uint16_t represents one row (up to 16 pixels)
    // Bit format: MSB (bit 15) is leftmost pixel, LSB (bit 0) is rightmost pixel
    // For width < 16, we use the leftmost 'w' bits
    
    for (int j = 0; j < h; j++) {
        // Get the row data (one uint16_t per row)
        uint16_t rowData = bitmap[j];
        
        for (int i = 0; i < w; i++) {
            // Calculate bit index: MSB first (bit 15 is pixel 0, bit 14 is pixel 1, etc.)
            // For width w, we use bits 15 down to (16-w)
            int bitIndex = 15 - i;
            
            // Check if bit is set
            if (rowData & (1 << bitIndex)) {
                setPixel(x + i, y + j, color);
            } else {
                setPixel(x + i, y + j, !color);
            }
        }
    }
}

#endif // USE_MSP1601

