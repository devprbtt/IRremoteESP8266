"""Config flow for the HVAC Telnet integration."""

from __future__ import annotations

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow
from homeassistant.const import CONF_HOST, CONF_PORT
from homeassistant.data_entry_flow import FlowResult

from .api import HvacTelnetClient, HvacTelnetError
from .const import DEFAULT_PORT, DOMAIN


async def _validate_input(host: str, port: int) -> None:
    """Validate that the ESP32 endpoint is reachable and speaks the expected API."""
    client = HvacTelnetClient(host, port)
    try:
        await client.async_start()
        await client.async_get_hvacs()
    finally:
        await client.async_stop()


class HvacTelnetConfigFlow(ConfigFlow, domain=DOMAIN):
    """Handle a config flow for HVAC Telnet."""

    VERSION = 1

    async def async_step_user(
        self,
        user_input: dict[str, object] | None = None,
    ) -> FlowResult:
        """Handle the initial step."""
        errors: dict[str, str] = {}

        if user_input is not None:
            host = str(user_input[CONF_HOST])
            port = int(user_input[CONF_PORT])
            unique_id = f"{host}:{port}"
            await self.async_set_unique_id(unique_id)
            self._abort_if_unique_id_configured()

            try:
                await _validate_input(host, port)
            except HvacTelnetError:
                errors["base"] = "cannot_connect"
            except Exception:
                errors["base"] = "unknown"
            else:
                return self.async_create_entry(
                    title=host,
                    data={
                        CONF_HOST: host,
                        CONF_PORT: port,
                    },
                )

        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required(CONF_HOST): str,
                    vol.Optional(CONF_PORT, default=DEFAULT_PORT): int,
                }
            ),
            errors=errors,
        )
