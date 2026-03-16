# ESP32-S3 GNSS Base Station

GNSS Base Station using ESP32 for RTK position services.

## Development Tool

- IDE: Visual Studio Code + ESP-IDF Extenstion
- Platform: Espressif 32
- Framework: ESP-IDF 5.5.2

## Target

This firmware is developed for [ESP32-S3-WROOM-1-N16R8](https://documentation.espressif.com/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf), with below specifications:
- 2.4 GHz Wi-Fi (802.11b/g/n, STA/AP/STA+AP, 150 Mbps) and Bluetooth 5 (BLE, Mesh, 2 Mbps)
- Xtensa dual-core 32-bit LX7 microprocessor with single precision FPU, up to 240 MHz
- 384 KB ROM
- 512 KB SRAM + 16 KB SRAM in RTC
- 16 MB Flash (Quad SPI), up to 80 MHz
- 8 MB PSRAM (Octal SPI), up to 80 MHz
- 36 GPIOs
- SPI, LCD interface, Camera interface, UART, I2C, I2S, remote control, pulse counter, LED PWM, full-speed USB 2.0 OTG, USB Serial/JTAG controller, MCPWM, SD/MMC host controller, GDMA, TWAI® controller (compatible with ISO 11898-1), ADC, touch sensor,temperature sensor, timers and watchdogs

__SDK Config__

Run sdk config:
```sh
idf.py menuconfig
```

- Serial flasher config
  - Flash SPI Mode (QIO)
  - Flash Sampling Mode (STR Mode)
  - Flash SPI speed (80 MHz)
  - Flash size (16 MB)
- Component config
  - ESP PSRAM → Support for external, SPI-connected RAM → SPI RAM config
    - Mode (QUAD/OCT) of SPI RAM chip in use (Octal Mode PSRAM)
    - Set RAM clock speed (80MHz clock speed)
  - ESP System Settings
    - CPU frequency (240 MHz)
- Partition Table
  - Partition Table (Custom partition table CSV)
  - Custom partition CSV file (partitions.csv)
  - Generate an MD5 checksum (No)

Save as the default config:
```sh
idf.py save-defconfig
```
```ini sdkconfig.defaults
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_MD5=n
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
```

Add packages:
```sh
idf.py add-dependency "espressif/mdns^1.10.1"
```

## Development

Prepare:

1. Select ESP-IDF version `5.5.2`, then the target chip `esp32s3`
2. Add `.vscode` subdirectory files
3. For testing, add a Python virtual environment `python3 -m venv .venv`, activate it `source .venv/bin/activate` and install `flask`, and run the test server `python3 scripts/server_dev.py`
