# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Changed to pico2 for RP2350 (Pico 2) microcontroller
# Using ARM Cortex-M0+ cores instead of RISC-V
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform: rp2040, rp2350-arm-s, rp2350-riscv")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(FLOPPY_APPLE_II_PICO C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(FLOPPY_APPLE_II_PICO 
    FLOPPY_APPLE_II_PICO.cpp
    FloppyEmulator.cpp
    SDCardManager.cpp
    CLIHandler.cpp
    FAT32.cpp
    SSD1306.cpp
    SH1107.cpp
    MSP1601.cpp
    Display.cpp
    RotaryEncoder.cpp
    UIHandler.cpp
)

pico_set_program_name(FLOPPY_APPLE_II_PICO "FLOPPY_APPLE_II_PICO")
pico_set_program_version(FLOPPY_APPLE_II_PICO "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(FLOPPY_APPLE_II_PICO 1)
pico_enable_stdio_usb(FLOPPY_APPLE_II_PICO 0)

# Generate PIO header
pico_generate_pio_header(FLOPPY_APPLE_II_PICO ${CMAKE_CURRENT_LIST_DIR}/floppy_bit_output.pio)
pico_generate_pio_header(FLOPPY_APPLE_II_PICO ${CMAKE_CURRENT_LIST_DIR}/floppy_irq_timer.pio)

# Add the standard library to the build
target_link_libraries(FLOPPY_APPLE_II_PICO
        pico_stdlib
        hardware_spi
        hardware_uart
        hardware_i2c
        hardware_pio
        hardware_dma
        hardware_pwm
        pico_multicore)

# Add the standard include files to the build
target_include_directories(FLOPPY_APPLE_II_PICO PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

#pico_set_binary_type(FLOPPY_APPLE_II_PICO no_flash)

pico_set_binary_type(FLOPPY_APPLE_II_PICO copy_to_ram)
pico_add_extra_outputs(FLOPPY_APPLE_II_PICO)





