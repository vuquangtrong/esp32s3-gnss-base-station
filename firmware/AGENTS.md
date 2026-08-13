# Guidance & Rules

Project structure:

- Firmware code in `main/`
- Frontend code in `www/index.html`

Technical Stack:

- Target on ESP32-S3, using [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/index.html), at least ESP-IDF 6.0.2
- Frontend Webpage uses [Pico.css](https://picocss.com/) for styling, and [Alpine.js](https://alpinejs.dev/) for reactive data binding and elements.

Development best practices and rules:

- Define macros, structs, types in header files
- Do not create documentation comment for method declaration
- Use meaningful names for parameters, variables
- Always initialize variables to a known value
- Always validate input paramerters
- Replace standard types like int or long with fixed-width types from <stdint.h> (uint8_t, int32_t, uint16_t)
- Hardware-modified variables or software flags should be volatile marked
- Prefer to use local variable instead of dynamically allocate on hep, may use static local variables to save stack if it has big size
- Try to minize data copy, reduce copy buffer to buffer, should pass pointer and elimate any tempoary buffer

Build steps:

- Change to the project's root directory first
- Then activate IDF env: `source $HOME/.espressif/tools/activate_idf_v6.0.2.sh`
- Then run build command: `idf.py build`
