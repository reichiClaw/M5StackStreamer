# AGENTS.md

## Cursor Cloud specific instructions

This repository is **embedded firmware** (not a client/server app): an M5Stack
Fire (ESP32) remote that discovers Google Cast receivers and starts a live radio
stream. It is a [PlatformIO](https://platformio.org/) project configured by
`platformio.ini`.

### Toolchain

- PlatformIO Core is installed in an isolated environment at
  `~/.platformio/penv` and symlinked onto `PATH` as `pio` / `platformio`
  (in `/usr/local/bin`). No virtualenv activation is needed.
- The pinned ESP32 platform (`espressif32@6.12.0`), the Xtensa toolchain, and
  the project libraries are cached under `~/.platformio/`. The **first** build
  downloads these (~1 GB, ~90s); later builds are fast.

### Build / test / run

- Build firmware: `pio run -e m5stack-fire` (compiles + links; verifies the full
  ESP32 build without hardware).
- Host unit tests: `pio test -e native` (Unity tests for the Cast protobuf codec;
  this is the only code that executes on the host).
- Flash / serial monitor (`pio run -e m5stack-fire --target upload`,
  `pio device monitor`) require a physically-connected M5Stack Fire and do **not**
  work in the cloud VM.
- There is no configured linter/formatter or CI in this repo.

### End-to-end testing caveat

Full E2E (Wi-Fi provisioning, `_googlecast._tcp` mDNS discovery, Cast V2 TLS,
media LOAD) requires **physical M5Stack Fire hardware, a Google Cast receiver,
and a multicast-friendly 2.4 GHz LAN**. This cannot be exercised inside the
cloud VM. Use `pio run -e m5stack-fire` (build) and `pio test -e native` (logic)
as the in-VM verification signals.
