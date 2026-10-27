# Koboard UI schema

Koboard describes a screen as a tree of nodes, lays it out, and paints it. Nothing positions itself; nothing tracks a `y` cursor; nothing hard-codes a touch rectangle. This document is the reference for that tree.

The schema is a deliberate subset of CSS flexbox — enough for the dashboard, small enough to hold in your head. It maps 1:1 onto JSON, and **the server is what produces it**: what the dashboard looks like is decided on the server, not on the device. Changing the UI is a server edit; the client only has to be rebuilt when the *engine* changes.

## Pipeline

```
your server                  builds a tree, serves it as JSON ──┐
                                                                │  HTTP
src/ui_json.c                ui_from_json()  ←──────────────────┘
  └─ screens.c               wraps it in the client-owned chrome
       └─ ui_layout.c        one pass down (constraints), one pass back
                             (sizes), one pass assigning absolute rects
            └─ painter       paint_fbink.c on the device,
                             paint_stb.c for the preview
                 └─ ui_hit_test()  maps a touch to the node that was drawn
```

`ui_text.c` measures text for the layout. It re-implements FBInk's OpenType line-breaking arithmetic exactly (see the comment at the top of that file), so what the layout measures is what FBInk draws — and so the host preview can measure without a framebuffer.

## The wire

A server is known to the client by one URL: the URL of its screen, as written in the config file (see README.md). Everything else is relative to that.

```
GET <screen URL>?w=758&h=1024&dpi=213
Authorization: Bearer <token>

{ "schema": 1,
  "screen": "dashboard",
  "refresh": { "seconds": 1800, "ping": "ping",
               "pingSeconds": 10, "flashSeconds": 7200 },
  "root":    { ...node... } }
```

The client appends its panel geometry to the configured URL (after `?` or `&`, as the URL needs), because spacing scales with the screen width; point sizes are resolved against the device's own DPI, so the server never deals in pixels for text. The token goes in a standard bearer header, and is left out altogether if the config has none. Only `http://` is supported — there is no TLS, so the token crosses the network in the clear.

A `schema` newer than the client's `UI_SCHEMA_VERSION` is refused rather than half-parsed, and any property the client does not recognise is ignored — that is what lets the server describe something a slightly older client has never heard of.

A response is parsed once, on arrival, before it is allowed to replace the screen that is currently working. After that it is re-parsed into the frame arena on every frame, so there is one code path and nothing to keep in sync.

### Envelope

Only `root` is required. Everything else has a client default, and the client puts a floor under each interval so a typo cannot make it hammer a server or the panel:

| field | default | floor | meaning |
|---|---|---|---|
| `refresh.seconds` | 1800 | 15 | refetch the screen this often |
| `refresh.ping` | none | — | a cheap URL that answers 200 while the server is up |
| `refresh.pingSeconds` | 10 | 5 | check `ping` this often; `0` = never |
| `refresh.flashSeconds` | 7200 | 300 | flash the whole panel to clear ghosting; `0` = never |

Without a ping URL, the status line's online/offline simply follows how the last fetch went. The policy belongs to the server that sent it: switching servers drops back to the defaults until the new one has said otherwise.

### URLs in a tree

An action's `path` and the envelope's `ping` are resolved against the screen URL the way a browser would: an absolute `http://…` URL is used as is, `/x` is relative to the screen URL's origin, and anything else is relative to its directory — so a screen at `http://host/kobo/dashboard` can say `ping` and mean `http://host/kobo/ping`, wherever it happens to be hosted.

## What the client still owns

The server sends the *body* of the screen. The client wraps it in the chrome that has to keep working when the server does not, and gives the body all the leftover height (`screen_frame()` in `screens.c`):

```
<server tree>            — h: flex:1, whatever the screen URL sent
status row    KEY_STATUS — last refresh, online/offline, and the two corners
```

The status row carries the screen's only two fixed tap targets, both invisible: `KEY_REFRESH` at bottom left refetches immediately, `KEY_MENU` at bottom right opens the menu. They draw nothing and they are always there, including on a message screen — which is the point, because "this server is not answering" is exactly when you need to reach the menu and pick another one.

