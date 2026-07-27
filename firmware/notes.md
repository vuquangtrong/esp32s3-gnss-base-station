# Development Notes

Target detection:

``` log
Connected to ESP32-S3 on /dev/ttyUSB0:
Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
Crystal frequency:  40MHz
MAC:                98:a3:16:cd:fd:dc
```

Can use command `ESP-IDF: Add VS Code Configuration Folder` to generate `.vscode` with generated coresponding C/C++ properties for code assistant.


## Flash Size

As the module has integrated 16 MB Quad SPI Flash, use SDK Config Editor (menuconfig) to set:
- Flash SPI Mode: `QIO`
- Flash Size: `16 MB`
- Flash Speed: `80 MHz` (default)

Run command `ESP-IDF: Save Default SDKCONFIG file (save-defconfig)` to export changes into `sdkconfig.defaults`:

``` ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
```
