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

## Configuration & Status

System Configuration is implemented as an array of configs which is backed by NVS key-value:
- at start up, it goes through all config keys, and read the value from NVS if NVS has the requested key, otherwise, use default values.
- reading a config will return value from the array whihc is in RAM
- writing a config will save value to the array, and also commit to NVS

System Status is an aray of key-value stored in RAM.


## WiFi AP & STA


- start an Access Point with name "BASE_GNSS_XXYY" where XX, YY are two last bytes in AP MAC address, password is `12345678`
  - the IP address of Access Point is 192.168.4.1

- start an Station to connect to an external Access Point whose SSID and Password are retrived from System Configuration, i.e. `CFG_WIFI_SSID`, `CFG_WIFI_PASSWORD`
  - it will try to connect at most 10 times, after that, system can reconnect by explicitly calling to `wifi_sta_connect` again.
  - user can connect to a new Access Point by calling `wifi_sta_connect`
  - station status is stored in `STT_STA_CONNECTED`


## Partitions

Create a custom partition table, and enable it in `sdkconfig.defaults`:

``` ini
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

``` ini
# Name,   Type, SubType, Offset,  Size, Flags
# partition table starts at offset 0x8000, and occupies an entire flash sector length 0x1000
nvs,      data, nvs,     0x9000,  512K,
phy_init, data, phy,           ,  512K,
factory,  app,  factory,       ,  2M,
coredump, data, coredump,      ,  1M,
www,      data, littlefs,      ,  4M,
storage,  data, littlefs,      ,  4M
```


## HTTP Web Server

The web server starts mDNS and NetBIOS Namespace to broadcast `gnss-station.local` as the name of the server.
This helps user to connect to server without knowing the IP address.

Web page files are saved in `www` folder, in `littlefs` file system.
Add this section to discover, compile, and include the partition image into flashing step:

``` cmake
set(WEB_SRC_DIR "${CMAKE_SOURCE_DIR}/www")
if(EXISTS ${WEB_SRC_DIR})
    littlefs_create_partition_image(www ${WEB_SRC_DIR} FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${WEB_SRC_DIR} doesn't exist.")
endif()
```

To reduce bandwidth and improve response times on file serving:
- implementing ETag support: when browser requests a file, server will check `If-None-Match` in request header to determine it should send the file content or not
- ETags will be read and cached into RAM for faster checking
- ETags are calculated in build time, using a python script

  ``` cmake
  execute_process(
      COMMAND python ${CMAKE_SOURCE_DIR}/gen_www_crc32.py
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  ```

The webpage uses [Pico.css](https://picocss.com/) for styling, and [Alpine.js](https://alpinejs.dev/) for reactive data binding and elements.

To reduce tying `$store.system`, create an object which holds reference to it, and assign that object to page body data.

``` html
<body x-data="{ sys: $store.system }">
```

When webpage can not fetch status from server, it will show a notification and scroll to the top.
