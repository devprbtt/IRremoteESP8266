"""Button platform for custom IR profiles."""

from __future__ import annotations

from homeassistant.components.button import ButtonEntity
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .const import DATA_COORDINATOR, DOMAIN
from .coordinator import HvacTelnetCoordinator
from .models import HvacDescription


async def async_setup_entry(
    hass: HomeAssistant,
    entry: ConfigEntry,
    async_add_entities: AddEntitiesCallback,
) -> None:
    """Set up custom profile command buttons."""
    coordinator: HvacTelnetCoordinator = hass.data[DOMAIN][entry.entry_id][DATA_COORDINATOR]
    known_ids: set[str] = set()

    def async_add_missing_entities() -> None:
        new_entities: list[HvacTelnetCommandButton] = []
        for description in coordinator.data.hvacs.values():
            if not description.is_custom or not description.custom_commands:
                continue
            for command_name in description.custom_commands:
                unique_id = f"{description.hvac_id}_{command_name}"
                if unique_id in known_ids:
                    continue
                new_entities.append(
                    HvacTelnetCommandButton(
                        coordinator,
                        entry.entry_id,
                        description,
                        command_name,
                    )
                )
                known_ids.add(unique_id)
        if new_entities:
            async_add_entities(new_entities)

    async_add_missing_entities()
    entry.async_on_unload(coordinator.async_add_listener(async_add_missing_entities))


class HvacTelnetCommandButton(CoordinatorEntity[HvacTelnetCoordinator], ButtonEntity):
    """Expose one custom command as a Home Assistant button."""

    def __init__(
        self,
        coordinator: HvacTelnetCoordinator,
        entry_id: str,
        description: HvacDescription,
        command_name: str,
    ) -> None:
        super().__init__(coordinator)
        self._description = description
        self._command_name = command_name
        base_name = description.profile_name or f"Device {description.hvac_id}"
        self._attr_unique_id = f"{entry_id}_{description.hvac_id}_{command_name}_button"
        self._attr_name = f"{base_name} {command_name}"

    @property
    def available(self) -> bool:
        """Return whether the command button is available."""
        return self.coordinator.data.available and (
            self._description.hvac_id in self.coordinator.data.states
        )

    async def async_press(self) -> None:
        """Send the custom command to the device profile."""
        await self.coordinator.client.async_send(
            self._description.hvac_id,
            {"command_name": self._command_name},
        )
