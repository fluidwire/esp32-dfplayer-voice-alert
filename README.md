# esp32-dfplayer-voice-alert — play recorded voice alerts from a DFPlayer Mini when MQTT events arrive

IoT example by [Fluidwire](https://fluidwire.com/).

Sample sketch for an ESP32 dev board that turns MQTT events into spoken voice alerts through a **DFPlayer Mini** MP3 module — no buzzer beeps, an actual recorded voice saying "flood warning" or "gate open" in Tagalog, English, or whatever you record. This is the building block behind flood-warning posts, barangay gate/entry systems, and queue-number announcers that keep showing up in Philippine thesis and community projects.

## What it does

The sketch subscribes to a wildcard MQTT topic (`fluidwire/demo/dfplayer/alert/#`). Whatever comes after the last `/` in the incoming topic is looked up in a small local table that maps a name straight to a numbered clip on the SD card — publish to `.../alert/flood` and it plays `0001.mp3`. Before playing, it checks the DFPlayer's **BUSY** pin; if a clip is already playing, the new alert is dropped (not queued) and reported back over MQTT, so two alerts never talk over each other. Every action — played, skipped, or unknown — is published as a small JSON status message.

## Required hardware

- 1x ESP32 dev board
- 1x DFRobot DFPlayer Mini (or a common clone) + microSD card
- 1x small 3W, 4–8 Ω speaker
- A ~1kΩ resistor (recommended on the RX line, see wiring)
- An MQTT broker (local Mosquitto, or a public test broker like `broker.hivemq.com`)
- A 2.4 GHz WiFi network

## Required libraries

Install via Library Manager:

- **DFRobotDFPlayerMini** by DFRobot
- **PubSubClient** by Nick O'Leary

## Wiring

| DFPlayer pin | ESP32 pin |
|--------------|-----------|
| VCC          | 5V        |
| GND          | GND       |
| TX           | GPIO16 (RX2) |
| RX           | GPIO17 (TX2) — through a ~1kΩ series resistor |
| BUSY         | GPIO4     |
| SPK_1/SPK_2  | 3W 4–8 Ω speaker (raw amp output, not a line-level input) |

The DFPlayer's RX pin is a 5V logic input on most breakouts, and ESP32 GPIOs are 3.3V-only. Most clones tolerate 3.3V TX directly, but the series resistor is cheap insurance either way.

## SD card layout

Format FAT32, and drop MP3 files in the **root** of the card, named `0001.mp3`, `0002.mp3`, `0003.mp3`, `0004.mp3` (4-digit, zero-padded) matching the alert table in the sketch:

| Alert name (MQTT topic suffix) | Track |
|---------------------------------|-------|
| `flood`                         | 0001.mp3 |
| `gate`                          | 0002.mp3 |
| `queue`                         | 0003.mp3 |
| `test`                          | 0004.mp3 |

Add more rows to `ALERT_TABLE` in the `.ino` (and matching numbered files) to add clips.

## Setup

1. Copy `config.h.example` → `config.h`. Fill in WiFi, MQTT broker details, and volume.
2. Record or source short MP3 clips, name them per the table above, and copy them to the SD card root.
3. Wire everything per the table above.
4. Flash. Open the Serial Monitor at **115200 baud** and confirm `[dfplayer] ready`.
5. Publish an (even empty) message to `fluidwire/demo/dfplayer/alert/flood` and confirm `0001.mp3` plays.

## Expected serial output

```
[dfplayer] initializing...
[dfplayer] ready
[wifi] connecting to my-network
....
[wifi] IP: 192.168.1.44
[mqtt] connecting... ok
[mqtt] subscribed to fluidwire/demo/dfplayer/alert/#
[dfplayer] BUSY pin raw=0
[dfplayer] playing track 1 for alert flood
[mqtt] {"alert":"flood","track":1,"action":"playing"}
[dfplayer] BUSY pin raw=1
[dfplayer] heartbeat, busy=0
```

## Troubleshooting

- **Nothing plays, no error** — check the BUSY pin polarity first (see below); if `isBusyNow()` reads the wrong sense, every alert looks "busy" and gets silently dropped. Watch the `[dfplayer] BUSY pin raw=...` log line while a clip should be playing.
- **BUSY polarity looks backwards** — a few DFPlayer clones wire BUSY inverted from the datasheet. Flip `DFPLAYER_BUSY_ACTIVE_LOW` in `config.h` and reflash.
- **`[dfplayer] FAILED to init`** — check the SD card is FAT32 with files in the root (not a subfolder), and double-check TX/RX aren't swapped — DFPlayer TX goes to ESP32 RX2 and vice versa.
- **Audio is faint or distorted** — the SPK pins are a raw amplifier output; don't drive anything but a bare speaker cone directly from them, and make sure VCC is a solid 5V (a weak USB port can starve the amp under load).
- **`failed state=-2`** on MQTT — the broker host/port is wrong or unreachable. `-2` is a network/connection failure.

## Security note

This sketch keeps your WiFi and MQTT credentials in `config.h`, which is `.gitignore`'d — it never gets committed. Only `config.h.example` ships in the repo. Never hardcode real SSID/password into the `.ino`. Anyone who can publish to your broker can trigger alerts, so for a public-facing gate or flood-warning deployment, use a private broker with `MQTT_USER`/`MQTT_PASSWORD` rather than a shared public test broker.

## License

MIT — see [LICENSE](LICENSE).

## Got stuck?

Drop by [fluidwire.com](https://fluidwire.com/) — happy to help.
