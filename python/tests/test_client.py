from __future__ import annotations

import socket
import threading
import unittest
from collections.abc import Callable, Iterable
from struct import pack, unpack

from megadrive_remote import (
    Buttons,
    MegaDriveClient,
    ProtocolError,
    RemoteTimeoutError,
    TilemapPlane,
)

SOH = 0x01
ETX = 0x03
ACK = 0x06
NAK = 0x15
VERSION = 1
HEADER_SIZE = 12

Handler = Callable[[int, bytes], tuple[int, bytes]]


def receive_exact(stream: socket.socket, count: int) -> bytes:
    result = bytearray()
    while len(result) < count:
        chunk = stream.recv(count - len(result))
        if not chunk:
            raise EOFError
        result.extend(chunk)
    return bytes(result)


def serve(stream: socket.socket, handlers: Iterable[Handler]) -> None:
    try:
        for handler in handlers:
            header = receive_exact(stream, HEADER_SIZE)
            marker, version, command, flags, request_id, length = unpack(">BBBBII", header)
            assert marker == SOH and version == VERSION and flags == 0
            payload = receive_exact(stream, length)
            assert receive_exact(stream, 1) == bytes((ETX,))
            response_marker, response_payload = handler(command, payload)
            stream.sendall(
                pack(
                    ">BBBBII",
                    response_marker,
                    VERSION,
                    command,
                    0,
                    request_id,
                    len(response_payload),
                )
                + response_payload
                + bytes((ETX,))
            )
    finally:
        stream.close()


class ClientHarness:
    def __init__(self, *handlers: Handler) -> None:
        client_socket, server_socket = socket.socketpair()
        self.client = MegaDriveClient.from_socket(client_socket, io_timeout=2.0)
        self.thread = threading.Thread(target=serve, args=(server_socket, handlers), daemon=True)
        self.thread.start()

    def close(self) -> None:
        self.client.close()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise AssertionError("fake server did not stop")

    def __enter__(self) -> MegaDriveClient:
        return self.client

    def __exit__(self, *_: object) -> None:
        self.close()


