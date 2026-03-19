"""Climate platform for the HVAC Telnet integration."""

from __future__ import annotations

from typing import Any

from homeassistant.components.climate import ClimateEntity
from homeassistant.components.climate.const import (
    ClimateEntityFeature,
    HVACAction,
    HVACMode,
)
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import ATTR_TEMPERATURE, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DATA_COORDINATOR, DOMAIN, MAX_TEMP_C, MIN_TEMP_C
from .coordinator import HvacTelnetCoordinator
from .models import HvacDescription

HVAC_MODE_MAP = {
    "off": HVACMode.OFF,
    "auto": HVACMode.AUTO,
    "cool": HVACMode.COOL,
    "heat": HVACMode.HEAT,
    "dry": HVACMode.DRY,
    "fan": HVACMode.FAN_ONLY,
}

HVAC_MODE_TO_API = {value: key for key, value in HVAC_MODE_MAP.items()}

FAN_MODES = ["auto", "min", "low", "medium", "high", "max"]


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up HVAC Telnet climate entities."""
    coordinator: HvacTelnetCoordinator = hass.data[DOMAIN][entry.entry_id][DATA_COORDINATOR]
    known_ids: set[str] = set()

    def async_add_missing_entities() -> None:
        new_entities = [
            HvacTelnetClimateEntity(coordinator, entry.entry_id, description)
            for description in coordinator.data.hvacs.values()
            if not description.is_custom and description.hvac_id not in known_ids
        ]
        if not new_entities:
            return
        known_ids.update(entity.hvac_id for entity in new_entities)
        async_add_entities(new_entities)

    async_add_missing_entities()

    entry.async_on_unload(coordinator.async_add_listener(async_add_missing_entities))


class HvacTelnetClimateEntity(CoordinatorEntity[HvacTelnetCoordinator], ClimateEntity):
    """A climate entity backed by an ESP32 HvacTelnetServer entry."""

    _attr_temperature_unit = UnitOfTemperature.CELSIUS
    _attr_target_temperature_step = 1
    _attr_min_temp = MIN_TEMP_C
    _attr_max_temp = MAX_TEMP_C
    _attr_supported_features = (
        ClimateEntityFeature.TARGET_TEMPERATURE
        | ClimateEntityFeature.FAN_MODE
        | ClimateEntityFeature.TURN_ON
        | ClimateEntityFeature.TURN_OFF
    )
    _attr_hvac_modes = [
        HVACMode.OFF,
        HVACMode.AUTO,
        HVACMode.COOL,
        HVACMode.HEAT,
        HVACMode.DRY,
        HVACMode.FAN_ONLY,
    ]
    _attr_fan_modes = FAN_MODES

    def __init__(
        self,
        coordinator: HvacTelnetCoordinator,
        entry_id: str,
        description: HvacDescription,
    ) -> None:
        super().__init__(coordinator)
        self._entry_id = entry_id
        self._description = description
        self._attr_unique_id = f"{entry_id}_{description.hvac_id}"
        self._attr_name = _display_name(description)
        self._attr_has_entity_name = True

    @property
    def hvac_id(self) -> str:
        """Return the underlying HVAC id."""
        return self._description.hvac_id

    @property
    def available(self) -> bool:
        """Return whether the entity is available."""
        return (
            self.coordinator.data.available
            and self._description.hvac_id in self.coordinator.data.states
        )

    @property
    def current_temperature(self) -> float | None:
        """Return the current temperature."""
        state = self._state
        if state is None:
            return None
        return _as_float(state.get("current_temp"))

    @property
    def target_temperature(self) -> float | None:
        """Return the target temperature."""
        state = self._state
        if state is None:
            return None
        return _as_float(state.get("setpoint"))

    @property
    def fan_mode(self) -> str | None:
        """Return the fan mode."""
        state = self._state
        if state is None:
            return None
        fan = str(state.get("fan", "auto"))
        return fan if fan in FAN_MODES else "auto"

    @property
    def hvac_mode(self) -> HVACMode | None:
        """Return the current HVAC mode."""
        state = self._state
        if state is None:
            return None
        if str(state.get("power", "off")) != "on":
            return HVACMode.OFF
        return HVAC_MODE_MAP.get(str(state.get("mode", "auto")), HVACMode.AUTO)

    @property
    def hvac_action(self) -> HVACAction | None:
        """Return the running HVAC action."""
        mode = self.hvac_mode
        if mode is None:
            return None
        if mode == HVACMode.OFF:
            return HVACAction.OFF
        if mode == HVACMode.COOL:
            return HVACAction.COOLING
        if mode == HVACMode.HEAT:
            return HVACAction.HEATING
        if mode == HVACMode.DRY:
            return HVACAction.DRYING
        if mode == HVACMode.FAN_ONLY:
            return HVACAction.FAN
        return HVACAction.IDLE

    async def async_set_hvac_mode(self, hvac_mode: HVACMode) -> None:
        """Set the HVAC mode."""
        if hvac_mode == HVACMode.OFF:
            await self.coordinator.client.async_send(
                self._description.hvac_id,
                {"power": "off"},
            )
            return

        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {
                "power": "on",
                "mode": HVAC_MODE_TO_API[hvac_mode],
                "temp": self.target_temperature or 24,
                "fan": self.fan_mode or "auto",
                "light": "true",
            },
        )

    async def async_set_temperature(self, **kwargs: Any) -> None:
        """Set the target temperature."""
        temperature = kwargs.get(ATTR_TEMPERATURE)
        if temperature is None:
            return
        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {
                "power": "on",
                "mode": HVAC_MODE_TO_API.get(self.hvac_mode or HVACMode.AUTO, "auto"),
                "temp": round(float(temperature)),
                "fan": self.fan_mode or "auto",
                "light": "true",
            },
        )

    async def async_set_fan_mode(self, fan_mode: str) -> None:
        """Set the fan mode."""
        if fan_mode not in FAN_MODES:
            fan_mode = "auto"
        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {
                "power": "on",
                "mode": HVAC_MODE_TO_API.get(self.hvac_mode or HVACMode.AUTO, "auto"),
                "temp": self.target_temperature or 24,
                "fan": fan_mode,
                "light": "true",
            },
        )

    async def async_turn_on(self) -> None:
        """Turn the HVAC on."""
        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {
                "power": "on",
                "mode": HVAC_MODE_TO_API.get(
                    self.hvac_mode if self.hvac_mode != HVACMode.OFF else HVACMode.AUTO,
                    "auto",
                ),
                "temp": self.target_temperature or 24,
                "fan": self.fan_mode or "auto",
                "light": "true",
            },
        )

    async def async_turn_off(self) -> None:
        """Turn the HVAC off."""
        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {"power": "off"},
        )

    @property
    def _state(self) -> dict[str, Any] | None:
        """Return the current raw state."""
        return self.coordinator.data.states.get(self._description.hvac_id)

    @property
    def device_info(self) -> DeviceInfo:
        """Return the parent device info."""
        status = self.coordinator.data.status
        host = str(status.get("hostname") or getattr(self.coordinator.client, "_host", "ir-server"))
        return DeviceInfo(
            identifiers={(DOMAIN, self._entry_id)},
            name=f"IR Server Telnet ({host})",
            manufacturer="Zafiro",
            model="IR Server Telnet",
            sw_version=str(status.get("firmware_version") or "unknown"),
            configuration_url=f"http://{host}.local/",
        )


def _as_float(value: Any) -> float | None:
    """Convert a state value to float when possible."""
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _display_name(description: HvacDescription) -> str:
    """Return the entity name from the configured profile when available."""
    if description.profile_name:
        return description.profile_name
    return f"HVAC {description.hvac_id}"
