"""The HVAC Telnet integration."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.core import HomeAssistant

from .api import HvacTelnetClient
from .const import DATA_CLIENT, DATA_COORDINATOR, DOMAIN, PLATFORMS
from .coordinator import HvacTelnetCoordinator


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up HVAC Telnet from a config entry."""
    hass.data.setdefault(DOMAIN, {})

    client = HvacTelnetClient(
        host=entry.data[CONF_HOST],
        port=entry.data[CONF_PORT],
    )
    coordinator = HvacTelnetCoordinator(hass, client)

    client.set_state_callback(coordinator.async_handle_state)
    client.set_availability_callback(coordinator.async_handle_availability)
    client.set_connected_callback(coordinator.async_handle_connected)

    try:
        await client.async_start(wait_for_connection=False)
        await coordinator.async_refresh()
    except Exception:
        await client.async_stop()
        raise

    hass.data[DOMAIN][entry.entry_id] = {
        DATA_CLIENT: client,
        DATA_COORDINATOR: coordinator,
    }

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if not unloaded:
        return False

    entry_data = hass.data[DOMAIN].pop(entry.entry_id)
    client: HvacTelnetClient = entry_data[DATA_CLIENT]
    await client.async_stop()

    if not hass.data[DOMAIN]:
        hass.data.pop(DOMAIN)

    return True
