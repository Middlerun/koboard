# HomeSrv Kobo client

Native e-ink client skeleton for HomeSrv, targeting Kobo e-readers (developed
against a Kobo Glo / Glo HD, firmware 4.x). Launched from [NickelMenu], renders
with [FBInk], cross-compiled with the [NickelTC] toolchain container.

Current state: **skeleton only**. It draws a hello screen over Nickel and
exits — the point is to validate the build → install → launch pipeline.

[NickelMenu]: https://pgaskin.net/NickelMenu
[FBInk]: https://github.com/NiLuJe/FBInk
[NickelTC]: https://github.com/pgaskin/NickelTC

## Layout

- `src/main.c` — the app. `--version` prints to stdout; otherwise draws the
  hello screen via FBInk (linked statically, `MINIMAL=1 BITMAP=1` build).
- `scripts/homesrv.sh` — launcher run by NickelMenu; logs the most recent run
  to `.adds/homesrv/homesrv.log` on the device.
- `nm/homesrv` — NickelMenu config adding two main-menu items:
  `HomeSrv` (run the app) and `HomeSrv Version` (show `--version` output in a
  message box — proves the binary executes even if drawing fails).
- `Makefile` — host targets orchestrate podman; `inner-*` targets run inside
  the NickelTC container.
- `vendor/FBInk/` — FBInk v1.25.0 source, cloned on demand (not committed).

## Prerequisites

- podman (the NickelTC image `ghcr.io/pgaskin/nickeltc:1.0` is ~1 GB, pulled
  once via `make toolchain` or on first build)
- git (to fetch FBInk with submodules)

## Build

```bash
make            # clone FBInk if needed, build libfbink.a + app, stage dist/
file build/homesrv   # expect: ELF 32-bit LSB executable, ARM, EABI5
```

Artifacts land in `dist/`:

```
dist/.adds/homesrv/bin/homesrv   ARM binary
dist/.adds/homesrv/homesrv.sh    launcher
dist/.adds/nm/homesrv            NickelMenu config
dist/homesrv-kobo-<ver>.tgz      the same tree, tarred
```

## Install

Connect the Kobo over USB and confirm the connection on the device, then:

```bash
make install                       # assumes /run/media/$USER/KOBOeReader
make install KOBO_MOUNT=/path/to/KOBOeReader
```

(Or manually copy `dist/.adds/*` into the `.adds/` folder on the Kobo.)
Eject the Kobo. NickelMenu picks up config changes automatically; if the new
entries don't appear in the main menu, reboot the device.

## Validate on the device

1. Open the NickelMenu main menu → tap **HomeSrv Version**: a message box
   should show `homesrv-kobo 0.1.0 (FBInk v1.25.0 ...)`.
2. Tap **HomeSrv**: the screen flashes and shows the HomeSrv banner plus the
   detected device/resolution. Nickel keeps running underneath; open any menu
   to make it redraw.

## Troubleshooting

- Check `.adds/homesrv/homesrv.log` on the Kobo (log of the most recent run).
- NickelMenu reports config syntax errors as an item in the main menu.
- Deeper digging: telnet/SSH in and run `logread`.

## Uninstall

Delete `.adds/homesrv/` and `.adds/nm/homesrv` from the Kobo.

## Next steps (not implemented)

- Wi-Fi: chain `nickel_wifi :autoconnect` in the NM config before launching.
- Talk to HomeSrv over HTTP (JSON endpoints on the server side).
- Touch input (FBInk ships libevdev; input via `/dev/input/event*`).
- Full-screen takeover (stop Nickel while the app runs, restart on exit) once
  the app becomes interactive.
