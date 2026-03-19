"""Async client for the ESP32 HvacTelnetServer JSON API."""

from __future__ import annotations

import asyncio
import contextlib
import inspect
import json
import logging
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from typing import Any

from .models import HvacDescription

_LOGGER = logging.getLogger(__name__)

JsonValue = dict[str, Any] | list[Any]
StateCallback = Callable[[dict[str, Any]], Awaitable[None] | None]
AvailabilityCallback = Callable[[bool], Awaitable[None] | None]
ConnectedCallback = Callable[[], Awaitable[None] | None]
KEEPALIVE_INTERVAL = 10


class HvacTelnetError(Exception):
    """Base API error."""


class HvacTelnetConnectionError(HvacTelnetError):
    """Connection related error."""


class HvacTelnetCommandError(HvacTelnetError):
    """Command execution error."""


@dataclass(slots=True)
class _PendingRequest:
    matcher: Callable[[JsonValue], bool]
    future: asyncio.Future[JsonValue]


class HvacTelnetClient:
    """Persistent connection manager for HvacTelnetServer."""

    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._request_lock = asyncio.Lock()
        self._connected = asyncio.Event()
        self._stop_event = asyncio.Event()
        self._pending: _PendingRequest | None = None
        self._runner: asyncio.Task[None] | None = None
        self._keepalive_task: asyncio.Task[None] | None = None
        self._state_callback: StateCallback | None = None
        self._availability_callback: AvailabilityCallback | None = None
        self._connected_callback: ConnectedCallback | None = None

    def set_state_callback(self, callback: StateCallback) -> None:
        """Register a state update callback."""
        self._state_callback = callback

    def set_availability_callback(self, callback: AvailabilityCallback) -> None:
        """Register an availability callback."""
        self._availability_callback = callback

    def set_connected_callback(self, callback: ConnectedCallback) -> None:
        """Register a callback invoked after a successful reconnect."""
        self._connected_callback = callback

    async def async_start(self, wait_for_connection: bool = True) -> None:
        """Start the background connection loop."""
        if self._runner is not None:
            return
        self._runner = asyncio.create_task(self._connection_loop())
        if not wait_for_connection:
            return
        try:
            await asyncio.wait_for(self._connected.wait(), timeout=10)
        except TimeoutError as err:
            raise HvacTelnetConnectionError(
                f"Timed out connecting to {self._host}:{self._port}"
            ) from err

    async def async_stop(self) -> None:
        """Stop the client and close any active socket."""
        self._stop_event.set()
        self._connected.clear()
        self._fail_pending(HvacTelnetConnectionError("Connection closed"))
        if self._writer is not None:
            self._writer.close()
            with contextlib.suppress(Exception):
                await self._writer.wait_closed()
        if self._keepalive_task is not None:
            self._keepalive_task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._keepalive_task
            self._keepalive_task = None
        if self._runner is not None:
            self._runner.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._runner
            self._runner = None

    async def async_get_hvacs(self) -> dict[str, HvacDescription]:
        """Fetch configured HVAC metadata."""
        response = await self._request(
            {"cmd": "list"},
            matcher=lambda msg: isinstance(msg, dict) and bool(msg.get("ok")) and "hvacs" in msg,
        )
        hvacs: dict[str, HvacDescription] = {}
        for item in response.get("hvacs", []):
            hvac_id = str(item["id"])
            commands: list[str] = []
            custom_data = item.get("custom_data")
            if isinstance(custom_data, dict):
                for command in custom_data.get("commands", []):
                    if not isinstance(command, dict):
                        continue
                    name = str(command.get("name", "")).strip()
                    if name:
                        commands.append(name)
            hvacs[hvac_id] = HvacDescription(
                hvac_id=hvac_id,
                protocol=str(item.get("protocol", "")),
                model=item.get("model"),
                emitter=item.get("emitter"),
                is_custom=bool(item.get("custom", False)),
                profile_name=str(item.get("profile_name", "")).strip(),
                custom_commands=tuple(commands),
            )
        return hvacs

    async def async_get_all_states(self) -> dict[str, dict[str, Any]]:
        """Fetch current state for all HVACs."""
        response = await self._request(
            {"cmd": "get_all"},
            matcher=lambda msg: isinstance(msg, list),
        )
        states: dict[str, dict[str, Any]] = {}
        for item in response:
            if not isinstance(item, dict) or item.get("type") != "state" or "id" not in item:
                continue
            states[str(item["id"])] = item
        return states

    async def async_get_status(self) -> dict[str, Any]:
        """Fetch runtime and version status for diagnostics."""
        response = await self._request(
            {"cmd": "status"},
            matcher=lambda msg: (
                isinstance(msg, dict)
                and bool(msg.get("ok"))
                and str(msg.get("type", "")) == "status"
            ),
        )
        if not isinstance(response, dict):
            raise HvacTelnetCommandError("Unexpected status response")
        return response

    async def async_send(self, hvac_id: str, payload: dict[str, Any]) -> dict[str, Any]:
        """Send a state-changing command."""
        full_payload = {"cmd": "send", "id": hvac_id, **payload}
        response = await self._request(
            full_payload,
            matcher=lambda msg: (
                isinstance(msg, dict)
                and msg.get("type") == "state"
                and str(msg.get("id")) == hvac_id
            ),
        )
        return response

    async def _connection_loop(self) -> None:
        """Maintain a persistent connection with automatic reconnect."""
        backoff = 1
        while not self._stop_event.is_set():
            try:
                _LOGGER.debug("Connecting to %s:%s", self._host, self._port)
                self._reader, self._writer = await asyncio.open_connection(
                    self._host,
                    self._port,
                )
                self._connected.set()
                self._start_keepalive()
                await self._notify_availability(True)
                await self._notify_connected()
                backoff = 1
                await self._read_loop()
            except asyncio.CancelledError:
                raise
            except Exception as err:
                if not self._stop_event.is_set():
                    _LOGGER.warning(
                        "Connection to %s:%s failed: %s",
                        self._host,
                        self._port,
                        err,
                    )
            finally:
                self._connected.clear()
                await self._notify_availability(False)
                self._fail_pending(HvacTelnetConnectionError("Connection lost"))
                if self._keepalive_task is not None:
                    self._keepalive_task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await self._keepalive_task
                    self._keepalive_task = None
                writer = self._writer
                self._reader = None
                self._writer = None
                if writer is not None:
                    writer.close()
                    with contextlib.suppress(Exception):
                        await writer.wait_closed()

            if self._stop_event.is_set():
                break
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)

    async def _read_loop(self) -> None:
        """Read newline-delimited JSON messages until disconnect."""
        assert self._reader is not None
        while not self._stop_event.is_set():
            raw_line = await self._reader.readline()
            if not raw_line:
                raise HvacTelnetConnectionError("Server closed the connection")

            line = raw_line.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                _LOGGER.debug("Ignoring non-JSON line from ESP32: %s", line)
                continue

            if isinstance(message, dict) and message.get("type") == "state" and "id" in message:
                await self._notify_state(message)

            pending = self._pending
            if pending is not None and pending.matcher(message):
                if not pending.future.done():
                    pending.future.set_result(message)
                self._pending = None
                continue

            if pending is not None and isinstance(message, dict) and message.get("ok") is False:
                if not pending.future.done():
                    pending.future.set_result(message)
                self._pending = None
                continue

            if isinstance(message, dict) and message.get("ok") is False:
                _LOGGER.debug("ESP32 returned error without pending request: %s", message)

    def _start_keepalive(self) -> None:
        """Ensure the background keepalive loop is running."""
        if self._keepalive_task is None or self._keepalive_task.done():
            self._keepalive_task = asyncio.create_task(self._keepalive_loop())

    async def _keepalive_loop(self) -> None:
        """Send periodic traffic so stale sockets are detected after ESP reboots."""
        while not self._stop_event.is_set():
            await asyncio.sleep(KEEPALIVE_INTERVAL)
            if not self._connected.is_set():
                return
            try:
                await self.async_ping()
            except asyncio.CancelledError:
                raise
            except Exception as err:
                _LOGGER.debug(
                    "Keepalive failed for %s:%s: %s",
                    self._host,
                    self._port,
                    err,
                )
                self._connected.clear()
                writer = self._writer
                if writer is not None:
                    writer.close()
                return

    async def _request(
        self,
        payload: dict[str, Any],
        matcher: Callable[[JsonValue], bool],
    ) -> JsonValue:
        """Send a command and wait for its matching response."""
        try:
            await asyncio.wait_for(self._connected.wait(), timeout=10)
        except TimeoutError as err:
            raise HvacTelnetConnectionError(
                f"Timed out waiting for connection to {self._host}:{self._port}"
            ) from err

        async with self._request_lock:
            writer = self._writer
            if writer is None:
                raise HvacTelnetConnectionError("Not connected")

            loop = asyncio.get_running_loop()
            future: asyncio.Future[JsonValue] = loop.create_future()
            self._pending = _PendingRequest(matcher=matcher, future=future)

            payload_line = json.dumps(payload, separators=(",", ":")) + "\r\n"
            try:
                writer.write(payload_line.encode("utf-8"))
                await writer.drain()
            except Exception as err:
                self._fail_pending(
                    HvacTelnetConnectionError(
                        f"Failed to send command to {self._host}:{self._port}"
                    )
                )
                self._connected.clear()
                writer.close()
                raise HvacTelnetConnectionError("Connection lost during send") from err

            try:
                response = await asyncio.wait_for(future, timeout=10)
            except TimeoutError as err:
                self._pending = None
                raise HvacTelnetCommandError(
                    f"Timed out waiting for response to {payload.get('cmd')}"
                ) from err

            if isinstance(response, dict) and response.get("ok") is False:
                raise HvacTelnetCommandError(
                    str(response.get("error", "Command failed"))
                )

            return response

    async def async_ping(self) -> dict[str, Any]:
        """Probe the ESP32 and force reconnect if the socket is stale."""
        response = await self._request(
            {"cmd": "list"},
            matcher=lambda msg: isinstance(msg, dict) and bool(msg.get("ok")),
        )
        if not isinstance(response, dict):
            raise HvacTelnetCommandError("Unexpected ping response")
        return response

    def _fail_pending(self, err: Exception) -> None:
        """Fail any in-flight request."""
        if self._pending is None:
            return
        if not self._pending.future.done():
            self._pending.future.set_exception(err)
        self._pending = None

    async def _notify_state(self, state: dict[str, Any]) -> None:
        """Dispatch a state update callback."""
        callback = self._state_callback
        if callback is None:
            return
        result = callback(state)
        if inspect.isawaitable(result):
            await result

    async def _notify_availability(self, available: bool) -> None:
        """Dispatch availability changes."""
        callback = self._availability_callback
        if callback is None:
            return
        result = callback(available)
        if inspect.isawaitable(result):
            await result

    async def _notify_connected(self) -> None:
        """Dispatch post-connect callback."""
        callback = self._connected_callback
        if callback is None:
            return
        result = callback()
        if inspect.isawaitable(result):
            await result
