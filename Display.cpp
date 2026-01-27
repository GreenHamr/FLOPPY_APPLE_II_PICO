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

void Display::drawRectEx(int x, int y, int w, int h, int lineWidth, bool rounded, bool color) {
    if (lineWidth <= 0 || w <= 0 || h <= 0) return;
    
    int radius = 0;
    if (rounded) {
        // Calculate radius based on smaller dimension
        radius = (w < h ? w : h) / 4;
        if (radius > 8) radius = 8;  // Limit radius to reasonable size
        if (radius < 2) radius = 2;  // Minimum radius for visible rounding
    }
    
    // Draw rectangle with specified line width
    for (int layer = 0; layer < lineWidth; layer++) {
        int offset = layer;
        int innerX = x + offset;
        int innerY = y + offset;
        int innerW = w - 2 * offset;
        int innerH = h - 2 * offset;
        
        if (innerW <= 0 || innerH <= 0) break;
        
        int currentRadius = radius - offset;
        if (currentRadius < 0) currentRadius = 0;
        if (currentRadius > innerW / 2) currentRadius = innerW / 2;
        if (currentRadius > innerH / 2) currentRadius = innerH / 2;
        
        if (rounded && currentRadius > 0) {
            // Draw rounded rectangle
            // Top edge (excluding corners)
            for (int px = innerX + currentRadius; px < innerX + innerW - currentRadius; px++) {
                setPixel(px, innerY, color);
            }
            
            // Bottom edge (excluding corners)
            for (int px = innerX + currentRadius; px < innerX + innerW - currentRadius; px++) {
                setPixel(px, innerY + innerH - 1, color);
            }
            
            // Left edge (excluding corners)
            for (int py = innerY + currentRadius; py < innerY + innerH - currentRadius; py++) {
                setPixel(innerX, py, color);
            }
            
            // Right edge (excluding corners)
            for (int py = innerY + currentRadius; py < innerY + innerH - currentRadius; py++) {
                setPixel(innerX + innerW - 1, py, color);
            }
            
            // Draw rounded corners using simple circle outline
            int r = currentRadius;
            for (int dy = -r; dy <= r; dy++) {
                for (int dx = -r; dx <= r; dx++) {
                    int distSq = dx*dx + dy*dy;
                    int outerSq = r * r;
                    int innerSq = (r - 1) * (r - 1);
                    
                    // Check if point is on the circle outline
                    if (distSq <= outerSq && distSq >= innerSq) {
                        // Top-left corner
                        if (dx <= 0 && dy <= 0) {
                            setPixel(innerX + r + dx, innerY + r + dy, color);
                        }
                        // Top-right corner
                        if (dx >= 0 && dy <= 0) {
                            setPixel(innerX + innerW - r + dx, innerY + r + dy, color);
                        }
                        // Bottom-right corner
                        if (dx >= 0 && dy >= 0) {
                            setPixel(innerX + innerW - r + dx, innerY + innerH - r + dy, color);
                        }
                        // Bottom-left corner
                        if (dx <= 0 && dy >= 0) {
                            setPixel(innerX + r + dx, innerY + innerH - r + dy, color);
                        }
                    }
                }
            }
        } else {
            // Draw sharp rectangle (no rounding)
            // Top edge
            for (int px = innerX; px < innerX + innerW; px++) {
                setPixel(px, innerY, color);
            }
            
            // Bottom edge
            for (int px = innerX; px < innerX + innerW; px++) {
                setPixel(px, innerY + innerH - 1, color);
            }
            
            // Left edge
            for (int py = innerY; py < innerY + innerH; py++) {
                setPixel(innerX, py, color);
            }
            
            // Right edge
            for (int py = innerY; py < innerY + innerH; py++) {
                setPixel(innerX + innerW - 1, py, color);
            }
        }
    }
}

void Display::drawBitmap(int x, int y, const uint16_t* bitmap, int w, int h, bool color) {
#ifdef USE_MSP1601
    display->drawBitmap(x, y, bitmap, w, h, color ? 0xFFFF : 0x0000);
#else
    display->drawBitmap(x, y, bitmap, w, h, color);
#endif
}

