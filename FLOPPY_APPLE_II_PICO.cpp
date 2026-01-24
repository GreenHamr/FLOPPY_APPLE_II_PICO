#include <cstdint>
#include <hardware/sync.h>
#include <pico/time.h>
#include <stdint.h>
#include <hardware/gpio.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "PinConfig.h"
#include "FloppyEmulator.h"
#include "SDCardManager.h"
#include "CLIHandler.h"
#include "Display.h"
#include "RotaryEncoder.h"
#include "UIHandler.h"

// Global instances of UI and CLI handlers
static CLIHandler* g_cli = nullptr;
static UIHandler* g_ui = nullptr;
static FloppyEmulator* g_floppy = nullptr;


bool reserved_addr(uint8_t addr) {
    return (addr & 0x78) == 0 || (addr & 0x78) == 0x78;
}

void ScanI2CBus0() {
    printf("\nI2C 0 Bus Scan\n");
    printf("   0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n");

    for (int addr = 0; addr < (1 << 7); ++addr) {
        if (addr % 16 == 0) {
            printf("%02x ", addr);
        }
        // Skip over any reserved addresses.
        int ret;
        uint8_t rxdata;
        i2c_read_timeout_us(i2c0,addr,&rxdata,1,false,1000);
        if (reserved_addr(addr))
            ret = PICO_ERROR_GENERIC;
        else
            ret = i2c_read_blocking(i2c0, addr, &rxdata, 1, false);
            //ret = i2c_read_timeout_us(I2CMain,addr,&rxdata,1,false,1000);
        printf(ret < 0 ? "." : "@");
        printf(addr % 16 == 15 ? "\n" : "  ");
    }
    printf("Done.\n");

}

// Core1 processing function
void core1_process() {
    // Dummy while loop for now
    gpio_init(3);
    gpio_set_dir(3, GPIO_OUT);
    gpio_put(3, 1);
    while (true) {
        //gpio_put(3, !gpio_get(3));
        
        // Process CLI commands
        if (!g_floppy->isWriteEnabled()) {

            if (g_cli) {
                g_cli->process();
            }
            
            // Update UI
            if (g_ui) {
                g_ui->update();
            }

            // Small delay to prevent CPU spinning
            // Note: core1 doesn't affect PIO/DMA on core0, so sleep_us is OK here
            sleep_us(10);
        }
    }
}

bool timer_callback(struct repeating_timer *t) {
    g_floppy->processStepperMotor();
    return true; // Връща true, за да продължи таймерът
}

int main()
{
    // Overclock Pico 2 (RP2350) to 200MHz for better performance
    // This provides more CPU power for GCR encoding and prevents track skipping
    // Set voltage regulator to 1.20V for stability at 200MHz (higher than default 1.10V)
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);  // Wait for voltage to stabilize
    
    // Set system clock to 200MHz (overclock from default 150MHz)
    // RP2350 can safely run at 200MHz with adequate voltage
    set_sys_clock_khz(200000, true);
    
    stdio_init_all();
    
    // Wait for serial connection
    sleep_ms(2000);
    uint32_t actualFreq = frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS);
    printf("Apple II Floppy Emulator Starting...\r\n");
    printf("CPU Frequency: %u MHz (overclocked to 200MHz)\r\n", actualFreq / 1000);
    
    // Create floppy emulator instance (static to avoid 140KB stack allocation)
    static FloppyEmulator floppy(
        GPIO_PH0, GPIO_PH1, GPIO_PH2, GPIO_PH3,  // Stepper phases (INPUT - from controller)
        GPIO_READ,                                // Read data (OUTPUT - to controller)
        GPIO_WRITE,                               // Write data (INPUT - from controller)
        GPIO_WRITE_ENABLE,                        // Write enable (INPUT - from controller)
        GPIO_DRIVE_SEL                            // Drive select (INPUT - from controller)
    );
    g_floppy = &floppy;

    // Initialize floppy emulator hardware
    floppy.init();
    printf("Floppy emulator initialized\r\n");
    printf("Waiting for Apple II controller signals...\r\n");
    printf("Stepper phases (PH0-PH3) are now INPUTS (monitoring controller)\r\n");
    
    // Initialize SD card with hotplug support (static to avoid large stack allocation)
    static SDCardManager sdCard(SD_SPI_INSTANCE, SD_SPI_CS, SD_SPI_MOSI, SD_SPI_MISO, SD_SPI_SCK, SD_CARD_DETECT);
    
    printf("Initializing SD card with hotplug support...\r\n");
    printf("SD Card SPI: CS=GPIO%d, MOSI=GPIO%d, MISO=GPIO%d, SCK=GPIO%d\r\n", 
           SD_SPI_CS, SD_SPI_MOSI, SD_SPI_MISO, SD_SPI_SCK);
    printf("SD Card Detect: GPIO%d (active LOW)\r\n", SD_CARD_DETECT);
    
    // Check if card is present
    bool sdInitialized = false;
    if (sdCard.isCardPresent()) {
        printf("SD card detected, initializing...\r\n");
        
        // Try multiple times with delays
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) {
                printf("Retrying SD card initialization (attempt %d/3)...\r\n", attempt + 1);
                sleep_ms(500);
            }
            
            // Use verbose mode for detailed diagnostics
            // Test max speed first, then initialize with that speed
            uint32_t maxSpeed = sdCard.testMaxReadSpeed(10, true);
            if (maxSpeed == 0) {
                maxSpeed = 20000000;  // Default to 20MHz if test failed
            }
            if (sdCard.init(maxSpeed, true)) {
                printf("SD card initialized successfully at %u Hz\r\n", maxSpeed/1000000);
                sdInitialized = true;
                break;
            }
        }
        
        if (!sdInitialized) {
            printf("SD card initialization failed after 3 attempts\r\n");
            printf("Possible issues:\r\n");
            printf("  - Wrong pin connections\r\n");
            printf("  - Card may need formatting\r\n");
        }
    } else {
        printf("No SD card detected (waiting for card insertion...)\r\n");
    }
    

    i2c_init(OLED_I2C_INSTANCE, 400*1000);
    gpio_set_function(OLED_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_I2C_SDA);
    gpio_pull_up(OLED_I2C_SCL);

    ScanI2CBus0();


    // Initialize CLI handler (UART1 on pins 4 and 5)
    static CLIHandler cli(uart1, CLI_UART_TX, CLI_UART_RX, CLI_UART_BAUDRATE);
    g_cli = &cli;
    g_cli->init();
    g_cli->setFloppyEmulator(&floppy);
    if (sdInitialized) {
        g_cli->setSDCardManager(&sdCard);
    }
    
    printf("CLI initialized on UART1 (TX: GPIO%d, RX: GPIO%d)\r\n", CLI_UART_TX, CLI_UART_RX);
    printf("Ready! Connect to UART1 at %d baud to use CLI\r\n", CLI_UART_BAUDRATE);
    printf("Type 'help' for available commands\r\n");
    
    // Initialize display