The menu itself (`screen_menu()`) is client-drawn for the same reason: it lists the servers from the config file, and switching to one drops the old server's tree and refetches. While it is up it replaces the body, and nothing in the background repaints over it.

Also client-side: the centred message used while connecting or when a response is refused (`screen_message()`), the defaults and limits of the refresh policy, and the tap toast — whose text comes from the server, in the action.

None of the chrome's styling is the server's to set. The status row is inset by width / 30 on each side, so a server that wants its body to line up with it uses the same margin.

## Client-side values

The one thing the server cannot send is the time: it is already stale when the tree arrives, and it has to keep ticking between fetches. So the server sends a placeholder and the device substitutes its own clock, every frame, as the tree is parsed:

| placeholder | becomes |
|---|---|
| `{clock:%a, %b %e}` | `Fri, Sep 26` — any strftime(3) format |
| `{clock}` | `08:01` — `%H:%M` by default |

That is what lets the clock live in the server's layout instead of the chrome: its wording, size and position are a server edit, like the rest of the tree. The device keeps the formatting honest in the one way it must — the value is the device's own clock, in the device's own timezone, so it stays right while the server is unreachable.

A placeholder naming anything else is passed through untouched, which is why `overflowText`'s `{n}` (expanded later, by the layout engine) is unaffected.

By convention the node showing the time carries `"key": "clock"`: that is what the client repaints on its own, once a minute, instead of redrawing the screen. Leave the key off and the time still updates — just only when something else redraws.

## Node kinds

| kind   | purpose                                         |
|--------|-------------------------------------------------|
| `box`  | container, background, border, padding, gaps    |
| `text` | a string, wrapped and optionally ellipsized     |

There is no separate rule/divider/spacer kind: a `box` with a fixed size and a background is a divider, and one without a background is a spacer.

## Properties

### Both kinds

| property | C | JSON | meaning |
|---|---|---|---|
| key | `.key = "task-12"` | `"key"` | stable identity; `ui_find()` locates it for a partial repaint |
| w, h | `.w = FLEX(1)` | `"w": "flex:1"` | see **Lengths** |
| pad | `.pad = {t,r,b,l}` | `"pad": [t,r,b,l]` | inside the border |
| border | `.border = 2` | `"border": 2` | uniform, drawn inside the rect |
| borderColor | `.border_color = UI_DARK` | `"borderColor": 64` | defaults to the foreground |
| bg | `.bg = UI_WHITE` | `"bg": 255` | 0–255 gray; unset means transparent |
| fg | `.fg = UI_MED` | `"fg": 128` | 0–255 gray; **inherited** by descendants |
| action | `.action = ACTION(...)` | `"action": {...}` | makes the node a tap target |

### box only

| property | C | JSON | meaning |
|---|---|---|---|
| dir | `.dir = UI_ROW` | `"dir": "row"` | main axis; default is `col` |
| gap | `.gap = 8` | `"gap": 8` | between children |
| main | `.main = UI_MAIN_CENTER` | `"main": 1` | `start`(0) `center`(1) `end`(2) `between`(3) |
| cross | `.cross = UI_CROSS_CENTER` | `"cross": 2` | `stretch`(0) `start`(1) `center`(2) `end`(3) |
| clip | `.clip = true` | `"clip": true` | drop whole children that do not fit |
| children | `CHILDREN(box, ...)` | `"children": [...]` | the nested nodes, in paint order |
| overflowText | `.overflow_text = "… +{n} more"` | `"overflowText"` | shown when `clip` dropped children; `{n}` is the count |

### text only

| property | C | JSON | meaning |
|---|---|---|---|
| text | `TEXT(a, "Done", ...)` | `"text"` | the string (copied into the arena) |
| size | `.size_pt = 13` | `"size": 13` | points; pixels are derived from the screen's DPI |
| style | `.style = UI_FNT_BOLD` | `"style": 1` | regular(0) bold(1) italic(2) bold-italic(3); missing faces fall back to regular |
| align | `.align = UI_ALIGN_CENTER` | `"align": 1` | start(0) center(1) end(2), within the node's own rect |
| maxLines | `.max_lines = 2` | `"maxLines": 2` | 0 = unlimited |
| noEllipsis | `.no_ellipsis = true` | `"noEllipsis": true` | truncate without appending `…` |

