set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
set(PICO_BOARD waveshare_rp2350_touch_lcd_3.49 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.4.0")
  message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.4.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

project(pico-clock C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

# Add compilation subdirectory (驱动库)
add_subdirectory(./lib/Config)
add_subdirectory(./lib/QSPI_PIO)
add_subdirectory(./lib/LCD)
add_subdirectory(./lib/Touch)
add_subdirectory(./lib/QMI8658)
add_subdirectory(./lib/PCF85063A)
add_subdirectory(./lib/lvgl)
add_subdirectory(./lib/PSRAM)

# Add header file directory
include_directories(./include)
include_directories(./lib/Config)
include_directories(./lib/QSPI_PIO)
include_directories(./lib/LCD)
include_directories(./lib/Touch)
include_directories(./lib/QMI8658)
include_directories(./lib/PCF85063A)
include_directories(./lib/lvgl)
include_directories(./lib/PSRAM)
include_directories(./lib/audio_pio)
include_directories(./lib/es8311)

# PICO_CONFIG: PICO_XOSC_STARTUP_DELAY_MULTIPLIER, Multiplier to lengthen xosc startup delay to accommodate slow-starting oscillators, type=int, min=1, default=1, group=hardware_xosc

add_executable(pico-clock
    main.c
    src/app_main.c
    src/clock_ui.c
)

pico_generate_pio_header(pico-clock ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO/qspi.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/QSPI_PIO)

pico_generate_pio_header(pico-clock ${CMAKE_CURRENT_LIST_DIR}/lib/audio_pio/audio_pio.pio
                        OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/audio_pio)

target_sources(pico-clock PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/lib/audio_pio/audio_pio.c
    ${CMAKE_CURRENT_LIST_DIR}/lib/es8311/es8311.c)


pico_set_program_name(pico-clock "pico-clock")
pico_set_program_version(pico-clock "0.1")

pico_enable_stdio_uart(pico-clock 0)
pico_enable_stdio_usb(pico-clock 1)

# Add the standard library to the build

add_compile_options(-O2)

target_link_libraries(pico-clock
                      Config
                      QSPI_PIO
                      LCD
                      Touch
                      PSRAM
                      QMI8658
                      PCF85063A
                      LVGL
                      pico_stdlib
                      hardware_pio 
                      hardware_pwm 
                      hardware_dma
                      hardware_spi
                      hardware_i2c
                      hardware_gpio
                      hardware_adc
                      pico_multicore
                      hardware_clocks
)

# Add the standard include files to the build
target_include_directories(pico-clock PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(pico-clock)

