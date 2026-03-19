"""Diagnostic sensors for the HVAC Telnet integration."""

from __future__ import annotations

from dataclasses import dataclass
from collections.abc import Callable
from typing import Any

from homeassistant.components.sensor import SensorEntity, SensorEntityDescription
from homeassistant.config_entries import ConfigEntry
from homeassistant.const import EntityCategory, UnitOfInformation, SIGNAL_STRENGTH_DECIBELS_MILLIWATT
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DATA_COORDINATOR, DOMAIN
from .coordinator import HvacTelnetCoordinator


@dataclass(frozen=True, kw_only=True)
class HvacTelnetSensorDescription(SensorEntityDescription):
    """Description for a diagnostic HVAC Telnet sensor."""

    value_fn: Callable[[dict[str, Any]], Any]


SENSORS: tuple[HvacTelnetSensorDescription, ...] = (
    HvacTelnetSensorDescription(
        key="firmware_version",
        name="Firmware Version",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("firmware_version"),
    ),
    HvacTelnetSensorDescription(
        key="filesystem_version",
        name="Filesystem Version",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("filesystem_version"),
    ),
    HvacTelnetSensorDescription(
        key="version_status",
        name="Version Status",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: "matched" if data.get("version_match") else "mismatch",
    ),
    HvacTelnetSensorDescription(
        key="wifi_rssi",
        name="WiFi RSSI",
        native_unit_of_measurement=SIGNAL_STRENGTH_DECIBELS_MILLIWATT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("wifi_rssi"),
    ),
    HvacTelnetSensorDescription(
        key="free_heap",
        name="Free Heap",
        native_unit_of_measurement=UnitOfInformation.BYTES,
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("heap_free"),
    ),
    HvacTelnetSensorDescription(
        key="telnet_clients",
        name="Telnet Clients",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("telnet_clients_active"),
    ),
    HvacTelnetSensorDescription(
        key="network_mode",
        name="Network Mode",
        entity_category=EntityCategory.DIAGNOSTIC,
        value_fn=lambda data: data.get("network_mode"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up diagnostic sensors."""
    coordinator: HvacTelnetCoordinator = hass.data[DOMAIN][entry.entry_id][DATA_COORDINATOR]
    async_add_entities(
        HvacTelnetDiagnosticSensor(coordinator, entry.entry_id, description)
        for description in SENSORS
    )


class HvacTelnetDiagnosticSensor(
    CoordinatorEntity[HvacTelnetCoordinator], SensorEntity
):
    """Diagnostic sensor backed by the telnet coordinator."""

    _attr_has_entity_name = True

    def __init__(
        self,
        coordinator: HvacTelnetCoordinator,
        entry_id: str,
        description: HvacTelnetSensorDescription,
    ) -> None:
        super().__init__(coordinator)
        self.entity_description = description
        self._entry_id = entry_id
        self._attr_unique_id = f"{entry_id}_{description.key}"

    @property
    def native_value(self) -> Any:
        """Return the current sensor value."""
        return self.entity_description.value_fn(self.coordinator.data.status)

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
