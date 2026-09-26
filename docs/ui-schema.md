# Kobo UI schema

The Kobo client describes a screen as a tree of nodes, lays it out, and paints
it. Nothing positions itself; nothing tracks a `y` cursor; nothing hard-codes a
touch rectangle. This document is the reference for that tree.

The schema is a deliberate subset of CSS flexbox — enough for the dashboard,
small enough to hold in your head. It maps 1:1 onto JSON, so the tree can later
be produced by the server instead of by C without redesigning anything.

## Pipeline

```
screens.c          builds a tree from DashboardData
  └─ ui_layout.c   one pass down (constraints), one pass back (sizes),
                   one pass assigning absolute rects
       └─ painter  paint_fbink.c on the device, paint_stb.c for the preview
            └─ ui_hit_test()  maps a touch to the node that was actually drawn
```

`ui_text.c` measures text for the layout. It re-implements FBInk's OpenType
line-breaking arithmetic exactly (see the comment at the top of that file), so
what the layout measures is what FBInk draws — and so the host preview can
measure without a framebuffer.

## Node kinds

| kind   | purpose                                         |
|--------|-------------------------------------------------|
| `box`  | container, background, border, padding, gaps    |
| `text` | a string, wrapped and optionally ellipsized     |

There is no separate rule/divider/spacer kind: a `box` with a fixed size and a
background is a divider, and one without a background is a spacer.

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

A definite length is a border-box size: padding and border come out of it, not
on top of it.

## Layout rules

- **Main axis** is horizontal for `dir: row`, vertical for `dir: col`. Children
  are laid end to end with `gap` between them.
- **Cross axis** children default to `stretch`, which is why text wraps at the
  full available width without being told to. Stretch only applies to a child
  whose cross-axis length is `auto`; an explicit `w`/`h` always wins.
- **Flex** needs a definite main-axis size on the parent. Inside an
  `auto`-sized parent, a flex child behaves as `auto`.
- **A definite height caps a text node's line count.** This is what keeps a long
  title inside its card without any manual bottom checks.
- **Clipping** drops whole children, never half of one, so a task card is either
  fully drawn or not drawn at all.
- Nodes are painted in tree order, so a later sibling draws over an earlier one.

## Actions

```c
.action = ACTION(a, .kind = UI_ACT_POST,
                    .path = "/api/tasks/complete.php",
                    .body = ui_strf(a, "task_id=%d", task->id),
                    .toast = "TASK COMPLETED!")
```

| kind | JSON | effect |
|---|---|---|
| `UI_ACT_POST` (1) | `{"kind":1,"path":...,"body":...}` | form-encoded POST, then refresh |
| `UI_ACT_REFRESH` (2) | `{"kind":2}` | re-fetch and redraw |
| `UI_ACT_QUIT` (3) | `{"kind":3}` | exit the app |
| `UI_ACT_OPEN` (4) | `{"kind":4,"target":"..."}` | reserved for server-driven screens |

`toast` is optional optimistic feedback: the node's rect is replaced with that
text while the request is in flight.

`ui_hit_test()` returns the deepest node with an action whose **painted rect**
contains the touch, so a touch target can never drift out of sync with what is
on screen.

## Building a tree

```c
UiNode *row = KIDS(BOX(a, .dir = UI_ROW, .gap = 8),
        TEXT(a, "Mon", .size_pt = 10, .w = FLEX(1)),
        TEXTF(a, (.size_pt = 10, .align = UI_ALIGN_END), "%d°", temp));
```

- `BOX` / `TEXT` take designated initialisers.
- `TEXTF` takes a **parenthesised** property list, then a format string.
- `KIDS(parent, ...)` appends children and returns the parent, so calls nest.
- Every string is copied into the arena, so passing a stack buffer is safe.

Memory comes from a per-frame arena that is reset each frame. Allocation never
returns NULL: on exhaustion it returns scratch space and sets `arena.oom`, which
is logged once per frame.

## Going server-driven (not built)

The tree is already JSON-shaped, and `ui_write_json()` emits exactly the format
above (plus the computed `rect`, for tests). Making the server own the UI would
need:

1. `/api/ui.php` emitting this JSON instead of raw dashboard data.
2. `ui_from_json()` on the device, over cJSON — the mirror of `ui_write_json()`.
3. Pointing `preview --url` at `/api/ui.php`.

Nothing in the schema, the layout engine, the painters or the touch handling
would change. That is the point of writing it down this way.
