"""Constants for the HVAC Telnet integration."""

from __future__ import annotations

from homeassistant.const import Platform

DOMAIN = "hvactelnet"
DEFAULT_PORT = 4998
PLATFORMS = (Platform.CLIMATE, Platform.BUTTON, Platform.SENSOR)

DATA_CLIENT = "client"
DATA_COORDINATOR = "coordinator"

MIN_TEMP_C = 16
MAX_TEMP_C = 32
