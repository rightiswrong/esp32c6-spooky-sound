"""Follow a media player: pick the Halloween effect that fits what's playing.

Works with any media_player that exposes the track title — Music Assistant's
players do, whichever protocol they use (Sendspin, AirPlay, Cast, or a Home
Assistant media player such as a View Assist tablet). No audio is analysed
here: this is the "metadata" hook. A thunderstorm track switches the light to
Lightning Storm; when playback stops, the light goes back to what it was.
"""

from __future__ import annotations

import logging

from homeassistant.core import CALLBACK_TYPE, Event, EventStateChangedData, HomeAssistant, callback
from homeassistant.helpers.event import async_call_later, async_track_state_change_event

from . import SoundRuntime
from .const import EFFECTS, RESTORE_GRACE_S, ROLE_EFFECT
from .logic import MOODS_BY_KEY, match_mood

_LOGGER = logging.getLogger(__name__)

PLAYING = "playing"


class MusicFollower:
    """Watches one media_player and keeps runtime.mood (and the effect) in step."""

    def __init__(self, hass: HomeAssistant, runtime: SoundRuntime) -> None:
        self.hass = hass
        self.runtime = runtime
        self._saved_effect: int | None = None   # what to restore when the soundtrack ends
        self._applied_mood: str | None = None
        self._cancel_restore: CALLBACK_TYPE | None = None

    @callback
    def async_start(self) -> CALLBACK_TYPE:
        unsub = async_track_state_change_event(self.hass, [self.runtime.media_player], self._changed)
        self.hass.async_create_task(self._evaluate(), eager_start=True)

        @callback
        def _stop() -> None:
            unsub()
            if self._cancel_restore:
                self._cancel_restore()
                self._cancel_restore = None

        return _stop

    @callback
    def _changed(self, event: Event[EventStateChangedData]) -> None:
        self.hass.async_create_task(self._evaluate(), eager_start=True)

    def current_mood(self) -> str | None:
        state = self.hass.states.get(self.runtime.media_player) if self.runtime.media_player else None
        if state is None or state.state != PLAYING:
            return None
        attrs = state.attributes
        return match_mood(
            (
                attrs.get("media_title"),
                attrs.get("media_artist"),
                attrs.get("media_album_name"),
                attrs.get("media_playlist"),
                attrs.get("media_series_title"),
            ),
            self.runtime.keywords,
        )

    async def _evaluate(self) -> None:
        mood = self.current_mood()
        if mood != self.runtime.mood:
            _LOGGER.debug("Soundtrack mood %s -> %s", self.runtime.mood, mood)
            self.runtime.mood = mood
            self.runtime.notify()
        await self.async_apply()

    async def async_apply(self) -> None:
        """Bring the light's effect in line with the mood (if following is on)."""
        mood = self.runtime.mood if self.runtime.follow_music else None
        if not self.runtime.entity(ROLE_EFFECT):
            return
        if mood is not None:
            if self._cancel_restore:
                self._cancel_restore()
                self._cancel_restore = None
            if mood == self._applied_mood:
                return
            if self._saved_effect is None:
                self._saved_effect = self.runtime.effect_index()
            self._applied_mood = mood
            await self.runtime.async_set_effect(EFFECTS.index(MOODS_BY_KEY[mood].effect))
            return
        # No (followed) mood: put the old effect back after a short grace
        # period, so a pause or a track change doesn't flicker the show.
        if self._applied_mood is not None and self._cancel_restore is None:
            self._cancel_restore = async_call_later(self.hass, RESTORE_GRACE_S, self._restore_later)

    async def _restore_later(self, _now) -> None:
        self._cancel_restore = None
        await self.async_restore_now()

    async def async_restore_now(self) -> None:
        if self._cancel_restore:
            self._cancel_restore()
            self._cancel_restore = None
        saved, self._saved_effect = self._saved_effect, None
        self._applied_mood = None
        if saved is not None and self.runtime.entity(ROLE_EFFECT):
            await self.runtime.async_set_effect(saved)
