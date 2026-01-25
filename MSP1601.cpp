#include "MSP1601.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
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

MSP1601::MSP1601(spi_inst_t* spiInstance, uint8_t cs, uint8_t dc, uint8_t rst, uint8_t led) {
    spi = spiInstance;
    csPin = cs;
    dcPin = dc;
    rstPin = rst;
    ledPin = led;
    buffer = new uint16_t[128 * 128];  // 1.6" display is 128x128
}

MSP1601::~MSP1601() {
    if (buffer) {
        delete[] buffer;
    }
}

void MSP1601::setCS(bool state) {
    gpio_put(csPin, !state);  // CS is active LOW
}

void MSP1601::setDC(bool state) {
    gpio_put(dcPin, state);  // HIGH = data, LOW = command
}

void MSP1601::setRST(bool state) {
    gpio_put(rstPin, state);
}

void MSP1601::setLED(bool state) {
    if (ledPin != 0xFF) {
        gpio_put(ledPin, state);
    }
}

void MSP1601::sendCommand(uint8_t cmd) {
    setCS(true);
    setDC(false);  // Command mode
    uint8_t data = cmd;
    spi_write_blocking(spi, &data, 1);
    setCS(false);
}

void MSP1601::sendData(uint8_t* data, uint32_t len) {
    setCS(true);
    setDC(true);  // Data mode
    spi_write_blocking(spi, data, len);
    setCS(false);
}

void MSP1601::sendData16(uint16_t* data, uint32_t len) {
    setCS(true);
    setDC(true);  // Data mode
    // Convert 16-bit to 8-bit (MSB first)
    for (uint32_t i = 0; i < len; i++) {
        uint8_t bytes[2];
        bytes[0] = (data[i] >> 8) & 0xFF;  // MSB
        bytes[1] = data[i] & 0xFF;         // LSB
        spi_write_blocking(spi, bytes, 2);
    }
    setCS(false);
}

bool MSP1601::init() {
    // Initialize GPIO pins
    gpio_init(csPin);
    gpio_set_dir(csPin, GPIO_OUT);
    gpio_put(csPin, 1);  // CS inactive (HIGH)
    
    gpio_init(dcPin);
    gpio_set_dir(dcPin, GPIO_OUT);
    gpio_put(dcPin, 0);  // Command mode
    
    gpio_init(rstPin);
    gpio_set_dir(rstPin, GPIO_OUT);
    gpio_put(rstPin, 1);  // Reset inactive
    
    if (ledPin != 0xFF) {
        gpio_init(ledPin);
        gpio_set_dir(ledPin, GPIO_OUT);
        gpio_put(ledPin, 1);  // LED on
    }
    
    // Hardware reset
    setRST(false);
    sleep_ms(10);
    setRST(true);
    sleep_ms(10);
    
    // Initialize SPI (should be done in main, but set format here)
    spi_set_format(spi, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    
    // SSD1283A initialization sequence
    sendCommand(SSD1283A_SOFT_RESET);
    sleep_ms(10);
    
    sendCommand(SSD1283A_ENTRY_MODE);
    uint8_t entryMode = 0x0000;  // Normal entry mode
    sendData(&entryMode, 1);
    
    sendCommand(SSD1283A_PIXEL_FORMAT);
    uint8_t pixelFormat = 0x55;  // 16-bit RGB565
    sendData(&pixelFormat, 1);
    
    // Set column address (0 to 127)
    sendCommand(SSD1283A_COLUMN_ADDR);
    uint8_t colAddr[4] = {0, 0, 0, 127};
    sendData(colAddr, 4);
    
    // Set page address (0 to 127)
    sendCommand(SSD1283A_PAGE_ADDR);
    uint8_t pageAddr[4] = {0, 0, 0, 127};
    sendData(pageAddr, 4);
    
    // Turn on display
    sendCommand(SSD1283A_DISPLAY_ON);
    
    clear();
    display();
    
    return true;
}

void MSP1601::clear() {
    memset(buffer, 0, 128 * 128 * sizeof(uint16_t));
}

void MSP1601::display() {
    // Set column address (0 to 127)
    sendCommand(SSD1283A_COLUMN_ADDR);
    uint8_t colAddr[4] = {0, 0, 0, 127};
    sendData(colAddr, 4);
    
    // Set page address (0 to 127)
    sendCommand(SSD1283A_PAGE_ADDR);
    uint8_t pageAddr[4] = {0, 0, 0, 127};
    sendData(pageAddr, 4);
    
    // Write memory
    sendCommand(SSD1283A_MEMORY_WRITE);
    sendData16(buffer, 128 * 128);
}

void MSP1601::setPixel(int x, int y, uint16_t color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 128) return;
    buffer[y * 128 + x] = color;
}

void MSP1601::setPixelBW(int x, int y, bool color) {
    setPixel(x, y, color ? 0xFFFF : 0x0000);
}

void MSP1601::drawChar(int x, int y, char c, uint16_t color) {
    if (c < 32 || c > 126) c = 32;
    
    const uint8_t* fontData = font5x7[c - 32];
    
    for (int col = 0; col < 5; col++) {
        uint8_t colData = fontData[col];
        for (int row = 0; row < 7; row++) {
            if (colData & (1 << row)) {
                setPixel(x + col, y + row, color);
            } else {
                setPixel(x + col, y + row, 0x0000);
            }
        }
    }
}

void MSP1601::drawString(int x, int y, const char* str, uint16_t color) {
    int pos = x;
    while (*str && pos < 128 - 5) {
        drawChar(pos, y, *str++, color);
        pos += 6;  // 5 pixels + 1 space
    }
}

void MSP1601::drawLine(int x0, int y0, int x1, int y1, uint16_t color) {
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

void MSP1601::drawRect(int x, int y, int w, int h, uint16_t color, bool filled) {
    if (filled) {
        fillRect(x, y, w, h, color);
    } else {
        drawLine(x, y, x + w - 1, y, color);
        drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
        drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
        drawLine(x, y + h - 1, x, y, color);
    }
}

void MSP1601::fillRect(int x, int y, int w, int h, uint16_t color) {
    for (int i = 0; i < w; i++) {
        for (int j = 0; j < h; j++) {
            setPixel(x + i, y + j, color);
        }
    }
}

void MSP1601::drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, uint16_t color) {
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
                setPixel(x + i, y + j, 0x0000);
            }
        }
    }
}

uint16_t MSP1601::rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

