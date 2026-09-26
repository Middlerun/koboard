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

The UI is declarative: `screens.c` builds a tree of nodes, the layout engine
computes an absolute rect for every node, and a painter draws it. Touch targets
and partial refreshes are queries over that tree, not separate bookkeeping. See
[docs/ui-schema.md](docs/ui-schema.md).

- `src/main.c` — event loop, touch dispatch, network, refresh policy
- `src/screens.c` / `.h` — what the dashboard looks like; builds the UI tree
- `src/ui.h` / `ui.c` — node schema, arena, builder macros, hit testing
- `src/ui_layout.c` — layout engine (portable, no FBInk)
- `src/ui_text.c` — text measurement, mirroring FBInk's OpenType metrics
- `src/ui_paint.c` / `ui_paint.h` — painter interface and the paint walk
- `src/paint_fbink.c` — device painter (the only file that knows FBInk's API)
- `src/paint_stb.c` — host painter, rasterises to a PNG for `make preview`
- `src/preview.c` — host preview binary
- `src/app.h` — app state
- `src/http.c` / `.h` — socket-based HTTP/1.0 client
- `src/json.c` / `.h` — dashboard JSON parsing via cJSON
- `fixtures/` — dashboard payloads to preview and test against
- `tests/golden/` — expected layout rects, compared by `make test`
- `fonts/DejaVuSans*.ttf` — bundled fonts, downloaded at build time
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

## Preview (no device needed)

The layout engine builds for the host too, so a UI change can be seen in a
second instead of a cross-compile and a reinstall:

```bash
make -C kobo preview                      # renders fixtures/typical.json
make -C kobo preview FIXTURE=overflow     # 10 tasks, 12 events
make -C kobo preview FIXTURE=empty        # no tasks, events or weather
make -C kobo preview FIXTURE=finished     # several already-finished events
make -C kobo open-preview                 # render and open the PNG
make -C kobo preview FIXTURE=typical PREVIEW_ARGS=--toast   # tap feedback
make -C kobo preview-live SERVER=http://192.168.1.50:8082 TOKEN=abc123
```

PNGs land in `kobo/build/preview-<fixture>-<device>.png`. Both a Glo (758x1024
@ 213 dpi) and a Glo HD (1072x1448 @ 300 dpi) are rendered by default, so a
layout is never tuned for one panel by accident; narrow it with
`PREVIEW_DEVICES=glo`.

This uses the same `screens.c`, layout engine and text metrics as the device.
Only the painter differs, so glyph rasterisation can differ by a pixel; layout
is identical.

## Tests

```bash
make -C kobo test     # compare every node's rect against tests/golden/
make -C kobo golden   # accept the current layout as the new expectation
```

`make test` renders every fixture at every device size and diffs the computed
rects. A layout regression fails here, on your desk, rather than on the Kobo.

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
- Calendar tap-to-scroll for more events
- Diff consecutive UI trees to repaint only what changed (the clock and status
  line already repaint on their own, via `ui_find` on a node key)
- Move tree building to the server (`/api/ui.php` + `ui_from_json`), so UI
  changes need no rebuild at all — see the end of docs/ui-schema.md
- USB networking fallback
