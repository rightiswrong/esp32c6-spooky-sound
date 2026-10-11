"""Sendspin bridge: join Music Assistant as a visualizer and cue the light in time.

Music Assistant's Sendspin server analyses the audio of a Sendspin group and
streams "visualizer" frames — loudness, a 16-bin spectrum, energy peaks and
beats — each stamped with the moment its audio plays. They arrive ahead of
the audio (players buffer), so a thunder cue can be written to Zigbee early by
the light's measured delay ("lead time") and the strike lands on the clap.

Requirements (see docs/MUSIC_ASSISTANT.md):
  * the speaker must be a *Sendspin* player in Music Assistant (e.g. the
    SendspinDroid app on an Echo Show running LineageOS);
  * this bridge appears in MA as a hidden "Spooky Sound" visualizer player:
    approve it once, then join it to the speaker's group.

aiosendspin is installed on demand when the bridge is switched on, so the
rest of the integration never depends on it. Its API moves quickly; the
bridge pins the 10.x line and fails soft (status "error") if that changes.
"""

from __future__ import annotations

import asyncio
import base64
import importlib
import logging
from typing import Any

from homeassistant.components import persistent_notification
from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.storage import Store
from homeassistant.requirements import async_process_requirements

from . import SoundRuntime
from .const import DOMAIN, SENDSPIN_CLIENT_NAME, SENDSPIN_REQUIREMENT
from .logic import (
    SPECTRUM_BINS,
    SPECTRUM_F_MAX,
    SPECTRUM_F_MIN,
    Cue,
    VisualizerCueEngine,
    cue_delay_s,
    sendspin_url,
)

_LOGGER = logging.getLogger(__name__)

STATUS_OFF = "off"
STATUS_INSTALLING = "installing"
STATUS_CONNECTING = "connecting"
STATUS_WAITING = "waiting"          # connected, no music streaming to our group
STATUS_STREAMING = "streaming"
STATUS_NO_SERVER = "no_server"
STATUS_ERROR = "error"

STALE_S = 0.25            # a cue this late is dropped: a late flash is worse than none
RETRY_S = (5, 10, 30, 60)


def _music_assistant_host(hass: HomeAssistant) -> str | None:
    """Borrow the server address from the Music Assistant integration, if installed."""
    for entry in hass.config_entries.async_entries("music_assistant"):
        if url := entry.data.get("url"):
            return url
    return None


