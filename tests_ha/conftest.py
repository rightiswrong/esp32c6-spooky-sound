"""Home Assistant test fixtures (pytest-homeassistant-custom-component)."""

from __future__ import annotations

import pytest

pytest_plugins = "pytest_homeassistant_custom_component"


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    """Load custom_components/spooky_sound in every test."""
    yield
