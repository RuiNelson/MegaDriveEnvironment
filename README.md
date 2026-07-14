# MegaDriveEnvironment

Develop Sega Mega Drive / Genesis software as a normal C++23 application on
your PC, with the hardware-facing parts of the program connected to a focused
Mega Drive runtime.

`MegaDriveEnvironment` shortens the inner development loop. Game code runs
natively, so the debugger, profiler, sanitizers, logs and IDE all work as they
would for any other desktop program. Around that code, the environment models
the memory map, VDP, 3-button controllers, Z80, YM2612 and PSG closely enough to
develop and inspect real hardware-style interactions.

The environment is deliberately **not a complete, cycle-accurate console
emulator**, and it does not turn arbitrary desktop C++ into a ROM. It is the PC
side of a portable game architecture. A real-hardware target still needs its
own startup code, linker layout, target memory access and cross-toolchain. The
[`MegaDriveEnvironmentSampleGame`](https://github.com/RuiNelson/MegaDriveEnvironmentSampleGame)
demonstrates that complete two-target workflow.

## Documentation map

- [Build and test](#build-and-test)
- [Add it to a CMake project](#add-it-to-a-cmake-project)
- [Runtime architecture](#runtime-architecture)
- [Memory and hardware access](#memory-and-hardware-access)
- [VDP and frame timing](#vdp-and-frame-timing)
- [Controllers and configuration](#controllers-and-configuration)
- [Sound and Z80](#sound-and-z80)
- [Debugging tools](#debugging-tools)
- [Portability to real hardware](#portability-to-real-hardware)

## Why use it?

- Debug game code with native breakpoints, stepping and memory inspection.
- Keep the Mega Drive's 24-bit, big-endian address space visible to the game.
- Exercise VDP ports, planes, sprites, scrolling, DMA and interrupts.
- Read keyboard or gamepad input through the original 3-button joypad protocol.
- Run Z80 programs and send timestamped writes to the YM2612 and PSG.
- Capture the current frame or a full VDP diagnostic image without enabling
  expensive per-frame debug output.
- Reuse portable game logic in a separate real-hardware build instead of
  maintaining a disposable PC-only renderer and input layer.

## Current scope

| Area | Implemented behaviour |
| --- | --- |
| 68000 execution | Native C++ game code plus a `CPU68K` register file for mechanically generated/recompiled code; no 68000 interpreter |
| System memory | 24-bit address normalization, 4 MiB ROM, 64 KiB Work RAM, big-endian byte/word/long access and mapped-device routing |
| VDP | Mode 5 ports and registers, VRAM/CRAM/VSRAM, planes A/B, window, scrolling, linked sprites, priorities, sprite limits/collision, DMA, H/V counters, HBlank/VBlank events, interlace and shadow/highlight |
| Video output | SDL3 window, integer/fitted scaling, internal 50/60 Hz timer or display VSync modes, PNG captures |
| Controllers | Two configurable keyboard/gamepad players exposed through the active-low 3-button protocol |
| Sound | YM2612 through ymfm, SN76489-compatible PSG, 48 kHz output and timestamped non-blocking event delivery |
| Z80 | Z80 core, 8 KiB RAM, banked 68000 access, bus request/reset and VBlank IRQ |
| Region | Language and 50/60 Hz pins through the hardware version register |

Accuracy is feature-driven. Unimplemented hardware ranges currently behave as
safe stubs, and host thread scheduling is not a substitute for console timing.
Always validate a release ROM in an independent emulator and on real hardware.

## Requirements

- [CMake](https://cmake.org/) 3.24 or newer;
- a C++23 compiler;
- [SDL3](https://wiki.libsdl.org/SDL3/FrontPage) installed where CMake can find
  its package configuration;
- Git and network access during the first configure, while CMake fetches
  `yaml-cpp`, `zlib` and `libpng`.

The project builds on macOS and other desktop platforms supported by its
dependencies. Compiler and SDL3 availability ultimately determine whether a
particular host configuration is supported.

## Build and test

```bash
git clone https://github.com/RuiNelson/MegaDriveEnvironment.git
cd MegaDriveEnvironment

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

This repository builds a static library; it is normally embedded in a game or
tool rather than launched by itself. To see a complete runnable project:

```bash
cd ..
git clone https://github.com/RuiNelson/MegaDriveEnvironmentSampleGame.git
cd MegaDriveEnvironmentSampleGame
./build_pc.sh
./run_pc.sh
```

Keep the two repositories side by side for the sample's default configuration,
or set its `MEGADRIVE_ENVIRONMENT_DIR` variable.

## Add it to a CMake project

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyMegaDriveGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    "../MegaDriveEnvironment"
    "${CMAKE_BINARY_DIR}/MegaDriveEnvironment"
)

add_executable(my_game src/main.cpp)
target_link_libraries(my_game PRIVATE
    MegaDriveEnvironment::MegaDriveEnvironment
)
```

Public headers are included from `include/MegaDriveEnvironment`:

```cpp
#include "system/MegaDriveEnvironment.hpp"
#include "system/graphics/VDP.hpp"
#include "system/memory/SystemMemory.hpp"
```

### Minimal host application

Derive one application object from `MegaDriveEnvironment`, choose the VDP
timing/scaling policy, implement `run()`, and call `boot()` on the main thread:

```cpp
#include "system/MegaDriveEnvironment.hpp"

class MyGame final : public MegaDriveEnvironment {
  public:
    MyGame()
        : MegaDriveEnvironment(
              VDP::InternalTimer,
              VDP::Integer,
              VDP::HardwareSpriteLimit) {
    }

  private:
    void run() override {
        // run() executes on the environment's CPU thread.
        while (!shouldQuit()) {
            // Update game state and access mapped hardware here.
            pace();
        }
    }
};

int main() {
    MyGame game;
    game.boot(); // blocks while the SDL event loop and game are running
}
```

`boot()` owns the runtime lifecycle: it starts the VDP, audio and Z80, runs
`run()` on a dedicated CPU thread, and keeps SDL event processing and frame
presentation on the calling thread. Closing the window or pressing `Ctrl+Q`
sets `shouldQuit()`; cooperative game loops should observe it and return.

For a practical VBlank-driven loop, VDP initialization, assets and a portable
memory adapter, use the Sample Game rather than growing the minimal snippet
into a second tutorial.

## Runtime architecture

```mermaid
classDiagram
    direction TB

    class MegaDriveEnvironment {
        <<abstract>>
        +boot()
        +shouldQuit()
        +memory()
        +vdp()
        +controllers()
        +z80()
        +sound()
        #cpu()
        #loadROM(path)
        #run()
        #vSync()
        #hSync(line)
        #handleOptionHotkey(keyCode)
    }

    class CPU68K {
        +d[8]
        +a[7]
        +ssp
        +usp
        +pc
        +sr
        +condition(cc)
    }

    class SystemMemory {
        +loadROM(path)
        +readByte(address)
        +readWord(address)
        +readLong(address)
        +writeByte(address, value)
        +writeWord(address, value)
        +writeLong(address, value)
    }

    class VDP {
        +start()
        +stop()
        +writeControlPort(value)
        +readControlPort()
        +writeDataPort(value)
        +readDataPort()
        +readHVCounter()
        +popInterrupt(out)
        +dumpFrameBufferToPNG(path, fullRange)
        +dumpEverythingToPNG(path, fullRange)
    }

    class Controllers {
        +getCurrentState()
        +setDelegate(delegate)
        +readPlayer1DataPort()
        +readPlayer2DataPort()
        +writePlayer1DataPort(value)
        +writePlayer2DataPort(value)
    }

    class ControllersDelegate {
        <<interface>>
        +controllersStateDidUpdate(newState)
    }

    class ControlsConfigStore {
        +player1
        +player2
        +save()
    }

    class Z80 {
        +start()
        +stop()
        +readRAMFor68K(address)
        +writeRAMFor68K(address, value)
        +setBusRequest(requested)
        +setReset(held)
        +pulseVBlankIRQ()
    }

    class Sound {
        +start()
        +stop()
        +readYM2612(port)
        +writeYM2612(port, value)
        +writePSG(value)
        +diagnostics()
    }

    class VDPState {
        +regs
        +vram
        +cram
        +vsram
        +reset()
    }

    class VDPPort {
        +writeControlPort(value)
        +readControlPort()
        +writeDataPort(value)
        +readDataPort()
        +executeDMA()
    }

    class VDPTile {
        +getTilePixel(tileAddr, pixelX, pixelY, hflip, vflip)
        +cramToRGB(palette, colorIndex, r, g, b)
    }

    class VDPRenderer {
        +renderFrame()
        +renderScanline(line)
    }

    class VDPRendererDebug {
        +dumpFrameBufferToPNG(path, fullRange)
        +dumpEverythingToPNG(path, fullRange)
    }

    class Framebuffer {
        +setPixel(x, y, b, g, r)
        +getPixel(x, y, b, g, r)
        +clear()
        +uploadToTexture(texture)
    }

    MegaDriveEnvironment "1" *-- "1" CPU68K : register file
    MegaDriveEnvironment "1" *-- "1" SystemMemory : owns
    MegaDriveEnvironment "1" *-- "1" VDP : owns
    MegaDriveEnvironment "1" *-- "1" Controllers : owns
    MegaDriveEnvironment "1" *-- "1" Z80 : owns
    MegaDriveEnvironment "1" *-- "1" Sound : owns

    SystemMemory ..> VDP : mapped VDP ports
    SystemMemory ..> Controllers : mapped joypad ports
    SystemMemory ..> Z80 : RAM and bus control
    SystemMemory ..> Sound : mapped audio ports
    Z80 ..> SystemMemory : banked 68K and VDP access
    Z80 ..> Sound : YM2612 and PSG writes
    VDP ..> MegaDriveEnvironment : raises 68K IRQs
    VDP ..> Z80 : raises VBlank IRQ

    Controllers o-- ControllersDelegate : optional observer
    Controllers ..> ControlsConfigStore : reads at construction

    VDP "1" *-- "1" VDPState : hardware state
    VDP "1" *-- "1" VDPPort : port and DMA logic
    VDP "1" *-- "1" VDPTile : tile decoding
    VDP "1" *-- "1" VDPRenderer : frame composition
    VDP "1" *-- "1" VDPRendererDebug : PNG diagnostics
    VDP "1" *-- "1" Framebuffer : pixel output

    VDPPort --> VDPState : reads and mutates
    VDPPort ..> SystemMemory : DMA source
    VDPTile --> VDPState : reads VRAM and CRAM
    VDPRenderer --> VDPState : reads display state
    VDPRenderer --> VDPTile : decodes pixels
    VDPRenderer --> Framebuffer : draws frame
    VDPRendererDebug --> VDPState : reads diagnostics
    VDPRendererDebug --> VDPTile : decodes diagnostics
    VDPRendererDebug --> Framebuffer : reads output
```

The filled diamonds correspond to by-value ownership in the headers. Dotted
arrows show runtime calls between classes; the VDP helper classes are private
implementation details composed inside `VDP`. `MegaDriveEnvironment` exposes
its top-level subsystems to derived applications through these accessors:

```cpp
memory();      // SystemMemory
vdp();         // VDP
controllers(); // Controllers
z80();         // Z80
sound();       // Sound
```

The important threading rules are:

- `run()` executes on the CPU thread.
- SDL events, window presentation and `handleOptionHotkey()` execute on the
  main thread.
- VDP rendering and interrupt scheduling execute on the VDP thread.
- The Z80 has its own execution thread; audio rendering is driven by SDL's
  audio callback.
- Public mapped-memory, VDP-port and controller operations are synchronized.
  Your own game objects are not automatically thread-safe.
- If an option hotkey or callback touches CPU-thread state, add explicit
  synchronization or send a message to the CPU thread.

## Memory and hardware access

`SystemMemory` is the central compatibility boundary. It normalizes addresses
to 24 bits, preserves Motorola big-endian ordering and routes mapped accesses
to the owning subsystem:

```cpp
auto &bus = memory();

bus.writeWord(0x00C00004, 0x8174); // VDP control-port write
const auto status = bus.readWord(0x00C00004);
const auto p1 = bus.readByte(0x00A10003);
```

For code intended to run on both PC and real hardware, prefer a small shared
memory contract whose PC implementation delegates to `SystemMemory` and whose
Mega Drive implementation performs direct volatile bus access. That keeps game
code independent of SDL and prevents a second hardware-specific game loop.

Key mapped ranges:

| Address | Behaviour |
| --- | --- |
| `$000000–$3FFFFF` | Loaded ROM image, read-only after `loadROM()` |
| `$A00000–$A01FFF` | Z80 RAM as observed by the 68000 |
| `$A04000–$A04003` | YM2612 ports |
| `$A10001` | Hardware version / region pins |
| `$A10003–$A1000B` | Player data and control ports |
| `$A11100`, `$A11200` | Z80 bus request and reset |
| `$C00000–$C0000F` | VDP data, control/status and H/V-counter ports |
| odd bytes `$C00011–$C00017` | PSG writes |
| `$FF0000–$FFFFFF` | 64 KiB 68000 Work RAM |

`loadROM(path)` accepts up to 4 MiB. Larger files are reported and truncated;
an unreadable file is reported and leaves the existing ROM contents unchanged.

### Host memory is not console memory

Native C++ objects, the host heap and the PC thread stack do **not** consume
emulated Work RAM. This is useful for development, but it can hide real-hardware
stack overflow or memory-budget errors. Portable projects should establish
explicit budgets for the 64 KiB Work RAM, avoid accidental dynamic allocation
in shared code, and test the final target early. The
[Sample Game README](https://github.com/RuiNelson/MegaDriveEnvironmentSampleGame#memory-management-and-the-64-kib-work-ram-budget)
contains a more detailed discussion of stack, pools and decompression buffers.

## VDP and frame timing

The VDP can be reached through the memory map or directly through `vdp()`. Use
mapped access when sharing the code with a Mega Drive build; use the direct API
for environment-specific tools and diagnostics.

### Synchronization modes

| Mode | Use |
| --- | --- |
| `VDP::InternalTimer` | Recommended for games. Runs from the selected region's internal 60 Hz or 50 Hz deadline and is independent of monitor refresh rate. |
| `VDP::VSync` | Presents one emulated frame per monitor refresh. Useful when that refresh rate is already appropriate. |
| `VDP::VSync2` | Holds each frame for two monitor refreshes. |
| `VDP::VSync3` | Holds each frame for three monitor refreshes. |

Select region pins before `boot()` when needed:

```cpp
game.setLanguagePin(MegaDriveEnvironment::LanguagePin::Overseas);
game.setVideoStandard(MegaDriveEnvironment::VideoStandard::Hz50);
```

### Scaling modes

- `VDP::Integer`: largest integer scale that fits the usable display;
- `VDP::Fit`: resizable fitted output with bilinear filtering;
- `VDP::Scale1x`, `Scale2x`, `Scale3x`: fixed initial scales with nearest-neighbour
  filtering.

`VDP::HardwareSpriteLimit` clips excess sprites like the hardware while still
reporting overflow state. `VDP::QuasiUnlimitedSprites` is useful for debugging
sprite tables without hiding entries behind the per-line limit.

## Controllers and configuration

`Controllers` loads `controls.yaml` once during construction. If the file is
missing or malformed, Player 1 uses the defaults below and Player 2 is disabled.

| Mega Drive input | Default keyboard input |
| --- | --- |
| D-pad | Arrow keys |
| A | `Z` |
| B | `X` |
| C | `C` |
| Start | `V` |

Games intended for real hardware should read the active-low controller data
ports and drive the TH line exactly as they would on the console. The
`Controllers` class translates configured keyboard/gamepad state into that
3-button protocol at `$A10003/$A10005`.

Applications can expose the built-in configuration UI:

```cpp
#include "config/controls/ControlsConfigUI.hpp"
#include <string_view>

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--config-controls") {
        runControlsConfig(); // blocks, then saves controls.yaml on Exit
        return 0;
    }

    MyGame game;
    game.boot();
}
```

Run the application from a stable working directory: `controls.yaml` and PNG
captures are stored relative to that directory. Gamepad bindings persist by
GUID and are resolved to the current SDL session when the environment starts.

## Sound and Z80

The mapped sound path mirrors the console architecture:

- 68000 or Z80 writes to `$A04000–$A04003` reach the YM2612;
- odd-byte writes in `$C00010–$C00017` reach the PSG;
- Z80 RAM, bus request, reset and VBlank IRQ are modelled;
- writes from different producers are stamped on a shared master-cycle
  timeline and consumed by the audio renderer;
- gameplay-facing audio writes do not block the producer thread once real-time
  audio is active. Under contention or queue pressure, diagnostics report
  dropped events rather than stalling the game.

Environment-only code may call `sound().writeYM2612()` and
`sound().writePSG()` directly. Shared game code should use memory-mapped ports
so the same sound routine remains valid on real hardware.

## Debugging tools

Because the executable is native, launch it directly under LLDB, GDB or your
IDE. CMake exports `compile_commands.json`, which language servers can consume.

Built-in host shortcuts:

| Shortcut | Action |
| --- | --- |
| `Ctrl+Q` | Request shutdown |
| `Ctrl+P` | Save the composed frame as `screenshot_NNN.png` |
| `Ctrl+S` | Save the full VDP diagnostic sheet as `vpd_NNN.png` |
| `Alt/Option` + key or gamepad button | Call `handleOptionHotkey()` without changing emulated joypad state |

The full VDP sheet contains the frame, tile sheets, plane name tables, palette,
VSRAM, window/sprite views and registers. The mouse cursor is hidden while the
VDP window is fullscreen and restored on exit.

Additional runtime controls:

```cpp
game.setDebugLog(true); // once-per-second runtime/audio diagnostics
game.setFastMode(true); // skip CPU-side pacing for bring-up and validation
```

Fast mode is intentionally not timing-accurate. Use it to find logic bugs, not
to judge play speed, raster effects or audio behaviour.

## Portability to real hardware

MegaDriveEnvironment makes shared code practical; it cannot make host-only C++
portable automatically. Keep the boundary explicit:

**Usually shared**

- game state and object model;
- collision, movement and scoring;
- tile/sprite/palette command generation;
- controller protocol and sound command logic;
- a target-neutral memory interface.

**Target-specific**

- PC `main()` and `MegaDriveEnvironment` adapter;
- Mega Drive reset/header/interrupt startup;
- host synchronization versus hardware busy-waiting;
- direct bus access and linker/ROM layout;
- freestanding runtime, allocation policy and memory initialization.

The Sample Game compiles the same gameplay and VDP code for both targets and is
the recommended starting point for a new project.

## Public API map

| Header | Purpose |
| --- | --- |
| [`system/MegaDriveEnvironment.hpp`](include/MegaDriveEnvironment/system/MegaDriveEnvironment.hpp) | Lifecycle, subsystem ownership, region, pacing and host hooks |
| [`system/memory/SystemMemory.hpp`](include/MegaDriveEnvironment/system/memory/SystemMemory.hpp) | 24-bit bus, ROM/Work RAM and mapped-device routing |
| [`system/graphics/VDP.hpp`](include/MegaDriveEnvironment/system/graphics/VDP.hpp) | VDP ports, synchronization, scaling, interrupts and PNG diagnostics |
| [`system/controllers/Controllers.hpp`](include/MegaDriveEnvironment/system/controllers/Controllers.hpp) | Input state and 3-button port protocol |
| [`system/sound/Sound.hpp`](include/MegaDriveEnvironment/system/sound/Sound.hpp) | YM2612/PSG access and audio diagnostics |
| [`system/z80/Z80.hpp`](include/MegaDriveEnvironment/system/z80/Z80.hpp) | Z80 RAM, bus/reset and execution lifecycle |
| [`config/controls/ControlsConfigUI.hpp`](include/MegaDriveEnvironment/config/controls/ControlsConfigUI.hpp) | Interactive keyboard/gamepad binding UI |
| [`system/cpu/CPU68K.hpp`](include/MegaDriveEnvironment/system/cpu/CPU68K.hpp) | Register and condition-code model for generated code |

## Repository layout

```text
include/MegaDriveEnvironment/  Public consumer headers
src/                           Runtime implementation
assets/                        Example/source art
docs/                          Supporting project documentation
tests/                         Focused automated tests
tools/                         Asset and development utilities
```

When changing emulated behaviour, add a focused test where possible and verify
the result against independent emulator documentation or real hardware. The
fast native loop is most valuable when it remains honest about where hardware
validation still matters.
