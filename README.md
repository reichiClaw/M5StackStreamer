# M5Stack OE3 Chromecast Controller

Firmware for an **M5Stack Fire** that discovers Google Cast receivers and starts
the Hitradio OE3 live stream on the selected receiver.

The M5Stack is a remote control, not an audio relay. It sends this media URL to
the Cast receiver, and the receiver downloads the MP3 stream directly:

```text
http://orf-live.ors-shoutcast.at/oe3-q2a
```

## Features

- Stores Wi-Fi credentials in the ESP32's normal persistent Wi-Fi storage.
- Opens a password-protected captive portal for first-time Wi-Fi setup.
- Discovers Chromecast, Chromecast Audio, Google TV, speakers, groups, and
  other Cast V2 receivers through `_googlecast._tcp` mDNS.
- Lets you select a receiver on the M5Stack display, including names containing
  `Ä`, `Ö`, `Ü`, `ä`, `ö`, `ü`, and `ß`.
- Uses an Ö3-branded card interface with receiver and status icons, colored
  controls, Wi-Fi state, the M5Stack IP address, and an animated on-air
  equalizer.
- Keeps the Cast sender connected during playback, sends ten-second
  heartbeats, checks media health, and automatically reconnects or reloads the
  stream after an unexpected interruption.
- Detects half-open connections: when the receiver sends nothing for
  35 seconds, the session is torn down and recovered instead of idling
  silently forever.
- Reacts immediately to receiver-initiated media status broadcasts (stream
  finished, paused, or failed) instead of waiting for the next health poll.
- Disables ESP32 Wi-Fi modem power save while running, which removes the
  latency bursts that destabilize long-lived Cast TLS connections.
- Remembers active playback in flash and automatically resumes OE3 after a
  power loss or reboot.
- Monitors internet connectivity by probing the OE3 stream host (DNS plus a
  bounded TCP connect) and shows INTERNET / NO INTERNET / CHECKING on the
  display. Starting a stream is refused while the internet is unreachable,
  and the boot auto-resume waits until the uplink is confirmed. Stopping
  always works, since it only needs the LAN.
- Launches the Default Media Receiver and loads OE3 as a live `audio/mpeg`
  source.
- Uses the middle button to start or stop the maintained playback session.
- Displays discovery, connection, launch, and media-load errors.
- Automatically rescans while no receiver is available and recovers from a
  lost Wi-Fi connection.

## Hardware and network

- M5Stack Fire (original ESP32 model)
- USB cable for flashing
- A Google Cast receiver
- A 2.4 GHz Wi-Fi network for the M5Stack

The M5Stack and receiver must be on the same LAN. Router options named
**AP/client isolation**, **guest isolation**, or **block multicast/mDNS** must
be disabled. The receiver may use 5 GHz as long as the router bridges both
bands and forwards multicast between them.

## Build and flash

This is a PlatformIO project. From the repository root:

```bash
pio run -e m5stack-fire
pio run -e m5stack-fire --target upload
pio device monitor --baud 115200
```

The first build downloads the pinned ESP32 platform and libraries. To run the
host-side Cast protobuf tests:

```bash
pio test -e native
```

## First-time Wi-Fi setup

1. Power on the M5Stack.
2. If no saved network connects, the display shows a unique hotspot name such
   as `M5Cast-A1B2C3` and its password.
3. Connect a phone or laptop to that hotspot.
4. Open `http://192.168.4.1` if the captive portal does not open by itself.
5. Select the 2.4 GHz Wi-Fi network and enter its password.
6. The setup hotspot closes, and receiver discovery starts automatically.

Credentials are not compiled into the firmware.

To erase saved Wi-Fi credentials, hold the **left and right buttons (A + C)**
while powering on or resetting the M5Stack.

## Controls

| Button | Action |
| --- | --- |
| A / NEXT | Select the next discovered receiver |
| B / START-STOP | Stop maintained playback; otherwise query and start OE3 |
| C / SCAN | Rescan the LAN for Cast receivers |

When no maintained session exists, the middle button queries the receiver
before deciding whether to start or stop. Once OE3 is being maintained, the
button first disables automatic recovery and then stops that session. Query and
action share one TLS connection for compatibility with Chromecast-built-in
speakers.

