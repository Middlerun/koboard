# Koboard

Turn a Kobo e-reader into a wall dashboard that shows whatever your server tells it to.

Koboard is a native e-ink client for Kobo e-readers. It doesn't know what a dashboard *is*: your server sends a tree of boxes and text over HTTP, and Koboard lays it out, paints it, and sends taps back as requests. Changing what the screen shows is a change on your server — no rebuild, no reinstall; the next refresh picks it up.

Launched from [NickelMenu], rendered with [FBInk] (OpenType text), cross-compiled with the [NickelTC] toolchain container. Developed against a Kobo Glo and Glo HD on firmware 4.x.

[NickelMenu]: https://pgaskin.net/NickelMenu/
[FBInk]: https://github.com/NiLuJe/FBInk
[NickelTC]: https://github.com/pgaskin/NickelTC

## Features

- **Server-driven UI**: a small flexbox-like schema of boxes and text, with borders, greys, padding, flex sizing, clipping and ellipsis. Layout happens on the device, against its real fonts and DPI.
- **Tap actions**: a node can POST to the server (with optimistic feedback in place), refetch, or open the menu.
- **A clock that keeps time between fetches**: the server sends `{clock:%a, %b %e}` and the device fills it in every frame.
- **Refresh policy from the server**: how often to refetch, what to ping to tell online from offline, and how often to flash the panel clear of ghosting — all with sane defaults and floors.
- **More than one server**: switch between, say, a production and a test server from an on-device menu.
- **Works when the server doesn't**: the status line, the menu and its two invisible corner targets are drawn by the client, so you can always switch server or quit.
- **Full-screen takeover**: pauses Nickel while running, like KOReader, and hands the screen and touch panel back cleanly on exit.

## Writing a server

Your server needs one URL that returns a UI tree as JSON:

```
GET http://your-server/dashboard?w=758&h=1024&dpi=213
Authorization: Bearer <token>
```

```json
{
  "schema": 1,
  "refresh": { "seconds": 300 },
  "root": {
    "kind": "box",
    "pad": 30,
    "gap": 10,
    "children": [
      {
        "kind": "text",
        "text": "{clock:%H:%M}",
        "size": 40,
        "key": "clock"
      },
      {
        "kind": "text",
        "text": "Hello from my server",
        "size": 14
      },
      {
        "kind": "box",
        "w": "40%",
        "border": 2,
        "pad": [8, 12, 8, 12],
        "action": {
          "kind": 1,
          "path": "/done",
          "body": "id=1",
          "toast": "OK!"
        },
        "children": [
          {
            "kind": "text",
            "text": "Done",
            "size": 11,
            "style": 1,
            "align": 1
          }
        ]
      }
    ]
  }
}
```

Any language will do — it's JSON over plain HTTP. The full reference, from node properties and layout rules to actions, the envelope and how URLs resolve, is [docs/ui-schema.md](docs/ui-schema.md).