#ifdef USE_MSP1601
    printf("Initializing MSP1601 LCD display (SPI)...\r\n");
    printf("LCD SPI: CS=GPIO%d, DC=GPIO%d, RST=GPIO%d, LED=GPIO%d\r\n",
           LCD_SPI_CS, LCD_SPI_DC, LCD_SPI_RST, LCD_SPI_LED);
    
    // Initialize SPI for LCD
    spi_init(LCD_SPI_INSTANCE, 10000000);  // 10MHz SPI speed
    gpio_set_function(LCD_SPI_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(LCD_SPI_SCK, GPIO_FUNC_SPI);
    
    // Create MSP1601 display instance (static to avoid stack allocation)
    static MSP1601 lcdDisplay(LCD_SPI_INSTANCE, LCD_SPI_CS, LCD_SPI_DC, LCD_SPI_RST, LCD_SPI_LED);
    static Display display(&lcdDisplay);
    if (display.init()) {
        printf("MSP1601 LCD display initialized successfully\r\n");
    } else {
        printf("MSP1601 LCD display initialization failed\r\n");
    }
#else
    printf("Initializing OLED display (I2C)...\r\n");
#ifdef USE_SSD1309
    printf("Using SSD1309 controller (2.42\" display)\r\n");
#elif defined(USE_SH1107)
    printf("Using SH1107 controller (128x128 display)\r\n");
#else
    printf("Using SSD1306 controller\r\n");
#endif
#ifdef USE_SSD1309
    i2c_init(OLED_I2C_INSTANCE, 400000);  // 400kHz I2C speed for SSD1309
#elif defined(USE_SH1107)
    i2c_init(OLED_I2C_INSTANCE, 400000);  // 400kHz I2C speed for SH1107
#else
    i2c_init(OLED_I2C_INSTANCE, 400000);  // 400kHz I2C speed for SSD1306
#endif
    gpio_set_function(OLED_I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_I2C_SDA);
    gpio_pull_up(OLED_I2C_SCL);
    
    // Create OLED display instance (static to avoid stack allocation)
#ifdef USE_SSD1309
    static SSD1306 oledDisplay(OLED_I2C_INSTANCE, OLED_I2C_ADDRESS, OLED_RESET);
#elif defined(USE_SH1107)
    static SH1107 oledDisplay(OLED_I2C_INSTANCE, OLED_I2C_ADDRESS, OLED_RESET);
#else
    static SSD1306 oledDisplay(OLED_I2C_INSTANCE, OLED_I2C_ADDRESS);
#endif
    static Display display(&oledDisplay);
    if (display.init()) {
        printf("OLED display initialized successfully\r\n");
    } else {
        printf("OLED display initialization failed\r\n");
    }
#endif
    
    // Create rotary encoder instance (static to avoid stack allocation)
    printf("Initializing rotary encoder...\r\n");
    static RotaryEncoder encoder(ENCODER_CLK, ENCODER_DT, ENCODER_SW);
    encoder.init();
    printf("Rotary encoder initialized (CLK: GPIO%d, DT: GPIO%d, SW: GPIO%d)\r\n", 
           ENCODER_CLK, ENCODER_DT, ENCODER_SW);
    
    // Create UI handler
    static UIHandler ui(&display, &encoder);
    g_ui = &ui;
    g_ui->init();
    g_ui->setFloppyEmulator(&floppy);
    if (sdInitialized) {
        g_ui->setSDCardManager(&sdCard);
    }
    printf("UI handler initialized\r\n");
    
    // Launch core1 processing
    printf("Launching core1 processing...\r\n");
    multicore_launch_core1(core1_process);
    printf("Core1 processing started\r\n");
    
    // Main loop (runs on core0)
    absolute_time_t lastSDCheck = get_absolute_time();
    
    // GPIO2 debug - toggle on each loop iteration to measure loop frequency
    gpio_init(2);
    gpio_set_dir(2, GPIO_OUT);
    gpio_put(2, 0);
    gpio_pull_up(2);
    gpio_init(3);
    gpio_set_dir(3, GPIO_OUT);
    gpio_put(3, 0);
    gpio_pull_up(3);
    gpio_init(14);
    gpio_set_dir(14, GPIO_OUT);
    gpio_put(14, 0);
    gpio_pull_up(14);
    printf("Main loop starting... V0.3.0\r\n");
    //g_floppy->startWriteIRQTimer();

//    uint8_t writeENState = 0;


    static repeating_timer_t motorTimer;
    add_repeating_timer_us(-1000, timer_callback, NULL, &motorTimer);


    while (true) {
      
        // CRITICAL: During write operation, use tight polling loop
        // Write timing is critical - based on AVR write loop implementation
        if (floppy.isWriteEnabled() && floppy.isDriveSelected()) {
            
            // Start write procedure (init_writing equivalent)
            g_floppy->startWritingProcedure();
            
            // Get initial magnetic state
            uint8_t magstate = g_floppy->floppy_write_in();
            uint32_t flags = save_and_disable_interrupts();
            // Tight polling loop - runs until write ends
            do {
                // Check for pin state change (flux transition)
                uint8_t new_magstate = g_floppy->floppy_write_in();
                if (magstate != new_magstate) {
                    magstate = new_magstate;
                    g_floppy->resetWritePWMTimer();  // Reset timer counter after transition
                    g_floppy->writePinChange();  // Bit "1" - shift left and add 1
                } else
                
                // Check for PWM timer overflow (4μs period elapsed without transition)
                if (g_floppy->checkPWMOverflow()) {
                    g_floppy->writeIdle();  // Bit "0" - shift left
                }
            } while (g_floppy->isWriteEnabled());
            
            // End write procedure (end_writing equivalent)
            g_floppy->stopWritingProcedure();
            restore_interrupts(flags);
            continue;
        }
        
        g_floppy->process();
        
        /*
        if (!g_floppy->getGCRTrackCacheDirty()) lastTime = get_absolute_time();
        int diff = absolute_time_diff_us(lastTime, get_absolute_time());
        //printf("Diff: %d\r\n", diff);
        if (diff > 3000000) {
            lastTime = get_absolute_time();
            printf("Saving GCR cache to disk image... V0.3.0\r\n");
            g_floppy->saveGCRCacheToDiskImage();
        }
        */
        // Don't sleep when PIO/DMA is active - it needs continuous CPU cycles
        // Only check drive selection for other tasks (SD card hotplug, etc.)
        if (!floppy.isDriveSelected()) {
            // Drive not selected - can do other tasks
            // Check SD card hotplug (every 100ms)
            absolute_time_t now = get_absolute_time();
            if (absolute_time_diff_us(lastSDCheck, now) > 100000) {  // 100ms
                bool cardStateChanged = sdCard.checkCardPresence();
                bool currentCardState = sdCard.isCardPresent();
                
                if (cardStateChanged) {
                    if (currentCardState && !sdCard.isInitialized()) {
                        // Card inserted, try to initialize
                        printf("SD card inserted, initializing...\r\n");
                        // Test max speed first, then initialize with that speed
                        uint32_t maxSpeed = sdCard.testMaxReadSpeed(20, true);
                        if (maxSpeed == 0) {
                            maxSpeed = 20000000;  // Default to 20MHz if test failed
                        }
                        if (sdCard.init(maxSpeed, false)) {  // Non-verbose for hotplug
                            printf("SD card initialized successfully at %u Hz\r\n", maxSpeed/1000000);
                            if (g_cli) g_cli->setSDCardManager(&sdCard);
                            if (g_ui) g_ui->setSDCardManager(&sdCard);
                        } else {
                            printf("SD card initialization failed\r\n");
                        }
                    } else if (!currentCardState && sdCard.isInitialized()) {
                        // Card removed (handled in checkCardPresence)
                        if (g_cli) g_cli->setSDCardManager(nullptr);
                        if (g_ui) g_ui->setSDCardManager(nullptr);
                    }
                }
                
                lastSDCheck = now;
            }
            // Minimal delay when drive is not selected (but avoid blocking when PIO/DMA is active)
            // Don't use sleep_us() here - just continue loop immediately to avoid blocking
        }
    }
}
