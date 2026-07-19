"""Python access to MegaDriveEnvironment's deterministic remote protocol."""

from .client import MegaDriveClient
from .exceptions import (
    ConnectionClosedError,
    ErrorCode,
    MegaDriveRemoteError,
    ProtocolError,
    RemoteTimeoutError,
    ServerError,
)
from .models import (
    Buttons,
    Framebuffer,
    PaletteEntry,
    Sprite,
    Tilemap,
    TilemapEntry,
    TilemapPlane,
    VDPState,
)

__all__ = [
    "Buttons",
    "ConnectionClosedError",
    "ErrorCode",
    "Framebuffer",
    "MegaDriveClient",
    "MegaDriveRemoteError",
    "PaletteEntry",
    "ProtocolError",
    "RemoteTimeoutError",
    "ServerError",
    "Sprite",
    "Tilemap",
    "TilemapEntry",
    "TilemapPlane",
    "VDPState",
]

__version__ = "0.1.0"
