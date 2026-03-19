"""Data models for the HVAC Telnet integration."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(slots=True)
class HvacDescription:
    """Static HVAC metadata returned by the ESP32."""

    hvac_id: str
    protocol: str
    model: int | None
    emitter: int | None
    is_custom: bool
    profile_name: str = ""
    custom_commands: tuple[str, ...] = ()


@dataclass(slots=True)
class HvacSnapshot:
    """Current integration snapshot."""

    hvacs: dict[str, HvacDescription] = field(default_factory=dict)
    states: dict[str, dict[str, Any]] = field(default_factory=dict)
    status: dict[str, Any] = field(default_factory=dict)
    available: bool = False