class SendspinBridge:
    def __init__(self, hass: HomeAssistant, entry: ConfigEntry, runtime: SoundRuntime) -> None:
        self.hass = hass
        self.entry = entry
        self.runtime = runtime
        self.engine = VisualizerCueEngine()
        self._task: asyncio.Task | None = None
        self._client: Any = None
        self._disconnected = asyncio.Event()
        self._pending: set[asyncio.TimerHandle] = set()
        self._store = Store[dict[str, Any]](hass, 1, f"{DOMAIN}.sendspin.{entry.entry_id}")
        self.frames = 0
        self.url: str | None = None
        self.error: str | None = None

    # ------------------------------------------------------------ lifecycle
    @callback
    def async_start(self) -> None:
        self._task = self.entry.async_create_background_task(self.hass, self._run(), f"{DOMAIN} sendspin bridge")

    async def async_stop(self) -> None:
        self._cancel_pending()
        if self._task:
            self._task.cancel()
            self._task = None
        if self._client is not None:
            try:
                await self._client.disconnect()
            except Exception:  # noqa: BLE001 — shutting down anyway
                pass
            self._client = None
        self._set_status(STATUS_OFF)

    def _set_status(self, status: str, error: str | None = None) -> None:
        self.error = error
        if self.runtime.bridge_status != status:
            self.runtime.bridge_status = status
            self.runtime.notify()

    # ----------------------------------------------------------------- run
    async def _run(self) -> None:
        self.url = sendspin_url(self.runtime.sendspin_host or _music_assistant_host(self.hass))
        if not self.url:
            self._set_status(STATUS_NO_SERVER)
            return
        self._set_status(STATUS_INSTALLING)
        try:
            await async_process_requirements(self.hass, DOMAIN, [SENDSPIN_REQUIREMENT])
            api = await self.hass.async_add_import_executor_job(_import_api)
            client = await self._make_client(api)
        except Exception as err:  # noqa: BLE001 — surface any failure as a status
            _LOGGER.warning("Sendspin bridge unavailable: %s", err)
            self._set_status(STATUS_ERROR, str(err))
            return
        self._client = client
        attempt = 0
        while True:
            self._set_status(STATUS_CONNECTING)
            self._disconnected.clear()
            try:
                await client.connect(self.url)
            except asyncio.CancelledError:
                raise
            except Exception as err:  # noqa: BLE001
                wait = RETRY_S[min(attempt, len(RETRY_S) - 1)]
                attempt += 1
                _LOGGER.debug("Sendspin connect to %s failed (%s); retry in %ss", self.url, err, wait)
                self._set_status(STATUS_ERROR, str(err))
                await asyncio.sleep(wait)
                continue
            attempt = 0
            self._set_status(STATUS_WAITING)
            await self._notify_once()
            await self._disconnected.wait()
            self._cancel_pending()
            await asyncio.sleep(RETRY_S[0])

    async def _make_client(self, api: dict[str, Any]) -> Any:
        data = await self._store.async_load() or {}
        if key := data.get("identity"):
            identity = api["Identity"].from_private_bytes(base64.urlsafe_b64decode(key + "=" * (-len(key) % 4)))
        else:
            identity = api["Identity"].generate()
            await self._store.async_save({**data, "identity": identity.private_b64u})
        pairing_store = await api["FileClientPairingStore"].open(
            self.hass.config.path(".storage", f"{DOMAIN}.sendspin_pairing.{self.entry.entry_id}.json")
        )
        spectrum = api["ClientHelloVisualizerSpectrum"](
            n_disp_bins=SPECTRUM_BINS, scale="log", f_min=SPECTRUM_F_MIN, f_max=SPECTRUM_F_MAX
        )
        client = api["SendspinClient"](
            identity,
            f"{SENDSPIN_CLIENT_NAME} ({self.entry.title})",
            [api["Roles"].VISUALIZER],
            pairing_store=pairing_store,
            device_info=api["DeviceInfo"](product_name="Spooky Sound", manufacturer="SpookyLab"),
            visualizer_support=api["ClientHelloVisualizerSupport"](buffer_capacity=262144),
            visualizer_state=api["VisualizerStatePayload"](
                types=["peak", "loudness", "spectrum", "beat"], rate_max=30, spectrum=spectrum
            ),
            session=async_get_clientsession(self.hass),
        )
        # A light reveals nothing private: let MA use it without a pairing PIN
        # (MA still asks once to approve the device).
        await client.set_unpaired_access(enabled=True)
        client.add_visualizer_listener(self._on_frames)
        client.add_stream_start_listener(lambda _msg: self._set_status(STATUS_STREAMING))
        client.add_stream_clear_listener(lambda _roles: self._cancel_pending())
        client.add_stream_end_listener(self._on_stream_end)
        client.add_disconnect_listener(self._on_disconnect)
        return client

    async def _notify_once(self) -> None:
        data = await self._store.async_load() or {}
        if data.get("told_user"):
            return
        await self._store.async_save({**data, "told_user": True})
        persistent_notification.async_create(
            self.hass,
            (
                f"**{SENDSPIN_CLIENT_NAME} ({self.entry.title})** is now connected to Music Assistant.\n\n"
                "1. In Music Assistant open **Settings → Players**, show hidden players, and approve it.\n"
                "2. Join it to the group of the Sendspin speaker that plays your Halloween soundtrack.\n\n"
                "Then play a thunderstorm track and watch the lightning land on the thunder."
            ),
            title="Spooky Sound: one step in Music Assistant",
            notification_id=f"{DOMAIN}_sendspin_{self.entry.entry_id}",
        )

    # ------------------------------------------------------------ streaming
    @callback
    def _on_disconnect(self) -> None:
        self._set_status(STATUS_CONNECTING)
        self._disconnected.set()

    @callback
    def _on_stream_end(self, roles: list[str] | None) -> None:
        if roles is None or "visualizer" in roles:
            self._cancel_pending()
            self._set_status(STATUS_WAITING)

    @callback
    def _on_frames(self, frames: list[Any]) -> None:
        if not self.runtime.music_sync or self._client is None:
            return
        self.frames += len(frames)
        if self.runtime.bridge_status != STATUS_STREAMING:
            self._set_status(STATUS_STREAMING)
        self.engine.set_focus(self.runtime.focus())
        for frame in frames:
            for cue in self.engine.feed(frame):
                self._schedule(cue)

    def _schedule(self, cue: Cue) -> None:
        client = self._client
        try:
            play_us = client.compute_play_time(cue.timestamp_us)
            now_us = client.now_us()
        except Exception:  # noqa: BLE001 — not time-synced yet
            return
        delay = cue_delay_s(play_us, now_us, self.runtime.lead_ms)
        if delay < -STALE_S:
            self.runtime.cues_dropped += 1
            return
        priority = cue.kind in ("thunder", "beat", "peak", "crackle")
        handle: asyncio.TimerHandle | None = None

        def _fire() -> None:
            self._pending.discard(handle)
            self.hass.async_create_task(self.runtime.async_send_cue(cue.kind, cue.param, priority=priority))

        handle = self.hass.loop.call_later(max(0.0, delay), _fire)
        self._pending.add(handle)

    def _cancel_pending(self) -> None:
        for handle in self._pending:
            handle.cancel()
        self._pending.clear()


def _import_api() -> dict[str, Any]:
    """Import the pieces of aiosendspin 10.x we use (runs in the executor)."""
    client = importlib.import_module("aiosendspin.client")
    core = importlib.import_module("aiosendspin.models.core")
    types = importlib.import_module("aiosendspin.models.types")
    viz = importlib.import_module("aiosendspin.models.visualizer")
    keys = importlib.import_module("aiosendspin.noise.keys")
    trust = importlib.import_module("aiosendspin.noise.trust_store")
    return {
        "SendspinClient": client.SendspinClient,
        "DeviceInfo": core.DeviceInfo,
        "Roles": types.Roles,
        "ClientHelloVisualizerSupport": viz.ClientHelloVisualizerSupport,
        "ClientHelloVisualizerSpectrum": viz.ClientHelloVisualizerSpectrum,
        "VisualizerStatePayload": viz.VisualizerStatePayload,
        "Identity": keys.Identity,
        "FileClientPairingStore": trust.FileClientPairingStore,
    }
