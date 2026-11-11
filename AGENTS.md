# Koboard

Native e-ink dashboard client for Kobo e-readers: C + FBInk, cross-compiled via the NickelTC podman image, launched by NickelMenu on the device. A server sends a UI tree as JSON; Koboard lays it out, paints it, and turns taps into requests. See `README.md`; the wire format is `docs/ui-schema.md`.

## Quick reference

```bash
make                    # cross-compile build/koboard (podman)
make preview            # host PNGs in build/, ~1s — no device needed
make preview UI=f.json  # any UI tree file
make test               # unit tests, then golden layout rects
make golden             # accept a deliberate layout change
make install            # onto a USB-mounted Kobo
```

## Ground rules

**Do not iterate on layout by cross-compiling and reinstalling.** The layout engine, text measurement and parser build for the host, and `make preview` runs exactly that code; only the painter differs. After a deliberate engine change `make test` fails: check the diff and the PNGs, then `make golden`.

**What the screen shows is the server's business, not this repo's.** The client owns only what has to work when the server does not: the status line with its two invisible corner targets (refresh, menu), the message screen, and the menu that switches between the servers in the config file (`src/config.c`). Even the clock is server-sent, as a `{clock:...}` placeholder the device fills in each frame (`UiVars` in `src/ui_json.h`).

**Touch targets come from the painted tree.** `ui_hit_test()` maps a touch to the node that was drawn there; never hard-code a hit rectangle. Invisible targets can be checked with `build/koboard-preview --hit X,Y`.

**Protocol changes need unit tests.** URL handling, the envelope and the refresh policy (`src/http.c`, `src/ui_json.c`, `src/policy.c`) are where an odd value from someone else's server lands; `tests/unit.c` covers them and runs first in `make test`.

**Nickel is paused, not stopped, while Koboard runs**, and still holds the touch device open. `src/touch.c` grabs it exclusively and waits for the last lift on exit; anything that leaks through, Nickel replays when it resumes.

## Source layout

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
- `fonts/DejaVuSans*.ttf` — bundled fonts (DejaVu Sans, with weather symbols merged in from Noto Emoji — see `README.md`); their licences are alongside them
- `scripts/koboard.sh` — launcher: pauses Nickel, runs Koboard, resumes Nickel
- `nm/koboard` — NickelMenu config (main menu entries)
- `Makefile` — build orchestration via podman/NickelTC
- `vendor/FBInk/` — FBInk v1.25.0, cloned on demand
- `vendor/cJSON/` — cJSON v1.7.15, downloaded at build time
