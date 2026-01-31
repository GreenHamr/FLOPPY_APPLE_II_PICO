# Apple II Floppy Disk Emulator for Raspberry Pi Pico 2

A high-performance emulator for Apple II Disk II floppy drives, based on Raspberry Pi Pico 2 (RP2350). This project allows you to use an SD card as a replacement for original 5.25" floppy diskettes for Apple II computers.

## 🎯 Key Features

- **Full compatibility** with Apple II Disk II controllers
- **Dual format support**:
  - `.dsk` files (143KB) - standard format
  - `.nic` files (280KB) - GCR-encoded format
- **Real-time operation** - uses PIO and DMA for precise signal generation
- **Read and write support** - full read/write operations
- **SD card support** - FAT32 filesystem, support for large cards (up to 64GB+)
- **Graphical interface** - OLED/LCD display with rotary encoder navigation
- **CLI interface** - serial terminal for control and diagnostics

## 🔧 Hardware Requirements

### Microcontroller
- **Raspberry Pi Pico 2** (RP2350) with ARM Cortex-M0+ cores
- Runs at 200MHz (overclocked from 150MHz)

### Display (choose one)
- **SSD1306** (128x64, I2C) - default
- **SSD1309** (128x64, I2C) - with colored status bar
- **SH1107** (128x128, I2C) - larger display
- **MSP1601/SSD1283A** (128x128, SPI) - LCD display

### Other Components
- **SD card module** (SPI interface)
- **Rotary encoder** (for UI navigation)
- **Apple II Disk II interface compatibility** (PH0-PH3, READ, WRITE, WRITE_ENABLE, DRIVE_SEL)

## 📋 GPIO Pinout

### Apple II Floppy Interface
```
PH0 (Phase 0)        → GPIO 6
PH1 (Phase 1)        → GPIO 7
PH2 (Phase 2)        → GPIO 8
PH3 (Phase 3)        → GPIO 9
READ (Data Output)   → GPIO 10
WRITE (Data Input)   → GPIO 11
WRITE_ENABLE         → GPIO 12
DRIVE_SEL            → GPIO 13
```

### SD Card (SPI)
```
MISO                 → GPIO 16
CS                   → GPIO 17
SCK                  → GPIO 18
MOSI                 → GPIO 19
Card Detect          → GPIO 15 (active LOW)
```

### Display (I2C for SSD1306/SH1107)
```
SDA                  → GPIO 20
SCL                  → GPIO 21
RESET (optional)     → GPIO 22
```

### Display (SPI for MSP1601)
```
MOSI                 → GPIO 14
SCK                  → GPIO 15
CS                   → GPIO 22
DC (Data/Command)    → GPIO 23
RST                  → GPIO 24
LED (Backlight)      → GPIO 25
```

### Rotary Encoder
```
CLK                  → GPIO 26
DT                   → GPIO 27
SW (Button)          → GPIO 28
```

### CLI UART
```
TX                   → GPIO 4
RX                   → GPIO 5
Baudrate             → 115200
```

## 🚀 Compilation and Installation

### Prerequisites

1. **Raspberry Pi Pico SDK** (version 2.2.0 or newer)
2. **CMake** (version 3.13 or newer)
3. **ARM GCC toolchain** (version 14.2 or newer)
4. **Python 3** (for pico-sdk tools)

### Build Steps

```bash
# Clone the project
git clone <repository-url>
cd FLOPPY_APPLE_II_PICO

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Compile
make -j4

# The result is FLOPPY_APPLE_II_PICO.uf2 file in build/ directory
```

### Pico Installation

1. Hold the **BOOTSEL** button on Pico 2
2. Connect USB cable to computer
3. Copy `FLOPPY_APPLE_II_PICO.uf2` file to the appeared USB drive
4. Pico 2 will automatically restart

## 💾 SD Card Preparation

1. **Format SD card** as FAT32
2. **Create directory** for disk images (e.g., `/DISKS/`)
3. **Copy `.dsk` or `.nic` files** to the directory
4. **Insert card** into SD card module

### Supported Formats

- **DSK files**: 143,360 bytes (35 tracks × 16 sectors × 256 bytes)
- **NIC files**: 286,720 bytes (35 tracks × 16 sectors × 512 bytes, GCR-encoded)

## 🎮 Usage

### Graphical Interface (UI)

The emulator automatically starts with a graphical interface:

- **Rotary encoder**: Navigate up/down
- **Encoder button**: Select/confirm
- **Screens**:
  - **File List**: List of files on SD card
  - **Info**: Information about loaded file (type, size, track, cache status)
  - **Status**: SD card status (type, speed, size, partition info)

### CLI Interface

Connect via serial terminal (115200 baud):

```
Available commands:
  help              - Show help
  load <file>       - Load disk image from SD card
  list              - List files in current directory
  cd <dir>          - Change directory
  pwd               - Print current directory
  info              - Show disk image info
  status            - Show emulator status
  seek <track>      - Seek to track (0-34)
  read <t> <s>      - Read track and sector
  gpio/pins         - Show GPIO pin states
  test              - Test emulator
```

## 🔬 Technical Details

### Architecture

- **Core 0**: Floppy emulation (real-time signals, PIO/DMA)
- **Core 1**: UI management, SD card operations, CLI

### GCR Encoding/Decoding

- **DSK files**: Automatic GCR format encoding/decoding
- **NIC files**: Direct use of GCR-encoded data

### Stepper Motor Tracking

- **GPIO IRQ**: Real-time tracking of stepper motor phases
- **High priority**: Guarantees no missed phases
- **Automatic tracking**: Position updates automatically

### Optimizations

- **PIO + DMA**: Hardware generation of bit signals
- **Track Cache**: Current track cached in memory
- **Lazy Loading**: Tracks loaded only when needed
- **Direct Block Access**: For NIC files - direct sector reading

## 📊 Specifications

### Apple II Disk II Format
- **Tracks**: 35 (0-34)
- **Sectors per track**: 16
- **Bytes per sector**: 256 (raw data)
- **Bit period**: 4μs (125 kbps)
- **Rotation speed**: 300 RPM (200ms per rotation)
- **GCR bytes per track**: 6,656 (416 bytes per sector)

### Performance
- **Read latency**: < 10ms for NIC files
- **Write support**: Full support
- **Track change**: Automatic tracking

## 🐛 Known Issues and Limitations

- SD card must be formatted as FAT32
- MBR partition support is limited (searches for first partition)
- exFAT/NTFS filesystems are not supported (shows formatting message)

## 🔧 Configuration

### Changing Display

Edit `PinConfig.h`:

```c
// For SH1107 (128x128)
#define USE_SH1107

// For SSD1306 (128x64)
// #define USE_SH1107  // Comment this out

// For MSP1601 (128x128, SPI)
// #define USE_MSP1601
```

### Changing GPIO Pins

All GPIO pins are defined in `PinConfig.h`. Change them according to your schematic.

## 📝 License

[Add license information here]

## 🙏 Acknowledgments

- Raspberry Pi Foundation for Pico SDK
- Apple II community for support and testing

## 📧 Contact

[Add contact information here]

---

**Version**: 0.1  
**Platform**: Raspberry Pi Pico 2 (RP2350)  
**SDK Version**: 2.2.0
