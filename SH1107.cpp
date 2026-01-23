#include "SH1107.h"

#ifndef USE_MSP1601  // Only compile SH1107 if MSP1601 is not used

#include "hardware/gpio.h"
#include "pico/time.h"
#include <string.h>
#include <stdlib.h>

// 5x7 font (same as SSD1306 for compatibility)
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

SH1107::SH1107(i2c_inst_t* i2cInstance, uint8_t i2cAddress, uint8_t resetPin) {
    i2c = i2cInstance;
    address = i2cAddress;
    this->resetPin = resetPin;
    memset(buffer, 0, sizeof(buffer));
}

SH1107::~SH1107() {
}

void SH1107::sendCommand(uint8_t cmd) {
    uint8_t data[2] = {0x00, cmd};  // Co = 0, D/C = 0 (command)
    i2c_write_timeout_us(i2c, address, data, 2, false, 5000);  // 5ms timeout
}

void SH1107::sendData(uint8_t* data, uint32_t len) {
    // Use continuous data mode for better performance
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

bool SH1107::init() {
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
        sleep_ms(50);
        gpio_put(resetPin, 1);  // Reset inactive (HIGH)
        sleep_ms(100);
    }
    
    // SH1107 initialization sequence (128x128)
    sendCommand(SH1107_DISPLAYOFF);
    sleep_ms(10);
    
    sendCommand(SH1107_SETDISPLAYCLOCKDIV);
    sendCommand(0x80);  // Default clock divider
    
    sendCommand(SH1107_SETMULTIPLEX);
    sendCommand(127);  // 128 rows - 1 (0-127)
    
    sendCommand(SH1107_SETDISPLAYOFFSET);
    sendCommand(0x00);  // No offset
    
    sendCommand(SH1107_SETSTARTLINE | 0x0);  // Start at line 0
    
    sendCommand(SH1107_CHARGEPUMP);
    sendCommand(0x14);  // Enable charge pump (internal VCC)
    sleep_ms(10);
    
    sendCommand(SH1107_MEMORYMODE);
    sendCommand(0x02);  // Page addressing mode (SH1107 uses 0x02 for page mode)
    
    sendCommand(SH1107_SEGREMAP | 0x1);  // Remap segments (flip horizontally)
    sendCommand(SH1107_COMSCANDEC);      // Scan from COM127 to COM0 (bottom to top)
    
    sendCommand(SH1107_SETCOMPINS);
    sendCommand(0x12);  // COM pin configuration: sequential, disable left/right remap (for 128px)
    
    sendCommand(SH1107_SETCONTRAST);
    sendCommand(0x80);  // Contrast value (50% - adjust as needed)
    
    sendCommand(SH1107_SETPRECHARGE);
    sendCommand(0xF1);  // Precharge period
    
    sendCommand(SH1107_SETVCOMDETECT);
    sendCommand(0x40);  // VCOMH deselect level
    
    sendCommand(SH1107_DISPLAYALLON_RESUME);  // Resume to RAM content
    sendCommand(SH1107_NORMALDISPLAY);        // Normal (not inverted)
    
    sleep_ms(100);
    sendCommand(SH1107_DISPLAYON);
    sleep_ms(10);
    
    clear();
    display();
    
    return true;
}

void SH1107::clear() {
    memset(buffer, 0, sizeof(buffer));
}

void SH1107::display() {
    // SH1107 uses page addressing mode - must send page by page
    for (uint8_t page = 0; page < 16; page++) {  // 128 pixels / 8 = 16 pages
        // Set page address
        sendCommand(SH1107_SETPAGESTART + page);
        
        // Set column address (SH1107 uses different column addressing)
        // Low nibble of column address
        sendCommand(SH1107_SETCOLUMNADDRLOW | 0x00);
        // High nibble of column address  
        sendCommand(SH1107_SETCOLUMNADDRHIGH | 0x00);
        
        // Send one page (128 bytes per page)
        sendData(&buffer[page * 128], 128);
    }
}

void SH1107::setPixel(int x, int y, bool color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 128) return;
    
    int page = y / 8;
    int bit = y % 8;
    
    if (color) {
        buffer[page * 128 + x] |= (1 << bit);
    } else {
        buffer[page * 128 + x] &= ~(1 << bit);
    }
}

void SH1107::drawChar(int x, int y, char c, bool color) {
    if (c < 32 || c > 126) c = 32;
    
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

void SH1107::drawString(int x, int y, const char* str, bool color) {
    int pos = x;
    while (*str && pos < 128 - 5) {
        drawChar(pos, y, *str++, color);
        pos += 6;
    }
}

void SH1107::drawLine(int x0, int y0, int x1, int y1, bool color) {
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

void SH1107::drawRect(int x, int y, int w, int h, bool color, bool filled) {
    if (filled) {
        fillRect(x, y, w, h, color);
    } else {
        drawLine(x, y, x + w - 1, y, color);
        drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
        drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
        drawLine(x, y + h - 1, x, y, color);
    }
}

void SH1107::fillRect(int x, int y, int w, int h, bool color) {
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            setPixel(x + i, y + j, color);
        }
    }
}

void SH1107::drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, bool color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int byteIndex = (j * w + i) / 8;
            int bitIndex = (j * w + i) % 8;
            if (bitmap[byteIndex] & (1 << bitIndex)) {
                setPixel(x + i, y + j, color);
            } else {
                setPixel(x + i, y + j, !color);
            }
        }
    }
}

#endif // USE_MSP1601

