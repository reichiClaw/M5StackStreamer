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
- Lets you select a receiver on the M5Stack display.
- Launches the Default Media Receiver and loads OE3 as a live `audio/mpeg`
  source.
- Uses the middle button as a remote-state start/stop toggle.
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
| B / START-STOP | Stop active media; otherwise start or restart OE3 |
| C / SCAN | Rescan the LAN for Cast receivers |

The middle button queries the receiver itself instead of relying on remembered
M5Stack state. The toggle therefore remains correct after either device
restarts or after playback is changed externally.

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

Cast receivers use device-generated certificates on the local Cast port, so
the firmware intentionally accepts the receiver's self-signed TLS certificate.
Traffic is encrypted, but the receiver is not authenticated by a public CA.
Only use the controller on a trusted LAN.

Google does not publish Cast V2 as a supported embedded-controller API.
Protocol changes in future receiver firmware could require a firmware update.
