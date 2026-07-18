# CLAUDE.md

Guidance for LLM agents working in `MegaDriveEnvironment`.

## Scope

`MegaDriveEnvironment` is a reusable C++23 Sega Mega Drive runtime and local PC
development environment. It is consumed by sibling projects, especially
`../StreetsOfRageRecompilation`.

Do not make changes in `../Genesis-Plus-GX`; it is an upstream dependency not
owned by this project.

## Layout

- `include/MegaDriveEnvironment/` - public headers for consumers.
- `src/` - runtime implementation.
- `assets/` - example/source art assets.
- `tools/` - local development utilities.
- `tests/` - tests and fixtures.

## Build

Configure and build this library directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Builds a **shared** library by default (`libMegaDriveEnvironment.dylib` /
`.so` / `.dll`) so sibling games can rebuild only the dylib for
implementation-only edits. Force a static archive with:

```bash
cmake -S . -B build -DMEGADRIVE_ENVIRONMENT_BUILD_SHARED=OFF
```

Consumer projects normally include it with CMake:

```cmake
# Skip re-linking the game when only the shared lib was rebuilt.
set(CMAKE_LINK_DEPENDS_NO_SHARED ON)
add_subdirectory("../MegaDriveEnvironment" "${CMAKE_BINARY_DIR}/MegaDriveEnvironment")
target_link_libraries(my_game PRIVATE MegaDriveEnvironment::MegaDriveEnvironment)
```

## Dependencies

- CMake 3.24+
- C++23 compiler
- SDL3 installed on the host
- FetchContent dependencies: `yaml-cpp`, `zlib`, `libpng`

By default FetchContent deps are **static** (PIC) and linked privately into the
shared library. Consumers that want shared deps set
`MEGADRIVE_ENVIRONMENT_SHARED_DEPS=ON` before `add_subdirectory` (see
`StreetsOfRageRecompilation`). libpng tests/tools/framework targets stay off.

## Architecture

Games subclass `MegaDriveEnvironment` and override `run()`. Optional interrupt
handlers include `vSync()` and `hSync(int line)`.

Key systems:

- `system/MegaDriveEnvironment` - root object and subsystem ownership.
- `system/memory/SystemMemory` - ROM/WRAM address space.
- `system/graphics` - VDP state, ports, tiles, renderer, framebuffer.
- `system/controllers` - Mega Drive joypad protocol.
- `system/sound` and `system/z80` - audio/sub-CPU subsystems.
- `config/controls` - controller configuration UI.
- `runtime_tests` - environment subclasses for visual/input/audio checks.

## Conventions

- Keep public consumer-facing APIs under `include/MegaDriveEnvironment/`.
- Include headers through the public include root, for example
  `#include "system/MegaDriveEnvironment.hpp"`.
- Avoid committing build outputs, fetched dependencies, caches, or generated
  screenshots.
