#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// ============================================================================
// Apple II Floppy Interface GPIO Pins
// ============================================================================

// Stepper motor phases (PH0-PH3)
#define GPIO_PH0           6
#define GPIO_PH1           7
#define GPIO_PH2           8
#define GPIO_PH3           9

// Read/Write control signals
#define GPIO_READ          10
#define GPIO_WRITE         11
#define GPIO_WRITE_ENABLE  12
#define GPIO_DRIVE_SEL     13  // Drive select (from controller)

// ============================================================================
// SD Card SPI Interface GPIO Pins
// ============================================================================

#define SD_SPI_INSTANCE    spi0
#define SD_CARD_DETECT     15  // Card detect pin (active LOW - connected to GND when card is present)
#define SD_SPI_MISO        16
#define SD_SPI_CS          17
#define SD_SPI_SCK         18
#define SD_SPI_MOSI        19

// ============================================================================
// CLI UART Interface GPIO Pins
// ============================================================================

#define CLI_UART_INSTANCE  uart1
#define CLI_UART_TX        4
#define CLI_UART_RX        5
#define CLI_UART_BAUDRATE  115200

// ============================================================================
// Display Interface Configuration
// ============================================================================

// Display controller selection:
// - Comment out all to use SSD1306 (default, I2C)
// - Uncomment USE_SSD1309 to use SSD1309 (I2C)
// - Uncomment USE_MSP1601 to use MSP1601/SSD1283A (SPI)
// #define USE_SSD1309
// #define USE_MSP1601

// ============================================================================
// OLED Display I2C Interface GPIO Pins (SSD1306/SSD1309)
// ============================================================================

#ifndef USE_MSP1601
    #define OLED_I2C_INSTANCE  i2c0
    #define OLED_I2C_SDA       20
    #define OLED_I2C_SCL       21

    #ifdef USE_SSD1309
        #define OLED_I2C_ADDRESS   0x3C  // SSD1309 default I2C address (can be 0x3C or 0x3D)
        #define OLED_RESET         22    // Reset pin for SSD1309 (GPIO22)
        #define OLED_WIDTH         128
        #define OLED_HEIGHT        64    // 2.42" display typically 128x64
        #define OLED_STATUS_HEIGHT 16    // Upper yellow section for status
        #define OLED_CONTENT_HEIGHT 48   // Lower blue section for content
    #else
        #define OLED_I2C_ADDRESS   0x3C  // SSD1306 default I2C address
        #define OLED_WIDTH         128
        #define OLED_HEIGHT        64
        #define OLED_STATUS_HEIGHT 16    // Upper yellow section for status
        #define OLED_CONTENT_HEIGHT 48   // Lower blue section for content
    #endif
#endif

// ============================================================================
// LCD Display SPI Interface GPIO Pins (MSP1601/SSD1283A)
// ============================================================================

#ifdef USE_MSP1601
    #define LCD_SPI_INSTANCE  spi1      // Use SPI1 for LCD (SPI0 is for SD card)
    #define LCD_SPI_MOSI      14
    #define LCD_SPI_SCK       15
    #define LCD_SPI_CS        22         // Chip Select
    #define LCD_SPI_DC        23         // Data/Command pin (A0)
    #define LCD_SPI_RST       24         // Reset pin
    #define LCD_SPI_LED       25         // Backlight control (optional, use 0xFF to disable)
    
    #define LCD_WIDTH         128        // 1.6" display typically 128x128
    #define LCD_HEIGHT        128
#endif

// ============================================================================
// Rotary Encoder GPIO Pins
// ============================================================================

#define ENCODER_CLK        26  // Clock pin (A)
#define ENCODER_DT         27  // Data pin (B)
#define ENCODER_SW         28  // Switch/Button pin

#endif // PIN_CONFIG_H