class MegaDriveClientTests(unittest.TestCase):
    def test_ping_memory_and_button_payloads(self) -> None:
        def ping(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x00)
            self.assertEqual(payload, b"")
            return ACK, b""

        def write(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x21)
            self.assertEqual(payload, pack(">I", 0xFF0100) + b"\x12\x34")
            return ACK, b""

        def restart(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x01)
            self.assertEqual(payload, pack(">I", 3_000))
            return ACK, b""

        def uptime(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x02, b""))
            return ACK, pack(">Q", 0xFEDCBA9876543210)

        def read(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x20)
            self.assertEqual(payload, pack(">II", 0xFF0100, 2))
            return ACK, b"\x12\x34"

        def buttons(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x10)
            self.assertEqual(
                payload,
                pack(">BBII", Buttons.A | Buttons.RIGHT, Buttons.START, 3, 2_000),
            )
            return ACK, b""

        with ClientHarness(ping, restart, uptime, write, read, buttons) as client:
            client.ping()
            client.restart_game(timeout_ms=3_000)
            self.assertEqual(client.get_game_uptime_ms(), 0xFEDCBA9876543210)
            client.write_value(0xFF0100, 0x1234, width=2)
            self.assertEqual(client.read_value(0xFF0100, width=2), 0x1234)
            client.press_buttons(
                player1=Buttons.A | Buttons.RIGHT,
                player2=Buttons.START,
                frames=3,
                timeout_ms=2_000,
            )

    def test_waits_and_server_timeout(self) -> None:
        def changed(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x22)
            self.assertEqual(payload, pack(">IB3xI", 0xFF0000, 2, 500))
            return ACK, pack(">I", 0xCAFE)

        def equals(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual(command, 0x23)
            self.assertEqual(payload, pack(">IB3xIII", 0xFF0000, 2, 0x1200, 0xFF00, 500))
            return NAK, pack(">H", 3) + b"memory wait timed out"

        with ClientHarness(changed, equals) as client:
            self.assertEqual(client.wait_memory_changed(0xFF0000, width=2, timeout_ms=500), 0xCAFE)
            with self.assertRaises(RemoteTimeoutError) as caught:
                client.wait_memory_equals(
                    0xFF0000,
                    0x1200,
                    width=2,
                    mask=0xFF00,
                    timeout_ms=500,
                )
            self.assertEqual(caught.exception.message, "memory wait timed out")

    def test_execution_data_get_set_and_clear(self) -> None:
        initial = b'{"state":"title"}\x00'
        replacement = b"\x00\xffbinary\x10"

        def get_initial(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x03, b""))
            return ACK, initial

        def set_replacement(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x04, replacement))
            return ACK, b""

        def clear(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x04, b""))
            return ACK, b""

        with ClientHarness(get_initial, set_replacement, clear) as client:
            self.assertEqual(client.get_execution_data(), initial)
            client.set_execution_data(bytearray(replacement))
            client.set_execution_data(b"")

    def test_vram_controller_release_and_sync_payloads(self) -> None:
        expected = (
            (0x32, pack(">HI", 0x1200, 3), b"abc"),
            (0x11, b"", b""),
            (0x40, pack(">II", 2, 700), b""),
            (0x41, pack(">II", 5, 800), b""),
            (0x42, pack(">H2xI", 120, 900), b""),
        )

        def handler_for(command_expected: int, payload_expected: bytes, response: bytes) -> Handler:
            def handler(command: int, payload: bytes) -> tuple[int, bytes]:
                self.assertEqual((command, payload), (command_expected, payload_expected))
                return ACK, response

            return handler

        handlers = tuple(handler_for(*values) for values in expected)
        with ClientHarness(*handlers) as client:
            self.assertEqual(client.read_vram(0x1200, 3), b"abc")
            client.release_buttons()
            client.wait_vsync(2, timeout_ms=700)
            client.wait_hsync_count(5, timeout_ms=800)
            client.wait_hsync_reach_line(120, timeout_ms=900)

    def test_framebuffer_and_vdp_decoders(self) -> None:
        framebuffer_payload = pack(">HHHBB", 2, 1, 6, 1, 0) + bytes((1, 2, 3, 4, 5, 6))
        summary = pack(">14H", 0x3400, 1, 2, 320, 224, 224, 64, 32, 0xC000, 0xE000, 0xA000, 64, 0xF800, 0)
        state_payload = (
            summary
            + bytes(range(24))
            + bytes(65_536)
            + pack(">64H", *range(64))
            + pack(">40H", *range(40))
            + bytes(640)
        )

        def framebuffer(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x30, b""))
            return ACK, framebuffer_payload

        def state(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x31, b""))
            return ACK, state_payload

        with ClientHarness(framebuffer, state) as client:
            frame = client.read_framebuffer()
            self.assertEqual(frame.pixel(0, 0), (3, 2, 1))
            self.assertEqual(frame.pixel(1, 0), (6, 5, 4))
            self.assertEqual(frame.rgb888(), bytes((109, 73, 36, 219, 182, 146)))
            vdp = client.read_vdp_state()
            self.assertEqual(vdp.status, 0x3400)
            self.assertEqual(vdp.plane_a_base, 0xC000)
            self.assertEqual(vdp.registers, bytes(range(24)))
            self.assertEqual(vdp.cram[63], 63)

    def test_palette_sat_and_tilemap_decoders(self) -> None:
        palette_payload = pack(">HHHBBB", 1, 5, 0x0EEE, 7, 6, 5)
        raw_sat = bytes(range(8))
        sat_record = raw_sat + pack(">hhBBBBHBBHH", -10, 20, 2, 3, 4, 0, 0x123, 2, 0b111, 0xD123, 0xF800)
        sat_payload = pack(">HH", 1, 24) + sat_record
        tile_entry = pack(">HHBBH", 0xE345, 0x345, 3, 0b101, 0xC000)
        tilemap_payload = pack(">BBHHHHH", TilemapPlane.A, 0, 1, 1, 0xC000, 8, 0) + tile_entry

        def palettes(_: int, __: bytes) -> tuple[int, bytes]:
            return ACK, palette_payload

        def sat(_: int, __: bytes) -> tuple[int, bytes]:
            return ACK, sat_payload

        def tilemap(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x35, bytes((TilemapPlane.A,))))
            return ACK, tilemap_payload

        with ClientHarness(palettes, sat, tilemap) as client:
            palette = client.read_palettes()[0]
            self.assertEqual((palette.raw, palette.rgb), (0x0EEE, (5, 6, 7)))
            sprite = client.read_sat()[0]
            self.assertEqual((sprite.x, sprite.y, sprite.width_tiles), (-10, 20, 2))
            self.assertTrue(sprite.priority and sprite.horizontal_flip and sprite.vertical_flip)
            tilemap_result = client.read_tilemap(TilemapPlane.A)
            entry = tilemap_result.at(0, 0)
            self.assertEqual((entry.tile_index, entry.palette), (0x345, 3))
            self.assertTrue(entry.priority and entry.vertical_flip)
            self.assertFalse(entry.horizontal_flip)

    def test_rejects_malformed_response(self) -> None:
        def malformed(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x30, b""))
            return ACK, b"too short"

        with ClientHarness(malformed) as client:
            with self.assertRaises(ProtocolError):
                client.read_framebuffer()

    def test_rejects_malformed_uptime_response(self) -> None:
        def malformed(command: int, payload: bytes) -> tuple[int, bytes]:
            self.assertEqual((command, payload), (0x02, b""))
            return ACK, b"\x00" * 7

        with ClientHarness(malformed) as client:
            with self.assertRaisesRegex(ProtocolError, "one u64"):
                client.get_game_uptime_ms()


if __name__ == "__main__":
    unittest.main()