Cast receiver names arrive as UTF-8. The firmware converts supported German
characters to the matching CP437 glyphs in the M5Stack's built-in display font,
so no external font file or SD card is required.

## Change the station

Edit the three stream constants in `include/AppConfig.h`:

```cpp
constexpr char kStreamUrl[] = "http://example.com/live.mp3";
constexpr char kStreamContentType[] = "audio/mpeg";
constexpr char kStreamTitle[] = "My station";
```

The URL must be reachable by the Cast receiver itself. A URL hosted only on a
phone, a VPN-only address, or `localhost` will not work.

## How it works

1. WiFiManager connects with saved credentials or exposes the setup portal.
2. ESPmDNS queries for `_googlecast._tcp` services.
3. The firmware opens TLS to the selected receiver's advertised Cast port.
4. It speaks the Cast V2 protobuf-framed protocol and queries app `CC1AD845`
   (Default Media Receiver) for its actual media state.
5. If media is active, it sends a receiver `STOP` request.
6. Otherwise it launches the receiver and sends a `LOAD` request with stream
   type `LIVE` and content type `audio/mpeg`.
7. Start success is shown only after `MEDIA_STATUS` reports `PLAYING`.
8. While playing, it retains the TLS and application channels, polls media
   status every 15 seconds, and uses bounded-backoff recovery when needed.

Heartbeats are transmitted every ten seconds, but a single missing `PONG`
does not tear down playback. Media-status timeouts retain the existing
session and retry later; socket write/framing failures and explicit channel
closure remain authoritative. This accommodates embedded Cast implementations
that use receiver-originated heartbeats. However, when *nothing* arrives from
the receiver for 35 seconds — roughly three missed heartbeat windows — the
connection is considered half-open and recovery starts. Recovery reconnects
with a single TLS attempt per cycle and exponential backoff from 2 seconds up
to 2 minutes, so the buttons, display, and diagnostics stay responsive while
a receiver is offline.

Cast receivers use device-generated certificates on the local Cast port, so
the firmware intentionally accepts the receiver's self-signed TLS certificate.
Traffic is encrypted, but the receiver is not authenticated by a public CA.
Only use the controller on a trusted LAN.

Google does not publish Cast V2 as a supported embedded-controller API.
Protocol changes in future receiver firmware could require a firmware update.

If a user-initiated secure Cast connection fails, the firmware retries twice
and displays the underlying TLS error. The complete numeric error and target
address are also available through `pio device monitor --baud 115200`.

Automatic recovery remains enabled until the middle **START-STOP** button is
used. Stopping playback from the receiver or another controller may therefore
cause OE3 to restart on the next health check. The playing/stopped decision
is also stored in flash: after a power loss the firmware reconnects and
resumes OE3 on the last receiver by itself. Press **START-STOP** to stop and
clear that stored state.

For Marshall Heddon, install current firmware using the Marshall app. If the
Cast session remains `PLAYING` but attached speakers become silent, remove
their standard Bluetooth pairing while retaining the `[LE]` pairing used by
the Marshall app; Marshall documents simultaneous Bluetooth and Auracast as a
possible source of dropouts. Ethernet can also isolate Heddon's Wi-Fi path.

## Diagnostics

The M5Stack IP appears in the status-card header. Open the following address
from another device on the same LAN:

```text
http://<M5-IP>/log
```

It returns the latest timestamped discovery, connection-stage, TLS, Cast
request/response, Wi-Fi signal, and memory diagnostics. Passwords are never
logged. The endpoint is intended only for a trusted LAN. The log stays
available during playback; a stalled HTTP client is dropped after two seconds
so it cannot delay Cast heartbeats. Up to 256 lines are retained when the
64 KiB PSRAM allocation succeeds; the internal-memory fallback retains
32 lines. The first line reports capacity, retained, total, and dropped line
counts.

For the complete live log, including ESP32 socket errors:

```bash
pio device monitor --baud 115200 | tee heddon.log
```

Wait until the terminal says it is connected, reset the M5Stack so boot events
are captured, then start casting. Quit with `Ctrl+C` and provide `heddon.log`
when reporting a connection problem.

## Logo artwork

The compact two-color logo in `include/Oe3Logo.h` is derived from
[Hitradio Ö3.svg on Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Hitradio_%C3%963.svg).
The source is a public-domain text logo; Hitradio Ö3 and its marks remain the
property of their respective owner.
