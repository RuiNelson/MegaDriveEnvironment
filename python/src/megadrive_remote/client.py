"""Synchronous typed client for MegaDriveEnvironment's binary TCP server."""

from __future__ import annotations

import socket
import threading
from struct import pack, unpack
from types import TracebackType
from typing import Type, TypeVar

from ._protocol import (
    ACK,
    ETX,
    HEADER,
    MAX_PAYLOAD,
    NAK,
    SOH,
    VERSION,
    Command,
    decode_framebuffer,
    decode_palettes,
    decode_sat,
    decode_tilemap,
    decode_vdp_state,
)
from .exceptions import (
    ConnectionClosedError,
    ErrorCode,
    ProtocolError,
    RemoteTimeoutError,
    ServerError,
)
from .models import Buttons, Framebuffer, PaletteEntry, Sprite, Tilemap, TilemapPlane, VDPState

ClientT = TypeVar("ClientT", bound="MegaDriveClient")


class MegaDriveClient:
    """One connection to a running ``MegaDriveEnvironment``.

    The class is safe to call from multiple Python threads, but serializes all
    requests because the server accepts exactly one in-flight command.
    """

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 6969,
        *,
        connect_timeout: float = 5.0,
        io_timeout: float | None = 10.0,
    ) -> None:
        if not host:
            raise ValueError("host must not be empty")
        if not 1 <= port <= 65_535:
            raise ValueError("port must be in 1..65535")
        if connect_timeout <= 0:
            raise ValueError("connect_timeout must be positive")
        if io_timeout is not None and io_timeout <= 0:
            raise ValueError("io_timeout must be positive or None")
        self.host = host
        self.port = port
        self.connect_timeout = connect_timeout
        self.io_timeout = io_timeout
        self._socket: socket.socket | None = None
        self._request_id = 0
        self._lock = threading.Lock()

    @classmethod
    def from_socket(
        cls: Type[ClientT], stream: socket.socket, *, io_timeout: float | None = 10.0
    ) -> ClientT:
        """Wrap an already-connected stream socket.

        Primarily useful for test harnesses and custom transports. Ownership is
        transferred to the returned client.
        """

        client = cls(io_timeout=io_timeout)
        client._socket = stream
        stream.settimeout(io_timeout)
        return client

    @property
    def connected(self) -> bool:
        return self._socket is not None

    def connect(self: ClientT) -> ClientT:
        """Open the TCP connection and return ``self``."""

        with self._lock:
            if self._socket is None:
                stream = socket.create_connection((self.host, self.port), self.connect_timeout)
                stream.settimeout(self.io_timeout)
                self._socket = stream
        return self

    def close(self) -> None:
        """Close the connection. Calling this more than once is harmless."""

        with self._lock:
            stream, self._socket = self._socket, None
            if stream is not None:
                try:
                    stream.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                stream.close()

    def __enter__(self: ClientT) -> ClientT:
        return self.connect()

    def __exit__(
        self,
        exception_type: type[BaseException] | None,
        exception: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()

    def _next_request_id(self) -> int:
        self._request_id = (self._request_id + 1) & 0xFFFFFFFF
        return self._request_id

    def _stream(self) -> socket.socket:
        if self._socket is None:
            raise ConnectionError("client is not connected; call connect() or use a with block")
        return self._socket

    @staticmethod
    def _receive_exact(stream: socket.socket, count: int) -> bytes:
        data = bytearray(count)
        view = memoryview(data)
        offset = 0
        while offset < count:
            received = stream.recv_into(view[offset:])
            if received == 0:
                raise ConnectionClosedError("server closed the connection during a response")
            offset += received
        return bytes(data)

    def _request(
        self,
        command: Command,
        payload: bytes = b"",
        *,
        operation_timeout_ms: int | None = None,
    ) -> bytes:
        if len(payload) > MAX_PAYLOAD:
            raise ValueError(f"payload exceeds the {MAX_PAYLOAD}-byte protocol limit")
        with self._lock:
            stream = self._stream()
            request_id = self._next_request_id()
            frame = HEADER.pack(SOH, VERSION, command, 0, request_id, len(payload)) + payload + bytes((ETX,))
            previous_timeout = stream.gettimeout()
            if operation_timeout_ms is not None:
                margin_timeout = operation_timeout_ms / 1000.0 + 1.0
                if self.io_timeout is None:
                    request_timeout: float | None = None
                else:
                    request_timeout = max(self.io_timeout, margin_timeout)
                stream.settimeout(request_timeout)
            try:
                stream.sendall(frame)
                response_header = self._receive_exact(stream, HEADER.size)
                marker, version, response_command, flags, response_id, response_size = HEADER.unpack(response_header)
                if version != VERSION or flags != 0:
                    raise ProtocolError("response has unsupported version or non-zero flags")
                if response_command != command or response_id != request_id:
                    raise ProtocolError("response command or request ID does not match the request")
                if marker not in (ACK, NAK):
                    raise ProtocolError(f"response begins with unknown marker 0x{marker:02X}")
                if response_size > MAX_PAYLOAD:
                    raise ProtocolError("response exceeds the protocol payload limit")
                response = self._receive_exact(stream, response_size)
                if self._receive_exact(stream, 1)[0] != ETX:
                    raise ProtocolError("response is missing its ETX footer")
            finally:
                if operation_timeout_ms is not None and self._socket is stream:
                    stream.settimeout(previous_timeout)
            if marker == NAK:
                if len(response) < 2:
                    raise ProtocolError("NAK response is missing its two-byte error code")
                raw_code = unpack(">H", response[:2])[0]
                message = response[2:].decode("utf-8", errors="replace")
                error_type = RemoteTimeoutError if raw_code == ErrorCode.TIMEOUT else ServerError
                raise error_type(raw_code, message, command=command, request_id=request_id)
            return response

    @staticmethod
    def _positive(value: int, label: str) -> int:
        if not isinstance(value, int) or value <= 0:
            raise ValueError(f"{label} must be a positive integer")
        return value

    @staticmethod
    def _address(address: int) -> int:
        if not isinstance(address, int) or not 0 <= address <= 0xFFFFFF:
            raise ValueError("address must fit the 24-bit system bus")
        return address

    @staticmethod
    def _width(width: int) -> int:
        if width not in (1, 2, 4):
            raise ValueError("width must be 1, 2, or 4")
        return width

    def ping(self) -> None:
        self._request(Command.PING)

    def press_buttons(
        self,
        *,
        player1: Buttons | int = Buttons.NONE,
        player2: Buttons | int = Buttons.NONE,
        frames: int = 1,
        timeout_ms: int | None = None,
    ) -> None:
        """Hold controller masks from the next VSync for ``frames`` frames."""

        frames = self._positive(frames, "frames")
        if not 0 <= int(player1) <= 0xFF or not 0 <= int(player2) <= 0xFF:
            raise ValueError("button masks must fit one byte")
        if timeout_ms is None:
            timeout_ms = max(1_000, frames * 50 + 1_000)
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        payload = pack(">BBII", int(player1), int(player2), frames, timeout_ms)
        self._request(Command.PRESS_BUTTONS, payload, operation_timeout_ms=timeout_ms)

    def release_buttons(self) -> None:
        self._request(Command.RELEASE_BUTTONS)

    def read_memory(self, address: int, length: int) -> bytes:
        address = self._address(address)
        if not isinstance(length, int) or length < 0 or address + length > 0x1000000:
            raise ValueError("memory range must fit the 24-bit system bus")
        if length > MAX_PAYLOAD:
            raise ValueError("memory read exceeds the protocol payload limit")
        return self._request(Command.READ_MEMORY, pack(">II", address, length))

    def write_memory(self, address: int, data: bytes | bytearray | memoryview) -> None:
        address = self._address(address)
        raw = bytes(data)
        if address + len(raw) > 0x1000000:
            raise ValueError("memory range must fit the 24-bit system bus")
        if len(raw) > MAX_PAYLOAD - 4:
            raise ValueError("memory write exceeds the protocol payload limit")
        self._request(Command.WRITE_MEMORY, pack(">I", address) + raw)

    def read_value(self, address: int, width: int = 1) -> int:
        width = self._width(width)
        return int.from_bytes(self.read_memory(address, width), "big")

    def write_value(self, address: int, value: int, width: int = 1) -> None:
        width = self._width(width)
        if not isinstance(value, int) or not 0 <= value < 1 << (width * 8):
            raise ValueError("value does not fit width")
        self.write_memory(address, value.to_bytes(width, "big"))

    def wait_memory_changed(self, address: int, *, width: int = 1, timeout_ms: int = 1_000) -> int:
        address = self._address(address)
        width = self._width(width)
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        if address % width:
            raise ValueError("address is not naturally aligned for width")
        response = self._request(
            Command.WAIT_MEMORY_CHANGED,
            pack(">IB3xI", address, width, timeout_ms),
            operation_timeout_ms=timeout_ms,
        )
        if len(response) != 4:
            raise ProtocolError("memory wait response must contain one u32")
        return unpack(">I", response)[0]

    def wait_memory_equals(
        self,
        address: int,
        expected: int,
        *,
        width: int = 1,
        mask: int | None = None,
        timeout_ms: int = 1_000,
    ) -> int:
        address = self._address(address)
        width = self._width(width)
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        if address % width:
            raise ValueError("address is not naturally aligned for width")
        maximum = (1 << (width * 8)) - 1
        if mask is None:
            mask = maximum
        if not 0 <= expected <= maximum or not 0 <= mask <= maximum:
            raise ValueError("expected and mask must fit width")
        response = self._request(
            Command.WAIT_MEMORY_EQUALS,
            pack(">IB3xIII", address, width, expected, mask, timeout_ms),
            operation_timeout_ms=timeout_ms,
        )
        if len(response) != 4:
            raise ProtocolError("memory wait response must contain one u32")
        return unpack(">I", response)[0]

    def read_framebuffer(self) -> Framebuffer:
        return decode_framebuffer(self._request(Command.READ_FRAMEBUFFER))

    def read_vdp_state(self) -> VDPState:
        return decode_vdp_state(self._request(Command.READ_VDP_STATE))

    def read_vram(self, offset: int = 0, length: int = 65_536) -> bytes:
        if not isinstance(offset, int) or not 0 <= offset <= 0xFFFF:
            raise ValueError("VRAM offset must be in 0..65535")
        if not isinstance(length, int) or length < 0 or offset + length > 65_536:
            raise ValueError("VRAM range is invalid")
        return self._request(Command.READ_VRAM, pack(">HI", offset, length))

    def read_palettes(self) -> tuple[PaletteEntry, ...]:
        return decode_palettes(self._request(Command.READ_PALETTES))

    def read_sat(self) -> tuple[Sprite, ...]:
        return decode_sat(self._request(Command.READ_SAT))

    def read_tilemap(self, plane: TilemapPlane | int) -> Tilemap:
        try:
            selected = TilemapPlane(plane)
        except ValueError as error:
            raise ValueError("plane must be TilemapPlane.A, B, or WINDOW") from error
        return decode_tilemap(self._request(Command.READ_TILEMAP, bytes((selected,))))

    def wait_vsync(self, count: int = 1, *, timeout_ms: int | None = None) -> None:
        count = self._positive(count, "count")
        if timeout_ms is None:
            timeout_ms = max(1_000, count * 50 + 500)
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        self._request(
            Command.WAIT_VSYNC_COUNT,
            pack(">II", count, timeout_ms),
            operation_timeout_ms=timeout_ms,
        )

    def wait_hsync_count(self, count: int = 1, *, timeout_ms: int = 1_000) -> None:
        count = self._positive(count, "count")
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        self._request(
            Command.WAIT_HSYNC_COUNT,
            pack(">II", count, timeout_ms),
            operation_timeout_ms=timeout_ms,
        )

    def wait_hsync_reach_line(self, line: int, *, timeout_ms: int = 1_000) -> None:
        if not isinstance(line, int) or not 0 <= line < 480:
            raise ValueError("line must be in 0..479")
        timeout_ms = self._positive(timeout_ms, "timeout_ms")
        self._request(
            Command.WAIT_HSYNC_REACH_LINE,
            pack(">H2xI", line, timeout_ms),
            operation_timeout_ms=timeout_ms,
        )
