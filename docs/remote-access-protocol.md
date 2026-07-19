# Remote access protocol

`MegaDriveEnvironment` exposes a single-client binary TCP automation service.
It listens on every network interface, on port `6969` by default. Pass a
different fourth constructor argument to `MegaDriveEnvironment`, or `0` to
disable the service.

The server is strictly request/reply: it never sends unsolicited events. All
integer fields are unsigned big-endian unless explicitly marked signed.

For Python applications, the typed client in [`../python/`](../python/) wraps
all framing, validation and VDP record decoding.

## Framing

Every request consists of this 12-byte header, its payload, and one `ETX` byte:

| Offset | Size | Value |
|---:|---:|---|
| 0 | 1 | `SOH` (`0x01`) |
| 1 | 1 | protocol version (`0x01`) |
| 2 | 1 | command |
| 3 | 1 | flags; must be zero |
| 4 | 4 | request ID |
| 8 | 4 | payload length |
| 12 | N | payload |
| 12+N | 1 | `ETX` (`0x03`) |

A response uses the same layout and echoes the command and request ID. Byte 0
is `ACK` (`0x06`) on success or `NAK` (`0x15`) on failure. A `NAK` payload is a
two-byte error code followed by an optional UTF-8 explanation.

Malformed framing closes the connection. A well-framed request with invalid
arguments receives `NAK`. The maximum payload is 16 MiB.

Error codes are: `1` malformed payload, `2` unknown command, `3` timeout, `4`
invalid argument, `5` unavailable, `6` too large, and `7` internal error.

## Commands

Reserved fields must be zero. Timeouts use host milliseconds. Bus addresses
are 32-bit fields but must fit the Mega Drive's 24-bit address space.

| Command | Name | Request payload | Successful response |
|---:|---|---|---|
| `00` | `PING` | empty | empty |
| `10` | `PRESS_BUTTONS` | P1 mask:u8, P2 mask:u8, frames:u32, timeout-ms:u32 | empty after buttons are released |
| `11` | `RELEASE_BUTTONS` | empty | empty |
| `20` | `READ_MEMORY` | address:u32, length:u32 | raw bytes |
| `21` | `WRITE_MEMORY` | address:u32, raw bytes | empty |
| `22` | `WAIT_MEMORY_CHANGED` | address:u32, width:u8, reserved:3, timeout-ms:u32 | observed value:u32 |
| `23` | `WAIT_MEMORY_EQUALS` | address:u32, width:u8, reserved:3, expected:u32, mask:u32, timeout-ms:u32 | observed value:u32 |
| `30` | `READ_FRAMEBUFFER` | empty | framebuffer record below |
| `31` | `READ_VDP_STATE` | empty | raw coherent VDP snapshot below |
| `32` | `READ_VRAM` | offset:u16, length:u32 | raw VRAM bytes |
| `33` | `READ_PALETTES` | empty | decoded palette records below |
| `34` | `READ_SAT` | empty | raw and decoded sprite records below |
| `35` | `READ_TILEMAP` | plane:u8 (`0` A, `1` B, `2` window) | decoded tilemap below |
| `40` | `WAIT_VSYNC_COUNT` | count:u32, timeout-ms:u32 | empty |
| `41` | `WAIT_HSYNC_COUNT` | count:u32, timeout-ms:u32 | empty |
| `42` | `WAIT_HSYNC_REACH_LINE` | line:u16, reserved:2, timeout-ms:u32 | empty |

Button mask bits are: bit 0 Up, 1 Down, 2 Left, 3 Right, 4 A, 5 B, 6 C,
and 7 Start. Remote buttons are ORed with physical input. `PRESS_BUTTONS`
waits for the next VSync, applies both masks, holds them for exactly `frames`
complete frame intervals, releases them, and only then replies. Timeout or
disconnect also releases them.

Memory wait widths are `1`, `2`, or `4`, must be naturally aligned, and use
68000 big-endian values. `WAIT_MEMORY_CHANGED` captures its initial value when
the command starts. `WAIT_MEMORY_EQUALS` tests
`(observed & mask) == (expected & mask)`. Bus reads and writes include
memory-mapped devices and can therefore have their hardware side effects.
Writes in `000000-3FFFFF` atomically patch the in-memory cartridge image; the
ROM file is never modified. A ROM segment dump/patch holds one whole-image lock
for the complete operation, not one lock per address.

HSync waits observe every active display line and do not depend on the game's
HBlank interrupt-enable bit. `WAIT_HSYNC_REACH_LINE` waits for the next
occurrence of that zero-based line.

## VDP records

### Framebuffer

The response starts with width:u16, height:u16, pitch:u16, format:u8, and one
reserved byte. Format `1` is tightly packed BGR: each channel occupies one byte
whose value is in `0-7`. Pixel data contains `height * pitch` bytes.

### Raw VDP state

`READ_VDP_STATE` returns this summary followed by the raw arrays:

- status, H counter, V counter, active width, active height, output height,
  plane width, plane height, plane A base, plane B base, window base, window
  width, and SAT base: thirteen u16 values;
- one reserved u16;
- 24 register bytes;
- 65,536 VRAM bytes;
- 64 CRAM words;
- 40 VSRAM words;
- 640 SAT shadow bytes.

All fields and arrays belong to one coherent snapshot protected by the VDP
mutex.

### Palettes

The header is count:u16 (`64`) and record-size:u16 (`5`). Each record contains
raw-CRAM:u16, B:u8, G:u8, R:u8. Decoded channels contain native values `0-7`
and respect the VDP full-colour enable bit.

### SAT

The header is count:u16 (`80`) and record-size:u16 (`24`). Each record is:

| Field | Size |
|---|---:|
| raw SAT shadow entry | 8 |
| X, Y | signed u16 each |
| width and height in tiles | u8 each |
| link | u8 |
| reserved | u8 |
| base tile | u16 |
| palette | u8 |
| flags | u8 |
| raw tile word | u16 |
| VRAM SAT address | u16 |

Flag bit 0 is priority, bit 1 horizontal flip, and bit 2 vertical flip. The
first four SAT bytes come from the renderer's SAT shadow; tile/X fields come
from live VRAM, matching renderer behaviour.

### Tilemaps

The header is plane:u8, reserved:u8, width:u16, height:u16, base:u16,
record-size:u16 (`8`), reserved:u16. Each record contains raw-entry:u16,
tile-index:u16, palette:u8, flags:u8, and VRAM-address:u16. Flags use the same
bit assignments as SAT records. Entries are row-major.
