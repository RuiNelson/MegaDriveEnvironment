# MegaDriveEnvironment

`MegaDriveEnvironment` is a fast development environment for building games and
applications for the Sega Mega Drive, known as the Genesis in the US.

The goal is to drastically shorten the development loop. Instead of writing
code, compiling a ROM, opening that ROM in an emulator, testing, debugging
inside the emulator, and repeating, the game is written directly in C++ and runs
as a normal application on the local machine.

The environment provides enough of the Mega Drive hardware layer to develop and
test game logic on a modern computer:

- VDP emulation for graphics;
- sound hardware for audio;
- player controls;
- a cartridge-like boot and execution flow;
- utilities and tests for validating rendering, input, audio, and assets.

This makes it possible to use the normal tools of a modern computer during
development: `printf`, rich logs, sanitizers, profilers, debuggers with
breakpoints and stepping, memory inspection, IDEs, and everything else that is
usually slow or awkward when working only through a ROM running in an emulator.

The final goal is for the game code to be real, not disposable scaffolding. The
same logic that runs quickly in the local environment should be reusable in the
final game, with as little difference as possible between “developing on the PC”
and “running on the Mega Drive”.

## Usage Model

A game or application defines a subclass of `MegaDriveEnvironment`, implements
its entry point, and overrides the VDP interrupt handlers it needs. From there,
the code writes to the VDP, plays sound, reads controllers, and manages its own
state as it would in a cartridge, while still running as a native
operating-system process.

```cpp
#include "system/MegaDriveEnvironment.hpp"

class MyGame : public MegaDriveEnvironment {
public:
    MyGame() : MegaDriveEnvironment(VDP::VSync, VDP::Integer) {}

protected:
    void run() override {
        while (true) {
            // Main game loop: update input, game logic, and sound.
        }
    }

    void vSync() override {
        // Vertical blank: update frame state, upload graphics, etc.
    }

    void hSync(int line) override {
        // Horizontal blank for scanline-specific effects.
    }

    void handleOptionHotkey(OptionHotkeyCode keyCode) override {
        if (keyCode.source == OptionHotkeyCode::Source::Keyboard && keyCode.keyboardKey == SDLK_L) {
            // Option+L: add a life, toggle a debug overlay, etc.
        }
        if (keyCode.source == OptionHotkeyCode::Source::Gamepad &&
            keyCode.gamepadButton == SDL_GAMEPAD_BUTTON_NORTH) {
            // Option+gamepad North button: another host-only debug action.
        }
    }
};
```

Option hotkeys are delivered on the SDL main thread and do not alter the
emulated controller ports. Overrides that mutate state owned by the game/CPU
thread must synchronize that state explicitly.

## Layout

```text
include/MegaDriveEnvironment/  Public headers for games/applications
src/                           Environment implementation
assets/                        Example assets and source materials
tools/                         Development utilities
tests/                         Tests and fixtures
```

## CMake Integration

Consumer projects can include the environment as a subdirectory:

```cmake
add_subdirectory("../MegaDriveEnvironment" "${CMAKE_BINARY_DIR}/MegaDriveEnvironment")
target_link_libraries(my_game PRIVATE MegaDriveEnvironment::MegaDriveEnvironment)
```

Includes use the environment's public include root:

```cpp
#include "system/MegaDriveEnvironment.hpp"
#include "system/graphics/VDP.hpp"
#include "system/controllers/Controllers.hpp"
```
