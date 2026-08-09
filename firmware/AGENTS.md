# Rules

Project structure:

- Main souce code in `main/`
- Frontend source in `www/index.html`

Strictly follow below rules:

- Do not create documentation comment for method declaration
- Always initialize variables to a known value
- Always validate input params
- Define macros, structs, types in header files

Technical Stack:

- Target on ESP32-S3, using [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/index.html).
- Frontend Webpage uses [Pico.css](https://picocss.com/) for styling, and [Alpine.js](https://alpinejs.dev/) for reactive data binding and elements.

Build steps:

- Change to the project's root directory first
- Then activate IDF env: `source $HOME/.espressif/tools/activate_idf_v6.0.2.sh`
- Then run build command: `idf.py build`

Target Chip: ESP32-S3-WROOM-1-N16R8 with below specifications:

- Xtensa® dual-core 32-bit LX7 microprocessor with single precision FPU, up to 240 MHz
- 384 KB ROM
- 512 KB SRAM + 16 KB SRAM in RTC
- (Not enabled) Embedded 8 MB PSRAM (Octal SPI), up to 80 MHz
- Integrated 16 MB Flash (Quad SPI), up to 80 MHz
- 2.4 GHz Wi-Fi (802.11b/g/n, STA/AP/STA+AP, 150 Mbps) and Bluetooth 5 (BLE, Mesh, 2 Mbps)

UI Develelopment Server:

- Activate python venv in the root directory `source .venv/bin/activate`, which has Flask installed
- Run dev server: `python3 dev-server.py`, which implements stuffed API endpoints declared in `server.c`
