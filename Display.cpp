#include "Display.h"

Display::Display(DisplayClass* disp) {
    display = disp;
}

Display::~Display() {
    // Don't delete display, it's managed externally
}

bool Display::init() {
    return display->init();
}

void Display::clear() {
    display->clear();
}

void Display::update() {
    display->display();
}

void Display::setPixel(int x, int y, bool color) {
#ifdef USE_MSP1601
    display->setPixelBW(x, y, color);
#else
    display->setPixel(x, y, color);
#endif
}

void Display::drawChar(int x, int y, char c, bool color) {
#ifdef USE_MSP1601
    display->drawChar(x, y, c, color ? 0xFFFF : 0x0000);
#else
    display->drawChar(x, y, c, color);
#endif
}

void Display::drawString(int x, int y, const char* str, bool color) {
#ifdef USE_MSP1601
    display->drawString(x, y, str, color ? 0xFFFF : 0x0000);
#else
    display->drawString(x, y, str, color);
#endif
}

void Display::drawLine(int x0, int y0, int x1, int y1, bool color) {
#ifdef USE_MSP1601
    display->drawLine(x0, y0, x1, y1, color ? 0xFFFF : 0x0000);
#else
    display->drawLine(x0, y0, x1, y1, color);
#endif
}

void Display::drawRect(int x, int y, int w, int h, bool color, bool filled) {
#ifdef USE_MSP1601
    display->drawRect(x, y, w, h, color ? 0xFFFF : 0x0000, filled);
#else
    display->drawRect(x, y, w, h, color, filled);
#endif
}

void Display::fillRect(int x, int y, int w, int h, bool color) {
#ifdef USE_MSP1601
    display->fillRect(x, y, w, h, color ? 0xFFFF : 0x0000);
#else
    display->fillRect(x, y, w, h, color);
#endif
}

void Display::drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, bool color) {
#ifdef USE_MSP1601
    display->drawBitmap(x, y, bitmap, w, h, color ? 0xFFFF : 0x0000);
#else
    display->drawBitmap(x, y, bitmap, w, h, color);
#endif
}

