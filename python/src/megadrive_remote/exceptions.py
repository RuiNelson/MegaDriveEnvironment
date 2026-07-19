"""Exceptions raised by :mod:`megadrive_remote`."""

from __future__ import annotations

from enum import IntEnum


class ErrorCode(IntEnum):
    """Error codes returned in a NAK response."""

    MALFORMED_PAYLOAD = 1
    UNKNOWN_COMMAND = 2
    TIMEOUT = 3
    INVALID_ARGUMENT = 4
    UNAVAILABLE = 5
    TOO_LARGE = 6
    INTERNAL = 7


class MegaDriveRemoteError(Exception):
    """Base class for all client and server errors."""


class ConnectionClosedError(MegaDriveRemoteError):
    """The peer closed the TCP stream before a complete response arrived."""


class ProtocolError(MegaDriveRemoteError):
    """The server returned framing or payload data that violates the protocol."""


class ServerError(MegaDriveRemoteError):
    """A valid NAK response returned by MegaDriveEnvironment."""

    def __init__(
        self,
        code: ErrorCode | int,
        message: str,
        *,
        command: int,
        request_id: int,
    ) -> None:
        try:
            self.code: ErrorCode | int = ErrorCode(code)
        except ValueError:
            self.code = code
        self.message = message
        self.command = command
        self.request_id = request_id
        label = self.code.name if isinstance(self.code, ErrorCode) else str(self.code)
        super().__init__(f"server error {label}: {message}" if message else f"server error {label}")


class RemoteTimeoutError(ServerError, TimeoutError):
    """The server reported that a wait operation timed out."""
