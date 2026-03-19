"""Config flow for the HVAC Telnet integration."""

from __future__ import annotations

from typing import Any

import voluptuous as vol

from homeassistant.const import CONF_HOST, CONF_NAME, CONF_PORT
from homeassistant.config_entries import ConfigFlow
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
    
    def __init__(self) -> None:
        """Initialize the config flow."""
        self._discovered_host: str | None = None
        self._discovered_port: int = DEFAULT_PORT
        self._discovered_name: str | None = None

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

    async def async_step_zeroconf(
        self,
        discovery_info: Any,
    ) -> FlowResult:
        """Handle Zeroconf discovery."""
        host = str(getattr(discovery_info, "host", "") or "").strip()
        hostname = str(getattr(discovery_info, "hostname", "") or "").rstrip(".")
        name = str(getattr(discovery_info, "name", "") or "").rstrip(".")
        port = int(getattr(discovery_info, "port", DEFAULT_PORT) or DEFAULT_PORT)

        if not host:
            return self.async_abort(reason="cannot_connect")

        unique_id = f"{host}:{port}"
        await self.async_set_unique_id(unique_id)
        self._abort_if_unique_id_configured(
            updates={
                CONF_HOST: host,
                CONF_PORT: port,
            }
        )

        try:
            await _validate_input(host, port)
        except HvacTelnetError:
            return self.async_abort(reason="cannot_connect")
        except Exception:
            return self.async_abort(reason="unknown")

        self._discovered_host = host
        self._discovered_port = port
        self._discovered_name = hostname or name or host
        return await self.async_step_zeroconf_confirm()

    async def async_step_zeroconf_confirm(
        self,
        user_input: dict[str, object] | None = None,
    ) -> FlowResult:
        """Confirm discovery from Zeroconf."""
        if self._discovered_host is None:
            return self.async_abort(reason="unknown")

        if user_input is not None:
            return self.async_create_entry(
                title=self._discovered_name or self._discovered_host,
                data={
                    CONF_HOST: self._discovered_host,
                    CONF_PORT: self._discovered_port,
                },
            )

        return self.async_show_form(
            step_id="zeroconf_confirm",
            description_placeholders={
                CONF_NAME: self._discovered_name or self._discovered_host,
                CONF_HOST: self._discovered_host,
                CONF_PORT: str(self._discovered_port),
            },
        )
