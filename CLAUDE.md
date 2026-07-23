# Agent guide

Instructions for automated contributors working in `MegaDriveEnvironment`.

## Purpose and boundaries

`MegaDriveEnvironment` is a reusable C++23 library that runs Mega Drive-style
game code as a native desktop application. It models the console-facing memory,
video, input, audio, Z80, timing, and debugging facilities used by consumers
such as `../StreetsOfRageRecompilation`.

This repository owns the host runtime. Game-specific recompilation logic
belongs in the consuming repository. `../Genesis-Plus-GX` is an upstream,
read-only behavioral reference: never edit, reformat, patch, commit, or update
it.

Before changing code:

1. inspect this repository's status and preserve unrelated work;
2. identify whether the change affects the public API, runtime implementation,
   tests, Python client, or only documentation;
3. inspect the consuming call sites when changing a public contract.

## Repository layout

| Path | Responsibility |
| --- | --- |
| `include/MegaDriveEnvironment/` | Public C++ headers exposed to consumers |
| `src/system/` | Root runtime, memory, graphics, controllers, sound, Z80, and remote access |
| `src/config/` | Controls configuration UI and persistence |
| `src/runtime_tests/` | Host diagnostic modes used by consumer executables |
| `src/util/` | Logging, images, fonts, and other runtime utilities |
| `tests/` | C++ integration and protocol tests |
| `python/` | Installable `megadrive_remote` client and Python tests |
| `docs/` | Protocol and architecture documentation |
| `tools/` | Maintenance and asset utilities |

Keep consumer-facing headers under the public include root. Include them using
the established public paths, for example:

```cpp
#include "system/MegaDriveEnvironment.hpp"
```

Do not expose source-tree-only headers through public declarations.

## Runtime architecture

Games derive from `MegaDriveEnvironment`, implement `run()`, and may override
`vSync()` and `hSync(int line)`. The root object owns these major systems:

- `SystemMemory`: normalized 24-bit address space, ROM, Work RAM, and mapped I/O;
- `VDP`: ports, VRAM/CRAM/VSRAM, rendering, DMA, counters, and interrupts;
- `Controllers`: keyboard/gamepad input through the Mega Drive joypad protocol;
- `Sound`: YM2612, PSG, audio mixing, and diagnostics;
- `Z80`: Z80 RAM, bus control, banked access, and VBlank IRQ;
- `RemoteAccess`: host-only observation and automation protocol.

Maintain the distinction between host facilities and console-visible hardware.
Do not leak SDL, filesystem, sockets, or remote-control behavior into portable
game logic unless the public API explicitly represents a host-only feature.

## Dependencies and CMake contract

Requirements:

- CMake 3.24 or newer;
- a C++23 compiler;
- SDL3 available through `find_package(SDL3 REQUIRED)`;
- threads from the host toolchain;
- Git/network access on the first configuration.

CMake fetches pinned yaml-cpp, zlib, and libpng revisions. Do not edit
`build/_deps`; change the declarations or integration in `CMakeLists.txt`.

Important options:

| Option | Default | Effect |
| --- | --- | --- |
| `MEGADRIVE_ENVIRONMENT_BUILD_SHARED` | `ON` | Build the runtime as a shared library; set `OFF` for a static archive |
| `MEGADRIVE_ENVIRONMENT_SHARED_DEPS` | `OFF` | Build yaml-cpp, zlib, and libpng as shared dependencies |
| `BUILD_TESTING` | CTest default | Build tests when this is the top-level project |

The default shared library lets consumers rebuild implementation-only changes
without relinking their executable. Preserve position-independent code and
Windows symbol export behavior. SDL3 is a public dependency because SDL types
appear in public headers; implementation dependencies should remain private
when the shared build permits it.

## Build and test

Configure and compile:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Run the complete registered test set:

```bash
ctest --test-dir build --output-on-failure
```

Build a static variant when the changed code affects link behavior:

```bash
cmake -S . -B build-static \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMEGADRIVE_ENVIRONMENT_BUILD_SHARED=OFF
cmake --build build-static --parallel
ctest --test-dir build-static --output-on-failure
```

The Python client tests are registered with CTest when a compatible Python
interpreter is found. They may also be run directly:

```bash
PYTHONPATH=python/src python3 -m unittest discover -s python/tests -v
```

For changes only exercised through a consumer, build the smallest relevant
consumer after this repository's tests pass.

## Change rules

- Preserve the C++23 and C11 language settings.
- Keep byte order, address normalization, device mapping, and thread ownership
  explicit; these are correctness boundaries, not convenience details.
- Avoid blocking the emulated CPU/audio/render loops with logging, network I/O,
  or filesystem work.
- Keep remote-access protocol changes backward-aware and update
  `docs/remote-access-protocol.md`, the C++ tests, and the Python client
  together.
- Add or update a focused regression test for observable behavior changes.
- Do not weaken warnings or tests globally to accommodate one implementation.
- Do not commit build output, fetched dependencies, caches, generated images,
  local control files, or transient captures.

## Cross-repository delivery

When this repository is checked out as a submodule, a completed change may
also require the parent meta-repository to record a new gitlink. After
validation, commit and push this repository to `main` automatically unless the
user explicitly asks not to publish, then update the parent gitlink. Preserve
unrelated work and never force-push or rewrite history. Report the tests run
here and any consumer build used for validation.
