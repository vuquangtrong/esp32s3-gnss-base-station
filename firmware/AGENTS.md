# AGENTS.md

## Project

- `main/` — ESP32 firmware.
- `www/index.html` — Web frontend.
- Target: ESP32-S3, ESP-IDF 6.1.1+.
- Frontend: Pico.css,  Alpine.js 2.x and ES5 Javascript syntax

## Coding Rules

- Follow existing project architecture and coding conventions.
- Prefer ESP-IDF APIs and follow ESP-IDF coding conventions.
- Define shared macros, structs, enums, and types in header files.
- Do not add documentation comments to function declarations.
- Prefer fixed-width types (`uint8_t`, `uint32_t`, etc.) and `size_t` over `int` or `long`.
- Validate function parameters, pointers, input values, and buffer boundaries.

## Memory Management

- Prefer pre-allocated static buffers when buffer sizes are known. Avoid large stack allocations.
- Prefer static allocation over dynamic allocation whenever practical. Avoid heap allocation to reduce fragmentation.
- Minimize data copying. Prefer pointers and lengths over temporary buffers.
- Ensure correct buffer boundaries, memory ownership, and pointer lifetimes.

## Hardware & FreeRTOS

- Use ESP-IDF drivers for hardware peripherals. Handle hardware errors and communication timeouts.
- Avoid busy-waiting; use FreeRTOS synchronization and notification mechanisms.
- Prefer ESP-IDF/FreeRTOS synchronization primitives or C atomic operations when appropriate.
- Avoid synchronization when data is accessed exclusively from one execution context. Do not introduce synchronization unnecessarily.

## Frontend

- Use Pico.css for styling and Alpine.js for reactive behavior.
- For better compatibility with older browsers and embedded WebViews, use Alpine.js 2.x and ES5 JavaScript syntax.
- Prefer semantic HTML and avoid unnecessary dependencies.
- Validate user input on both frontend and firmware.
- Handle asynchronous operations and HTTP errors.
- Escape user-controlled data before inserting it into HTML.

## AI Agent Rules

- Inspect existing code before making changes.
- Reuse existing implementations and avoid unnecessary refactoring.
- Keep changes focused and preserve existing functionality.
- Consider memory, stack usage, concurrency, and hardware constraints.
- Verify API compatibility with the configured ESP-IDF version.
- Build and verify firmware after code or configuration changes.
- Report build errors honestly. Never claim unverified functionality works.

## Build

Always run commands from the project root directory.

Activate ESP-IDF environment:

```bash
source "$HOME/.espressif/tools/activate_idf_v6.1.1.sh"
```

Build firmware:

```bash
idf.py build
```

Useful commands:

```bash
idf.py fullclean      # Clean build artifacts
idf.py reconfigure    # Reconfigure project
idf.py flash          # Flash firmware (only when requested)
idf.py monitor        # Monitor serial output
```

Do not flash or erase the device unless explicitly requested.
