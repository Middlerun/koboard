# HomeSrv Kobo client

Native e-ink dashboard client for HomeSrv, targeting Kobo e-readers
(developed against a Kobo Glo / Glo HD, firmware 4.x).

Launched from [NickelMenu], renders with [FBInk] (OpenType font rendering),
cross-compiled with the [NickelTC] toolchain container.

Features:
- Weather display (current temp, forecast, Unicode icons)
- Task list with Done / Defer buttons (sorted by due date, status-colored)
- Calendar events for today and tomorrow (with feed name)
- Defer modal (1 day, 2 days, 4 days, 1 week)
- Touch input via libevdev
- Auto-refresh every 15 minutes
- Full-screen takeover (stops/restarts Nickel, like KOReader)

## Layout

- `src/main.c` — main app: event loop, touch input, rendering (~530 lines)
- `src/http.c` / `.h` — socket-based HTTP/1.0 client
- `src/json.c` / `.h` — JSON response parsing via cJSON
- `src/layout.h` — layout constants, data structures, function declarations
- `fonts/DejaVuSans.ttf` — bundled proportional font, downloaded at build time
- `scripts/homesrv.sh` — launcher: stops Nickel, runs homesrv, restores Nickel
- `nm/homesrv` — NickelMenu config (main menu entries)
- `Makefile` — build orchestration via podman/NickelTC
- `vendor/FBInk/` — FBInk v1.25.0, cloned on demand
- `vendor/cJSON/` — cJSON v1.7.15, downloaded at build time

## Prerequisites

- podman
- git, curl

## Build

```bash
make -C kobo
file kobo/build/homesrv   # expect: ELF 32-bit ARM EABI5
```

## Install

1. Ensure the HomeSrv server is running and reachable on the LAN.
2. Go to Settings → API Token in the web UI; copy the token.
3. Connect the Kobo over USB, confirm the connection on the device, then:

```bash
make -C kobo install
# answers: http://192.168.x.x:8080  and  the token
```

Or copy `kobo/dist/.adds/` onto the Kobo manually and create the config file:

```
# /mnt/onboard/.adds/homesrv/config
SERVER_URL=http://192.168.x.x:8080
API_TOKEN=abc123...
```

## Usage

- **HomeSrv**: launches the dashboard (full-screen takeover, exits on SIGTERM)
- **HomeSrv Version**: message box showing version + FBInk version

The app stops Nickel on launch and restores it on exit. To exit the app and
return to Nickel, you can kill the process (e.g., via telnet/SSH).

## Troubleshooting

- `.adds/homesrv/homesrv.log` on the Kobo — most recent run output
- NickelMenu reports config syntax errors as main-menu items
- If the screen stays white/frozen: telnet/SSH in and run `killall homesrv`

## Uninstall

Delete `.adds/homesrv/` and `.adds/nm/homesrv` from the Kobo.

## Next steps (post-validation)

- Wi-Fi auto-connect chain in NM config
- Better button labels with visible borders
- Calendar tap-to-scroll for more events
- Partial updates for done/defer instead of full refresh
- USB networking fallback