## Lengths

| form | C | JSON | resolves to |
|---|---|---|---|
| auto | `AUTO` (the default) | `"auto"` | text: its measured size. box: its children's extent. |
| pixels | `PX(200)` | `"200"` | exactly that many pixels |
| percent | `PCT(37)` | `"37%"` | that share of the **parent's content box** on that axis |
| flex | `FLEX(2)` | `"flex:2"` | that share of the parent's *leftover* main-axis space |

A definite length is a border-box size: padding and border come out of it, not on top of it.

## Layout rules

- **Main axis** is horizontal for `dir: row`, vertical for `dir: col`. Children are laid end to end with `gap` between them.
- **Cross axis** children default to `stretch`, which is why text wraps at the full available width without being told to. Stretch only applies to a child whose cross-axis length is `auto`; an explicit `w`/`h` always wins.
- **Flex** needs a definite main-axis size on the parent. Inside an `auto`-sized parent, a flex child behaves as `auto`.
- **A definite height caps a text node's line count.** This is what keeps a long title inside its card without any manual bottom checks.
- **Clipping** drops whole children, never half of one, so a task card is either fully drawn or not drawn at all.
- Nodes are painted in tree order, so a later sibling draws over an earlier one.

## Actions

```json
"action": { "kind": 1, "path": "/tasks/complete", "body": "task_id=12",
            "toast": "COMPLETED!" }
```

| kind | JSON | effect |
|---|---|---|
| `UI_ACT_POST` (1) | `{"kind":1,"path":...,"body":...}` | form-encoded POST to `path` (see **URLs in a tree**), then refresh |
| `UI_ACT_REFRESH` (2) | `{"kind":2}` | re-fetch and redraw |
| `UI_ACT_OPEN` (4) | `{"kind":4,"target":"menu"}` | switch to a client-drawn screen: `menu` or `dashboard` |
| `UI_ACT_QUIT` (3) | — | exit the app |
| `UI_ACT_SERVER` (5) | — | switch to the server at that index in the config, and refetch |

The last two are **not accepted from the wire** — `ui_from_json()` parses an allow-list of the first three and drops the rest. Quitting belongs to the menu, and which servers exist is not something a server should get a say in. Neither costs the server anything: it can send the user to the menu with `UI_ACT_OPEN`, and the menu offers both.

`toast` is optional optimistic feedback: the node's rect is replaced with that text while the request is in flight.

`ui_hit_test()` returns the deepest node with an action whose **painted rect** contains the touch, so a touch target can never drift out of sync with what is on screen.

## Authoring a screen

On the server, in any language: the wire format above is all there is. A small builder that mirrors the node kinds pays for itself, though — HomeSrv, the first Koboard server, has one in PHP (`HomeSrv\Ui\Node`) that validates every property name against this schema, so a typo throws on the server instead of being silently ignored on the device.

Client-side, for the chrome only, with the C macros:

```c
UiNode *row = CHILDREN(BOX(a, .dir = UI_ROW, .gap = 8),
        TEXT(a, "Mon", .size_pt = 10, .w = FLEX(1)),
        TEXTF(a, (.size_pt = 10, .align = UI_ALIGN_END), "%d°", temp));
```

- `BOX` / `TEXT` take designated initialisers.
- `TEXTF` takes a **parenthesised** property list, then a format string.
- `CHILDREN(parent, ...)` appends children and returns the parent, so calls nest.
- Every string is copied into the arena, so passing a stack buffer is safe.

Memory comes from a per-frame arena that is reset each frame. Allocation never returns NULL: on exhaustion it returns scratch space and sets `arena.oom`, which is logged once per frame.

## Seeing a change without a device

The client renders any tree file, with no server and no Kobo:

```bash
make preview UI=tree.json        # PNGs in build/
make preview-live URL=http://host/dashboard TOKEN=...
```

Its own fixtures (`fixtures/*.json`) are trees too — exactly what a server sends — and `make test` checks the client's layout of them. What a particular server *sends* is that server's to test: render your server's output to a file and keep it as a golden, or preview it here.
