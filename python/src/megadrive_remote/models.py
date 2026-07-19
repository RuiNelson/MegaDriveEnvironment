"""Typed values returned by the MegaDriveEnvironment remote client."""

from __future__ import annotations

from dataclasses import dataclass
from enum import IntEnum, IntFlag
from pathlib import Path


class Buttons(IntFlag):
    """Mega Drive controller buttons; values can be combined with ``|``."""

    NONE = 0
    UP = 1 << 0
    DOWN = 1 << 1
    LEFT = 1 << 2
    RIGHT = 1 << 3
    A = 1 << 4
    B = 1 << 5
    C = 1 << 6
    START = 1 << 7


class TilemapPlane(IntEnum):
    """VDP nametable selected by :meth:`MegaDriveClient.read_tilemap`."""

    A = 0
    B = 1
    WINDOW = 2


@dataclass(frozen=True)
class Framebuffer:
    """Packed native-colour VDP framebuffer."""

    width: int
    height: int
    pitch: int
    bgr: bytes

    def __post_init__(self) -> None:
        if self.width <= 0 or self.height <= 0 or self.pitch < self.width * 3:
            raise ValueError("invalid framebuffer dimensions")
        if len(self.bgr) != self.height * self.pitch:
            raise ValueError("framebuffer byte count does not match height * pitch")

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        """Return one pixel as ``(red, green, blue)`` with channels in 0..7."""

        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError("pixel is outside the framebuffer")
        offset = y * self.pitch + x * 3
        blue, green, red = self.bgr[offset : offset + 3]
        return red, green, blue

    def rgb888(self) -> bytes:
        """Convert native BGR 0..7 pixels to tightly packed RGB888 bytes."""

        lookup = (0, 36, 73, 109, 146, 182, 219, 255)
        result = bytearray(self.width * self.height * 3)
        destination = 0
        for y in range(self.height):
            row = y * self.pitch
            for x in range(self.width):
                blue, green, red = self.bgr[row + x * 3 : row + x * 3 + 3]
                result[destination : destination + 3] = bytes(
                    (lookup[red], lookup[green], lookup[blue])
                )
                destination += 3
        return bytes(result)

    def save_ppm(self, path: str | Path) -> None:
        """Save the framebuffer as a dependency-free binary PPM image."""

        output = Path(path)
        with output.open("wb") as stream:
            stream.write(f"P6\n{self.width} {self.height}\n255\n".encode("ascii"))
            stream.write(self.rgb888())


@dataclass(frozen=True)
class VDPState:
    """One coherent raw snapshot of VDP state and memory."""

    status: int
    h_counter: int
    v_counter: int
    active_width: int
    active_height: int
    output_height: int
    plane_width_cells: int
    plane_height_cells: int
    plane_a_base: int
    plane_b_base: int
    window_base: int
    window_width_cells: int
    sat_base: int
    registers: bytes
    vram: bytes
    cram: tuple[int, ...]
    vsram: tuple[int, ...]
    sat_shadow: bytes


@dataclass(frozen=True)
class PaletteEntry:
    """One raw CRAM word and its native 3-bit BGR interpretation."""

    raw: int
    blue: int
    green: int
    red: int

    @property
    def rgb(self) -> tuple[int, int, int]:
        return self.red, self.green, self.blue


@dataclass(frozen=True)
class Sprite:
    """One raw and decoded SAT record."""

    raw_shadow: bytes
    x: int
    y: int
    width_tiles: int
    height_tiles: int
    link: int
    base_tile: int
    palette: int
    priority: bool
    horizontal_flip: bool
    vertical_flip: bool
    raw_tile_word: int
    vram_address: int


@dataclass(frozen=True)
class TilemapEntry:
    """One decoded VDP nametable entry."""

    raw: int
    tile_index: int
    palette: int
    priority: bool
    horizontal_flip: bool
    vertical_flip: bool
    vram_address: int


@dataclass(frozen=True)
class Tilemap:
    """A row-major decoded plane or window nametable."""

    plane: TilemapPlane
    width: int
    height: int
    base_address: int
    entries: tuple[TilemapEntry, ...]

    def __post_init__(self) -> None:
        if len(self.entries) != self.width * self.height:
            raise ValueError("tilemap entry count does not match its dimensions")

    def at(self, x: int, y: int) -> TilemapEntry:
        """Return the entry at cell coordinates ``x, y``."""

        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError("cell is outside the tilemap")
        return self.entries[y * self.width + x]
