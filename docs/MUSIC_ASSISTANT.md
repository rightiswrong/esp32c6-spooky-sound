# Music Assistant, your Echo Show, and timing the lights to the music

## Is my Echo Show a Sendspin player?

Music Assistant combines every protocol a device speaks into one player, and shows those
protocols as **chips next to the player's name in its settings**. To check:

1. Open Music Assistant → **Settings → Players** and open your Echo Show.
2. Look at the chips beside its name:
   * **Home Assistant**: MA streams to it as a Home Assistant media player. This is how
     the **View Assist Companion app** shows up, since it exposes its built-in player to Home
     Assistant.
   * **Sendspin**: a Sendspin client is running on it, for example **SendspinDroid**.
   * Both: you can choose the default protocol in the same settings page.

At the time of writing, **View Assist doesn't speak Sendspin natively.** It's an open feature
request ([#119](https://github.com/msp1974/ViewAssist_Companion_App/issues/119),
[#309](https://github.com/msp1974/ViewAssist_Companion_App/issues/309)). People with
LineageOS Echo Shows run the separate **SendspinDroid** app (Android 8+, APK from
[its releases](https://github.com/chrisuthe/SendspinDroid)) alongside it.

## The two hooks

| | Metadata ("follow the soundtrack") | Sendspin visualizer ("sync to the music") |
|---|---|---|
| Works with | any media player with a track title, including View Assist | **Sendspin** players and groups only |
| What it knows | the title, artist and album of the track | peaks, beats, a 16-band spectrum and loudness, time-stamped to playback |
| What it does | switches the effect: storm → Lightning Storm, fireplace → Candle Flicker, heartbeat → Tell-Tale Heart, cauldron/witch → Witch's Cauldron, ghost/haunted/scream → Poltergeist; restores the old effect 15 s after playback stops | sends cues timed to the audio: thunder strikes, crackle bursts, fire level, rumble, heartbeats, bangs. It only sends what the current effect uses |
| Timing | — (the light's **microphone** reacts to each clap locally, about 21 ms in simulation) | cues sent early by *Light lead time*, so the strike can land exactly on the clap |
| Setup | pick the media player in the integration | below |

They combine well. With *Sound source = Mic + Home Assistant*, the microphone and Sendspin
both feed the same effects.

**No microphone?** Hook 1 still switches effects by track. Hook 2 then provides all of the
per-sound timing on its own, which is why it matters more without a mic: on a View-Assist-only
Echo Show (no Sendspin), the effects follow the track's mood but animate on their own, not on
each thunderclap.

## Hook 1 — follow the soundtrack

In **Spooky Sound → Configure**, pick the player, preferably the **Music Assistant** entity
for your speaker, since it carries the richest metadata. The keywords for each mood can be
edited there (comma separated, matched as whole words; storms win over fires when a track is
both). The *Soundtrack mood* sensor shows what was matched, and the *Follow the soundtrack*
switch turns the effect switching on or off.

Works on a NanoC6 Spooky Lights too: map just its Effect entity.

## Hook 2 — Sendspin

Music Assistant's Sendspin server analyses whatever a Sendspin group plays and streams
visualizer data to any visualizer client in that group. MA's own Hue Lights Sync uses the same
mechanism. Spooky Sound joins as one of those clients.

1. Make the Echo Show a Sendspin player (SendspinDroid), and make Sendspin its default
   protocol in MA.
2. In **Spooky Sound → Configure**, turn on **Sync to the music with Sendspin**. Leave the host
   empty to use the Music Assistant integration's server address, or enter it (`192.168.1.10`;
   port 8927 is added). The first time, Home Assistant installs the `aiosendspin` library.
3. A notification appears once the bridge is connected. In Music Assistant → Settings → Players,
   **show hidden players** (visualizers are hidden by default), open **Spooky Sound (…)**,
   **approve** it, and **join it to the Echo Show's group**.
4. Set the light to Lightning Storm (or let *Follow the soundtrack* do it) and play a
   thunderstorm. The *Sendspin* diagnostic sensor should read `streaming`, with frames and cues
   climbing.

### Tuning *Light lead time*

The visualizer frames say when their audio plays, and Spooky Sound sends each cue that many
milliseconds early. The default is 120 ms, a typical Home Assistant → ZHA → router → light delay.

* Flash **after** the clap → raise it.
* Flash **before** the clap → lower it.
* Cues that would arrive more than 250 ms late are dropped (a late flash is worse than none),
  and counted in the sensor's `cues_dropped`.

### Zigbee budget

Cues are rate-limited to about 6 per second (bursts of 4). Continuous levels (fire, rumble) are
refreshed every ~2 s or on a 10 % change and always keep a token spare for a thunderclap.
Crackles go out in bunches every 250 ms; the light spreads them over 0.4 s. Your porch sensors
won't notice.

## Without Sendspin, from your own automations

`spooky_sound.send_cue` sends any cue. For example, play a thunder sound effect on the Echo Show
from a script and send `cue: thunder` alongside it. Or trigger on the doorbell (see the README).
