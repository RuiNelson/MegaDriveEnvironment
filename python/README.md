# MegaDriveEnvironment remote client for Python

Typed, dependency-free access to a running `MegaDriveEnvironment` over its
binary TCP automation protocol.

## Installation

From the `MegaDriveEnvironment` checkout:

```bash
python3 -m pip install ./python
```

For development without installing:

```bash
PYTHONPATH=python/src python3 your_script.py
```

Python 3.9 or newer is required.

## Quick start

```python
from megadrive_remote import Buttons, MegaDriveClient, TilemapPlane

with MegaDriveClient("127.0.0.1", 6969) as mega_drive:
    mega_drive.ping()
    mega_drive.restart_game()
    print(mega_drive.get_game_uptime_ms())
    print(mega_drive.get_game_uptime_frames())
    print(mega_drive.get_execution_data())

    # A+B+Right on player 1, Start on player 2, for exactly three frames.
    mega_drive.press_buttons(
        player1=Buttons.A | Buttons.B | Buttons.RIGHT,
        player2=Buttons.START,
        frames=3,
    )

    # Atomic training step: freeze at a frame boundary, apply input for four
    # of twelve frames, and receive the exact final 64 KiB work-RAM snapshot.
    mega_drive.set_lockstep(True)
    step = mega_drive.step_input(
        player1=Buttons.B | Buttons.RIGHT,
        held_frames=4,
        total_frames=12,
    )
    print(step.frame, len(step.work_ram))
    mega_drive.set_lockstep(False)

    score = mega_drive.read_value(0xFF0100, width=4)
    mega_drive.write_value(0xFF0100, score + 1000, width=4)

    new_mode = mega_drive.wait_memory_changed(
        0xFFFF00,
        width=2,
        timeout_ms=5_000,
    )

    frame = mega_drive.read_framebuffer()
    print(frame.width, frame.height, frame.pixel(10, 20))
    frame.save_ppm("frame.ppm")

    palettes = mega_drive.read_palettes()
    sprites = mega_drive.read_sat()
    plane_a = mega_drive.read_tilemap(TilemapPlane.A)
    print(palettes[1].rgb, sprites[0], plane_a.at(0, 0))
```

## API overview

`MegaDriveClient` supports:

- `ping()`;
- `restart_game()`;
- `get_game_uptime_ms()`;
- `get_game_uptime_frames()`;
- `get_execution_data()` and `set_execution_data()`;
- `press_buttons()` and `release_buttons()`;
- `set_lockstep()` and `step_input()`;
- `read_memory()`, `write_memory()`, `read_value()`, and `write_value()`;
- `wait_memory_changed()` and `wait_memory_equals()`;
- `read_framebuffer()` and `read_vdp_state()`;
- `read_vram()`, `read_palettes()`, `read_sat()`, and `read_tilemap()`;
- `wait_vsync()`, `wait_hsync_count()`, and `wait_hsync_reach_line()`.

Every method validates addresses, widths, lengths and timeout values before
sending a request. Blocking commands automatically allow the socket slightly
more time than the server-side timeout.

The client is safe to share between Python threads, but requests are serialized
because the server permits one in-flight command. A single connection cannot
send a second command to satisfy a wait already in progress; the running game
or another in-process producer must cause the awaited change.

`set_lockstep(True)` is intended for training and deterministic automation. It
returns only when execution has stopped at a complete-frame boundary.
`step_input()` then advances an exact number of frames and returns a
`StepResult(frame, work_ram)` in one response. Always disable lockstep before
returning control to a human; disconnect and cold restart also disable it.

## Errors

Server `NAK` replies raise `ServerError`. A server-side wait timeout raises the
more specific `RemoteTimeoutError`, which is also a Python `TimeoutError`.
Malformed responses raise `ProtocolError`, and premature disconnects raise
`ConnectionClosedError`.

```python
from megadrive_remote import MegaDriveClient, RemoteTimeoutError

with MegaDriveClient() as mega_drive:
    try:
        mega_drive.wait_memory_equals(
            0xFF0000,
            0x42,
            timeout_ms=250,
        )
    except RemoteTimeoutError as error:
        print(error.code, error.message)
```

The underlying wire format remains documented in
[`../docs/remote-access-protocol.md`](../docs/remote-access-protocol.md).
