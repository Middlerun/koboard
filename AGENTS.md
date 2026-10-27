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
