"""Coordinator for the HVAC Telnet integration."""

from __future__ import annotations

import logging
from copy import deepcopy

from homeassistant.core import HomeAssistant
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import HvacTelnetClient, HvacTelnetError
from .models import HvacSnapshot

_LOGGER = logging.getLogger(__name__)


class HvacTelnetCoordinator(DataUpdateCoordinator[HvacSnapshot]):
    """Track metadata, state, and connectivity for the ESP32 HVAC server."""

    def __init__(self, hass: HomeAssistant, client: HvacTelnetClient) -> None:
        super().__init__(
            hass,
            _LOGGER,
            name="hvactelnet",
        )
        self.client = client
        self.data = HvacSnapshot()

    async def _async_update_data(self) -> HvacSnapshot:
        """Perform the initial/full refresh."""
        try:
            hvacs = await self.client.async_get_hvacs()
            states = await self.client.async_get_all_states()
            status = await self.client.async_get_status()
        except HvacTelnetError as err:
            raise UpdateFailed(str(err)) from err

        return HvacSnapshot(
            hvacs=hvacs,
            states=states,
            status=status,
            available=True,
        )

    async def async_handle_state(self, state: dict[str, object]) -> None:
        """Merge a pushed state update into the current snapshot."""
        hvac_id = str(state["id"])
        next_data = HvacSnapshot(
            hvacs=self.data.hvacs,
            states=deepcopy(self.data.states),
            status=self.data.status,
            available=True,
        )
        next_data.states[hvac_id] = dict(state)
        self.async_set_updated_data(next_data)

    async def async_handle_availability(self, available: bool) -> None:
        """Update integration availability."""
        if self.data.available == available:
            return
        self.async_set_updated_data(
            HvacSnapshot(
                hvacs=self.data.hvacs,
                states=self.data.states,
                status=self.data.status,
                available=available,
            )
        )

    async def async_handle_connected(self) -> None:
        """Refresh full state after reconnect."""
        try:
            fresh = await self._async_update_data()
        except UpdateFailed as err:
            _LOGGER.debug("Reconnect refresh failed: %s", err)
            return
        self.async_set_updated_data(fresh)
