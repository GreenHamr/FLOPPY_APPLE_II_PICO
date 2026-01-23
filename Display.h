#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "PinConfig.h"

#ifdef USE_MSP1601
    #include "MSP1601.h"
    typedef MSP1601 DisplayClass;
    #define DISPLAY_WIDTH LCD_WIDTH
    #define DISPLAY_HEIGHT LCD_HEIGHT
#else
    #include "SSD1306.h"
    typedef SSD1306 DisplayClass;
    #define DISPLAY_WIDTH OLED_WIDTH
    #define DISPLAY_HEIGHT OLED_HEIGHT
#endif

// Unified display interface wrapper
class Display {
private:
    DisplayClass* display;
    
public:
    Display(DisplayClass* disp);
    ~Display();
    
    bool init();
    void clear();
    void update();  // Changed from display() to avoid name conflict
    void setPixel(int x, int y, bool color);
    void drawChar(int x, int y, char c, bool color = true);
    void drawString(int x, int y, const char* str, bool color = true);
    void drawLine(int x0, int y0, int x1, int y1, bool color = true);
    void drawRect(int x, int y, int w, int h, bool color = true, bool filled = false);
    void fillRect(int x, int y, int w, int h, bool color = true);
    void drawBitmap(int x, int y, const uint8_t* bitmap, int w, int h, bool color = true);
    
    DisplayClass* getDisplay() { return display; }
};

#endif // DISPLAY_H

