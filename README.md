# HomeSrv Kobo client

Native e-ink dashboard client for HomeSrv, targeting Kobo e-readers
(developed against a Kobo Glo / Glo HD, firmware 4.x).

Launched from [NickelMenu], renders with [FBInk] (OpenType font rendering),
cross-compiled with the [NickelTC] toolchain container.

Features:
- Weather display (current temp, forecast, Unicode icons)
- Task list with a Done button (sorted by due date)
- Calendar events for today and tomorrow (with feed name)
- Touch input via libevdev
- Invisible corner targets: bottom left refetches, bottom right opens the menu
- Menu: switch between configured servers (prod/test), or quit
- Auto-refresh every 30 minutes, and immediately after a tap
- Full-screen takeover (stops/restarts Nickel, like KOReader)

## Layout

The UI is declarative, and **the server builds it**: `/api/ui.php` sends a tree
of nodes, the client parses it, the layout engine computes an absolute rect for
every node, and a painter draws it. Touch targets and partial refreshes are
queries over that tree, not separate bookkeeping.

So changing what the dashboard looks like means editing
`src/Ui/DashboardScreen.php` on the server — no rebuild, no reinstall, the next
refresh picks it up. The client is rebuilt only when the engine itself changes.

The clock is part of that server-sent layout too: the server sends
`{clock:%a, %b %e}` and the device substitutes its own clock every frame, so
the time keeps ticking while the server is unreachable. Only the status line
and the menu are still drawn by the client, because those have to work when
nothing else does. See [docs/ui-schema.md](docs/ui-schema.md).

- `src/main.c` — event loop, touch dispatch, network, refresh policy
- `src/screens.c` / `.h` — the chrome the client owns: status, menu, messages
- `src/config.c` / `.h` — the config file: the list of servers to choose from
- `src/ui_json.c` / `.h` — builds a tree from the server's JSON (cJSON), and
  substitutes the client-side values in it, such as the clock
- `src/ui.h` / `ui.c` — node schema, arena, builder macros, hit testing
- `src/ui_layout.c` — layout engine (portable, no FBInk)
- `src/ui_text.c` — text measurement, mirroring FBInk's OpenType metrics
- `src/ui_paint.c` / `ui_paint.h` — painter interface and the paint walk
- `src/paint_fbink.c` — device painter (the only file that knows FBInk's API)
- `src/paint_stb.c` — host painter, rasterises to a PNG for `make preview`
- `src/preview.c` — host preview binary
- `src/app.h` — app state
- `src/http.c` / `.h` — socket-based HTTP/1.0 client
- `fixtures/` — dashboard data to render, preview and test against
- `fixtures/servers.config` — server list for the menu preview and its goldens
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
second instead of a cross-compile and a reinstall. Needs `php` on the host: the
tree comes from the server's builder, fixture and all.

```bash
make -C kobo preview                      # renders fixtures/typical.json
make -C kobo preview FIXTURE=overflow     # 10 tasks, 12 events
make -C kobo preview FIXTURE=empty        # no tasks, events or weather
make -C kobo preview FIXTURE=finished     # several already-finished events
make -C kobo open-preview                 # render and open the PNG
make -C kobo preview FIXTURE=typical PREVIEW_ARGS=--toast   # tap feedback
make -C kobo preview SCREEN=menu          # the menu, from fixtures/servers.config

# Invisible targets cannot be checked by looking at a PNG, so ask the hit test:
build/homesrv-preview --ui build/ui-typical-glo.json --hit 700,1000 \
    --font fonts/DejaVuSans.ttf --out /dev/null
# -> 700,1000: menu action=4 target=menu rect=[663,968,70,44]
make -C kobo preview-live SERVER=http://192.168.1.50:8082 TOKEN=abc123
```

PNGs land in `kobo/build/preview-<fixture>-<device>.png`. Both a Glo (758x1024
@ 213 dpi) and a Glo HD (1072x1448 @ 300 dpi) are rendered by default, so a
layout is never tuned for one panel by accident; narrow it with
`PREVIEW_DEVICES=glo`.

Each fixture goes through `bin/render-ui.php` — the same builder `/api/ui.php`
uses — and then through the same parser, layout engine and text metrics as the
device. Only the painter differs, so glyph rasterisation can differ by a pixel;
layout is identical. `preview-live` skips the fixture and fetches a real tree
from a running server.

## Tests

```bash
make -C kobo test     # compare every node's rect against tests/golden/
make -C kobo golden   # accept the current layout as the new expectation
```

`make test` builds every fixture's UI on the server side, renders it at every
device size and diffs the computed rects. A regression in either half — what
the server describes or how the client lays it out — fails here, on your desk,
rather than on the Kobo.

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
SERVER_1_NAME=Home
SERVER_1_URL=http://192.168.x.x:8080
SERVER_1_TOKEN=abc123...
```

### More than one server

List up to six; the app starts on the first, and the menu switches between them
without a restart — which is how you point the device at a test server and back:

```
SERVER_1_NAME=Home
SERVER_1_URL=http://192.168.1.50:8082
SERVER_1_TOKEN=abc123

SERVER_2_NAME=Test
SERVER_2_URL=http://192.168.1.50:8083
SERVER_2_TOKEN=def456
```

`SERVER_n_NAME` is optional and defaults to the host:port. Gaps in the
numbering are fine. The choice is not remembered across restarts: the app
always comes back up on server 1.

## Usage

- **HomeSrv**: launches the dashboard (full-screen takeover, exits on SIGTERM)
- **HomeSrv Version**: message box showing version + FBInk version

On screen, two corners are tap targets and neither draws anything:

- **bottom left** — refetch the UI now, without waiting for the next refresh
- **bottom right** — open the menu: switch server, close, or quit

Both work on the connecting/loading screens too, so an unreachable server can
always be swapped for another one. The app stops Nickel on launch and restores
it on exit.

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
- More screens: `UI_ACT_OPEN` is in the schema but nothing serves a second one
- USB networking fallback
