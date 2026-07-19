"""Wire constants and strict payload decoders. Not part of the public API."""

from __future__ import annotations

from enum import IntEnum
from struct import Struct, unpack_from

from .exceptions import ProtocolError
from .models import (
    Framebuffer,
    PaletteEntry,
    Sprite,
    Tilemap,
    TilemapEntry,
    TilemapPlane,
    VDPState,
)

SOH = 0x01
ETX = 0x03
ACK = 0x06
NAK = 0x15
VERSION = 0x01
MAX_PAYLOAD = 16 * 1024 * 1024
HEADER = Struct(">BBBBII")


class Command(IntEnum):
    PING = 0x00
    PRESS_BUTTONS = 0x10
    RELEASE_BUTTONS = 0x11
    READ_MEMORY = 0x20
    WRITE_MEMORY = 0x21
    WAIT_MEMORY_CHANGED = 0x22
    WAIT_MEMORY_EQUALS = 0x23
    READ_FRAMEBUFFER = 0x30
    READ_VDP_STATE = 0x31
    READ_VRAM = 0x32
    READ_PALETTES = 0x33
    READ_SAT = 0x34
    READ_TILEMAP = 0x35
    WAIT_VSYNC_COUNT = 0x40
    WAIT_HSYNC_COUNT = 0x41
    WAIT_HSYNC_REACH_LINE = 0x42


def _expect_size(payload: bytes, expected: int, label: str) -> None:
    if len(payload) != expected:
        raise ProtocolError(f"{label} response has {len(payload)} bytes; expected {expected}")


def decode_framebuffer(payload: bytes) -> Framebuffer:
    if len(payload) < 8:
        raise ProtocolError("framebuffer response is shorter than its header")
    width, height, pitch, pixel_format, reserved = unpack_from(">HHHBB", payload)
    if pixel_format != 1 or reserved != 0:
        raise ProtocolError("unsupported framebuffer format or non-zero reserved byte")
    _expect_size(payload, 8 + height * pitch, "framebuffer")
    return Framebuffer(width, height, pitch, payload[8:])


def decode_vdp_state(payload: bytes) -> VDPState:
    expected_size = 28 + 24 + 65_536 + 64 * 2 + 40 * 2 + 640
    _expect_size(payload, expected_size, "VDP state")
    summary = unpack_from(">14H", payload)
    if summary[13] != 0:
        raise ProtocolError("VDP state reserved field is non-zero")
    offset = 28
    registers = payload[offset : offset + 24]
    offset += 24
    vram = payload[offset : offset + 65_536]
    offset += 65_536
    cram = unpack_from(">64H", payload, offset)
    offset += 64 * 2
    vsram = unpack_from(">40H", payload, offset)
    offset += 40 * 2
    sat_shadow = payload[offset : offset + 640]
    return VDPState(
        status=summary[0],
        h_counter=summary[1],
        v_counter=summary[2],
        active_width=summary[3],
        active_height=summary[4],
        output_height=summary[5],
        plane_width_cells=summary[6],
        plane_height_cells=summary[7],
        plane_a_base=summary[8],
        plane_b_base=summary[9],
        window_base=summary[10],
        window_width_cells=summary[11],
        sat_base=summary[12],
        registers=registers,
        vram=vram,
        cram=tuple(cram),
        vsram=tuple(vsram),
        sat_shadow=sat_shadow,
    )


def decode_palettes(payload: bytes) -> tuple[PaletteEntry, ...]:
    if len(payload) < 4:
        raise ProtocolError("palette response is shorter than its header")
    count, record_size = unpack_from(">HH", payload)
    if record_size != 5:
        raise ProtocolError(f"unsupported palette record size {record_size}")
    _expect_size(payload, 4 + count * record_size, "palette")
    return tuple(
        PaletteEntry(*unpack_from(">HBBB", payload, 4 + index * record_size))
        for index in range(count)
    )


def _flags(value: int) -> tuple[bool, bool, bool]:
    return bool(value & 1), bool(value & 2), bool(value & 4)


def decode_sat(payload: bytes) -> tuple[Sprite, ...]:
    if len(payload) < 4:
        raise ProtocolError("SAT response is shorter than its header")
    count, record_size = unpack_from(">HH", payload)
    if record_size != 24:
        raise ProtocolError(f"unsupported SAT record size {record_size}")
    _expect_size(payload, 4 + count * record_size, "SAT")
    result: list[Sprite] = []
    for index in range(count):
        offset = 4 + index * record_size
        raw = payload[offset : offset + 8]
        x, y, width, height, link, reserved, base_tile, palette, flags, tile_word, address = unpack_from(
            ">hhBBBBHBBHH", payload, offset + 8
        )
        if reserved != 0:
            raise ProtocolError("SAT reserved field is non-zero")
        priority, horizontal_flip, vertical_flip = _flags(flags)
        result.append(
            Sprite(
                raw_shadow=raw,
                x=x,
                y=y,
                width_tiles=width,
                height_tiles=height,
                link=link,
                base_tile=base_tile,
                palette=palette,
                priority=priority,
                horizontal_flip=horizontal_flip,
                vertical_flip=vertical_flip,
                raw_tile_word=tile_word,
                vram_address=address,
            )
        )
    return tuple(result)


def decode_tilemap(payload: bytes) -> Tilemap:
    if len(payload) < 12:
        raise ProtocolError("tilemap response is shorter than its header")
    plane_raw, reserved8, width, height, base, record_size, reserved16 = unpack_from(">BBHHHHH", payload)
    if reserved8 != 0 or reserved16 != 0:
        raise ProtocolError("tilemap reserved field is non-zero")
    if record_size != 8:
        raise ProtocolError(f"unsupported tilemap record size {record_size}")
    try:
        plane = TilemapPlane(plane_raw)
    except ValueError as error:
        raise ProtocolError(f"unknown tilemap plane {plane_raw}") from error
    count = width * height
    _expect_size(payload, 12 + count * record_size, "tilemap")
    entries: list[TilemapEntry] = []
    for index in range(count):
        raw, tile_index, palette, flags, address = unpack_from(">HHBBH", payload, 12 + index * record_size)
        priority, horizontal_flip, vertical_flip = _flags(flags)
        entries.append(
            TilemapEntry(
                raw=raw,
                tile_index=tile_index,
                palette=palette,
                priority=priority,
                horizontal_flip=horizontal_flip,
                vertical_flip=vertical_flip,
                vram_address=address,
            )
        )
    return Tilemap(plane, width, height, base, tuple(entries))