To see a tree without a Kobo, save it to a file and run `make preview UI=tree.json`, or point the preview at your running server with `make preview-live URL=… TOKEN=…`. See [Preview](#preview-no-device-needed).

[HomeSrv](https://github.com/Middlerun/homesrv) — a household task tracker, and where Koboard started — is one such server: its dashboard is built in PHP, and the trees in `fixtures/` are recordings of it.

## How it works

The server sends a tree of nodes; the client parses it, the layout engine computes an absolute rect for every node, and a painter draws it. Touch targets and partial refreshes are queries over that tree, not separate bookkeeping, so a tap can never land somewhere other than what was drawn.

The client wraps the server's tree in the little it still owns: the status line along the bottom, and the menu. Those have to work when nothing else does.

- `src/main.c` — event loop, touch dispatch, network, refresh policy
- `src/screens.c` / `.h` — the chrome the client owns: status, menu, messages
- `src/config.c` / `.h` — the config file: the list of servers to choose from
- `src/policy.c` / `.h` — refresh/ping/flash intervals: a server's envelope, made safe
- `src/touch.c` / `.h` — keeps touches away from the paused Nickel (grab, and wait for lift on exit)
- `src/ui_json.c` / `.h` — builds a tree from the server's JSON (cJSON), and substitutes the client-side values in it, such as the clock
- `src/ui.h` / `ui.c` — node schema, arena, builder macros, hit testing
- `src/ui_layout.c` — layout engine (portable, no FBInk)
- `src/ui_text.c` — text measurement, mirroring FBInk's OpenType metrics
- `src/ui_paint.c` / `ui_paint.h` — painter interface and the paint walk
- `src/paint_fbink.c` — device painter (the only file that knows FBInk's API)
- `src/paint_stb.c` — host painter, rasterises to a PNG for `make preview`
- `src/preview.c` — host preview binary
- `src/app.h` — app state
- `src/http.c` / `.h` — socket-based HTTP/1.0 client
- `fixtures/*.json` — UI trees, exactly as a server sends them, to preview and test against
- `fixtures/servers.config` — server list for the menu preview and its goldens
- `tests/golden/` — expected layout rects, compared by `make test`
- `tests/unit.c` — unit tests for URLs, the envelope and the refresh policy
- `fonts/DejaVuSans*.ttf` — bundled fonts (DejaVu Sans, with weather symbols merged in from Noto Emoji — see [Fonts](#fonts)); their licences are alongside them
- `scripts/koboard.sh` — launcher: pauses Nickel, runs Koboard, resumes Nickel
- `nm/koboard` — NickelMenu config (main menu entries)
- `Makefile` — build orchestration via podman/NickelTC
- `vendor/FBInk/` — FBInk v1.25.0, cloned on demand
- `vendor/cJSON/` — cJSON v1.7.15, downloaded at build time

## Prerequisites

- podman (for the cross-compiler)
- git, curl
- a C compiler, for the host preview and tests
- python3 + fonttools — only to regenerate the bundled fonts; the fonts are checked in, so a normal build does not need it

## Fonts

Text is drawn in DejaVu Sans, regular and bold. On top of DejaVu's own coverage, the bundled fonts carry a set of weather symbols merged in from Noto Emoji, so a server can use them in any text node — a forecast is one of the most common things a dashboard shows:

☀ ☁ ☂ ☔ ⚡ ⛅ ⛈ ❄ 🌤 🌥 🌦 🌧 🌨 🌩 🌪 🌫 💧

DejaVu has no glyph at all for most of these, such as fog (🌫), drizzle (🌧) and the droplet (💧). For the few it does have, Noto's nicer versions replace its own.

They can't ship as a second font: FBInk's `fbink_add_ot_font()` *replaces* the font for a given style rather than appending a fallback, so loading an emoji font after DejaVu silently drops DejaVu and the whole screen goes blank. Instead the symbols are baked into the DejaVu files themselves. `scripts/make-font.sh` reproduces that: it downloads DejaVu Sans and Noto Emoji, subsets Noto Emoji down to the symbols wanted, and copies those glyphs over DejaVu's own for the same codepoints — into both the regular and bold faces. The result is checked in, so building and testing need no Python:

```bash
scripts/make-font.sh   # regenerates fonts/DejaVuSans.ttf and fonts/DejaVuSans-Bold.ttf
```

To offer servers a different set of symbols, change `EMOJI_CODEPOINTS` in `scripts/merge-emoji.py`, run it again, and update the list above.

## Build

```bash
make
file build/koboard   # expect: ELF 32-bit ARM EABI5
```

## Preview (no device needed)

The layout engine builds for the host too, so a UI change can be seen in a second instead of a cross-compile and a reinstall. It draws any UI tree — a fixture, a server's saved output, or a live server:

```bash
make preview                      # renders fixtures/typical.json
make preview FIXTURE=overflow     # 10 tasks, 12 events
make preview FIXTURE=empty        # nothing to show
make preview FIXTURE=finished     # several already-finished events
make preview UI=path/to/tree.json # any tree file
make open-preview                 # render and open the PNG
make preview FIXTURE=typical PREVIEW_ARGS=--toast   # tap feedback
make preview SCREEN=menu          # the menu, from fixtures/servers.config
make preview-live URL=http://192.168.1.10:8080/dashboard TOKEN=abc123

# Invisible targets cannot be checked by looking at a PNG, so ask the hit test:
build/koboard-preview --ui fixtures/typical.json --hit 700,1000 \
    --font fonts/DejaVuSans.ttf --out /dev/null
# -> 700,1000: menu action=4 target=menu rect=[663,968,70,44]
```

PNGs land in `build/preview-<fixture>-<device>.png`. Both a Glo (758x1024 @ 213 dpi) and a Glo HD (1072x1448 @ 300 dpi) are rendered by default, so a layout is never tuned for one panel by accident; narrow it with `PREVIEW_DEVICES=glo`.

A tree goes through the same parser, layout engine and text metrics as on the device. Only the painter differs, so glyph rasterisation can differ by a pixel; layout is identical. `preview-live` fetches the tree the way the device does: the URL with the panel's geometry appended, and the token as a bearer header.

## Tests

```bash
make test     # unit tests, then every node's rect against tests/golden/
make golden   # accept the current layout as the new expectation
```

The unit tests cover the protocol side — URL parsing and resolution, the envelope, and the floors and defaults of the refresh policy — which is where an unusual value from someone else's server is most likely to land.

`make test` renders every fixture tree at every device size and diffs the computed rects, so a layout regression fails here, on your desk, rather than on the Kobo. It needs no server: the fixtures are trees, and what a server *sends* is that server's to test.

## Install

1. Make sure your server is running and reachable from the Kobo's network.
2. Connect the Kobo over USB, confirm the connection on the device, then:

```bash
make install
# asks for the screen URL and the token
```

Or copy `dist/.adds/` onto the Kobo manually and create the config file:

```
# /mnt/onboard/.adds/koboard/config
SERVER_1_NAME=Home
SERVER_1_URL=http://192.168.1.10:8080/dashboard
SERVER_1_TOKEN=abc123...
```

`SERVER_n_URL` is the URL of the screen itself, not just the host: the client fetches exactly that, with `w`, `h` and `dpi` appended, and resolves everything the server sends back (action paths, the ping URL) against it. The token is sent as `Authorization: Bearer <token>`, and can be left out for a server that does not need one. Only `http://` is supported — there is no TLS, so the token crosses your network in the clear.

### More than one server

List up to six; the app starts on the first, and the menu switches between them without a restart — which is how you point the device at a test server and back:

```
SERVER_1_NAME=Home
SERVER_1_URL=http://192.168.1.10:8080/dashboard
SERVER_1_TOKEN=abc123

SERVER_2_NAME=Test
SERVER_2_URL=http://192.168.1.10:8081/dashboard
SERVER_2_TOKEN=def456
```

`SERVER_n_NAME` is optional and defaults to the host:port. Gaps in the numbering are fine. The choice is not remembered across restarts: the app always comes back up on server 1.

## Usage

NickelMenu gets two entries on the main menu:

- **Koboard** — launches the dashboard (full-screen takeover, exits on SIGTERM)
- **Koboard Version** — message box showing the version and FBInk's

On screen, two corners are tap targets and neither draws anything:

- **bottom left** — refetch the UI now, without waiting for the next refresh
- **bottom right** — open the menu: switch server, close, or quit

Both work on the connecting/loading screens too, so an unreachable server can always be swapped for another one. The app pauses Nickel on launch and resumes it on exit.

## Troubleshooting

- `.adds/koboard/koboard.log` on the Kobo — most recent run output
- NickelMenu reports config syntax errors as main-menu items
- If the screen stays white/frozen: telnet/SSH in and run `killall koboard`
- Touch orientation has only been checked on the Glo and Glo HD; other models may need an entry in the device-ID table in `src/main.c`

## Uninstall

Delete `.adds/koboard/` and `.adds/nm/koboard` from the Kobo.

## Next steps

- Wi-Fi auto-connect chain in NM config
- Diff consecutive UI trees to repaint only what changed (the clock and status line already repaint on their own, via `ui_find` on a node key)
- More screens: `UI_ACT_OPEN` only knows the client's own two
- TLS
- USB networking fallback

## Licence

Copyright (C) 2026 Eddie McLean.

Koboard is licensed under the GNU General Public License, version 3 — see [LICENSE](LICENSE). It statically links [FBInk](https://github.com/NiLuJe/FBInk), which is GPLv3, so the binary has to be too.

Bundled and fetched components keep their own licences: cJSON (MIT), stb (MIT or public domain), libunibreak (zlib), libevdev (MIT), the DejaVu fonts (Bitstream Vera / Arev licence, [fonts/LICENSE-DejaVu.txt](fonts/LICENSE-DejaVu.txt)), and Noto Emoji (SIL Open Font License 1.1, [fonts/OFL-NotoEmoji.txt](fonts/OFL-NotoEmoji.txt)) — the latter contributes the weather symbols merged into the bundled DejaVu files. Both font licences ship with the fonts in the device package too.
