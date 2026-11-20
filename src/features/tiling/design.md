# tiling

A built-in tiler, on by default on a live desktop. A window that opens goes into the slot of its layout instead of floating
where the app put it. It uses documented Win32 only, runs on the GUI thread, polls nothing and
injects nothing. A shell turns it off with `Tiling.enabled = false`, a run with `--no-tiling`.

```qml
Component.onCompleted: {
    Tiling.layout = "mainStack"   // see Layouts
    Tiling.gap = 8
    Tiling.ratio = 0.5
    Tiling.rules = [{ process: "Calculator.exe", action: "float" },
                    { class: "TaskManagerWindow", action: "ignore" },
                    { process: "Game.exe", action: "tile" },
                    { process: "zen.exe", action: "settle" }]
    Tiling.enabled = true
}
```

## Parts

| File | Role |
|---|---|
| `layouts.*` | pure geometry: `layoutSlots`, `outerRect` (border compensation), names |
| `tiling_model.*` | per `(HMONITOR, desktop GUID)` ordered slots (a slot is one window or a stack of windows), layout override, float set |
| `tiling_rules.*` | user rules (`process`, `class`, `title` regex -> `ignore`, `float`, `tile` or `settle`) and `classify`, the eligibility function |
| `window_placer.h` | the interface to the machine: enumerate, query facts, work area, shown desktop, `place`, `raise`, `activate` |
| `tiling_engine.*` | the behaviour: events in, placements out |
| `tiling_events.h` | event kinds, WinEvent codes, the `TilingHooks` interface |
| `tiling_command.*` | the `tile` verb of `wakuc` over `TilingControl` |
| `win32/` | `WindowPlacer` and `TilingHooks` for Windows |
| `qml/tiling_service.*` | the `Tiling` singleton: settings, coexistence guard, screen debounce |
| `testing/fake_window_placer.h` | fake placer and manual clock for the tests |

Tests never touch a real window: the engine sees the machine through `WindowPlacer`, and time
through `Scheduler`.

## Events

One out-of-context range hook per group (`kernel::WinEventHook`): destroy, show and hide; name
change; cloak and uncloak; minimize start and end; move and size start and end; foreground.
Location change is hooked only during a managed drag in a layout that resizes by mouse or under
`Tiling.dragAction: "swap"`, and its callback forwards only the dragged window. The hook filters
`idObject == OBJID_WINDOW`, `idChild == 0` and `GetAncestor(GA_ROOT) == hwnd` (not for destroy: a destroyed window has no ancestor). Events from
waku's own process are skipped by the hook (`WINEVENT_SKIPOWNPROCESS`).

- **A new window.** `Show` arrives before the final title and size. The engine evaluates one
  event-loop turn later; a window that is still minimized or untitled is looked at again every
  100 ms, five times at most. `NameChange` looks again only at a window that is new or was rejected
  for want of a title.
- **A window goes.** `Destroy`, `Hide` of a hidden window, and `MinimizeStart` take it out of its
  stack. `MinimizeEnd` evaluates it again; it returns at the end of the stack.
- **Cloak.** A desktop switch cloaks every window of the old desktop. A cloaked window whose
  desktop is not the shown one keeps its place in that desktop's stack; one that cloaked itself
  on the shown desktop leaves. The decision waits 150 ms because the registry value that names the
  shown desktop can trail the event.
- **Desktop switch.** Windows has no documented notification. The first `Uncloaked`, `Cloaked` or
  `Foreground` after a switch reads `CurrentVirtualDesktop` (`vdesk::currentDesktopId`, the row
  that already is on the allowlist) and retiles the new desktop. No timer.
- **User intent.** `MoveSizeStart` remembers the bounds. A title-bar translation floats the
  window at `MoveSizeEnd` (with `Tiling.dragAction: "swap"` it drops it instead, see Drag to
  swap). In `mainStack`, `columns`, `rows` and `centerMain`, a size change remains tiled and moves
  the boundary it was dragged on (see Mouse resize). The neighbours are placed at the release, a
  preview shows them until then. Flipped layouts use the same shared edges. Outer edges stay constrained to the work area, gaps remain intact, and the
  window is placed back into its final slot on release. Other layouts keep the prior
  move/resize-floats behavior. A clamped window is compared against the bounds it had at
  the start. Cancellation restores the starting sizing; destroy, stop and cancellation clear
  the active resize tracking.

## Eligibility

A window is managed unless: not visible; cloaked; minimized or untitled (retry); a window of waku;
owned by a visible window; a tool window; without `WS_THICKFRAME` or `WS_CAPTION`; maximized after
startup; fullscreen; a splash or shell class (`isBuiltinIgnoredClass`); ignored by a rule; or the
window of an elevated process while waku is not elevated (UIPI forbids moving it; unmanaged, no error).
A `float` rule keeps the window, unplaced; `toggle` tiles it. A tiled window that `toggle` or
`float` takes out of the layout gets back the size it had before the tiler first placed it (its
launch size, or its size when waku started), centred on the tile it leaves and kept inside the
work area. When a floating window is tiled again with `toggle`, the size it had while floating
replaces that size, so the next float gives back the size the user last chose. A window floated by
a drag keeps the size the user gave it. A maximized managed window keeps its place in the stack
but takes no slot.

A `tile` rule manages a window that has no `WS_THICKFRAME` or no `WS_CAPTION` (a game with a
fixed-size or frameless window). It lifts those two checks only: a fullscreen, maximized, tool or
elevated window stays unmanaged. Whether the window redraws at the size of its slot is up to the
application; one that refuses the size is handled like any other clamped window.

A `settle` rule is for an application that puts back the size and position it saved when it last
closed, after it has shown, over the tiler's placement. Zen and Firefox do this, and do it in steps:
their saved position at once, their saved width about half a second later. Without the rule such a
window opens in its slot, then shrinks back to its saved bounds (half the width, when it last shared
the screen with another tile), and the tiler leaves it there as a clamped window. The rule lifts no
check; a window it names is managed as any other and, right after it opens, placed again once it has
stopped changing its own bounds (see Placement). Add one only for an application that does this:
under the rule a window's own changes in its first 10 s, a Win+Arrow snap included, are undone.

```qml
Tiling.rules = [{ process: "zen.exe", action: "settle" },
                { process: "firefox.exe", action: "settle" }]
```

On load or shell reload, the initial scan waits one event-loop turn so the shell can set its rules.
It restores otherwise eligible maximized windows with `ShowWindowAsync(SW_SHOWNOACTIVATE)` and waits
for the maximized state to clear before placing them. Restoration is requested once, with bounded
completion checks; a hung window cannot block the bar or cause an endless retry. Windows on another
virtual desktop wait until that desktop is shown. Minimized windows and ignore or float rules
remain untouched. Later maximization stays under the user's control until the next load or reload,
or an explicit `tile toggle`: on a maximized window, that command requests the same bounded
restoration before tiling instead of floating it. The host auto-hides docked panels during
maximization, releases their work area, and restores their reservation once maximization ends.

## Placement

`SetWindowPos(NOZORDER | NOACTIVATE | NOSENDCHANGING | NOCOPYBITS | FRAMECHANGED |
ASYNCWINDOWPOS)` per window; not `DeferWindowPos`, which fails as a batch on one bad window. The
async flag keeps a hung app from blocking the GUI thread, so the readback right after the call can
be stale: 150 ms later the engine reads the bounds again. A window that clamps (a minimum size)
keeps its slot, leaves a gap and is counted in `status`. The one exception is a window a `settle`
rule names, right after it opens: such an application puts back the bounds it saved when it last
closed, over the placement, possibly seconds later and in steps (Zen: its saved position, then
300 ms later its saved width). For 10 s (`kSettleMs`), a readback or a location event that finds such a window off its
slot starts a 300 ms wait (`kSettleQuietMs`) that each further change restarts; when it runs out
the window is placed again, three times at most (`kSettlePlacements`). Placing again at once lost
to Zen: the corrections landed between its steps. A real clamp costs the three extra placements and
is then left alone. Changes after the period are the window's own. With `--log-file`, every decision is a
debug line starting `tiling:` (manage, float, ignore with its reason, place, readback, a window
that moved itself), naming the window by handle, process and class, never by title. The slot is the
visible bounds; `outerRect` adds the invisible border per edge (`GetWindowRect` minus
`DWMWA_EXTENDED_FRAME_BOUNDS`). The work area is `rcWork` of the window's monitor in physical
pixels. A cloaked window is never placed. Retile requests are coalesced: any number in one turn
place each window once.

Screen changes (`screenAdded`, `screenRemoved`, `availableGeometryChanged`, `geometryChanged`)
retile after 400 ms.

## Drag to swap

As in Komorebi and GlazeWM, the mouse can rearrange tiles: drag a tiled window by its title bar
onto another and the two trade places. It is opt-in; by default a title-bar drag floats the window
(see Events, "User intent").

### Behaviour

Opt-in with `Tiling.dragAction`: `"float"` (default) or `"swap"`. With `"swap"`,
a title-bar drag of a managed window ends by where the cursor is when the button is released:

| Released over | Result |
|---|---|
| another tile that holds one window | the two tiles trade places (`TilingModel::swap`) |
| a tile that holds a stack | the window joins that stack and becomes its top (`TilingModel::stackOnto`) |
| its own tile or a gap | it goes back to its tile (a retile) |
| another monitor | it joins that monitor's layout at the end, as a new window would (`TilingModel::add`) |
| anywhere, with the float key held | it floats, as with `"float"` |

- The float key is `Tiling.dragFloatModifier`: `"ctrl"` (default), `"alt"` or `"shift"`. Ctrl is
  the default because Windows and PowerToys leave it free during a title-bar drag: PowerToys
  FancyZones shows its zones while Shift is held in a drag, and PowerToys Grab And Move starts a
  drag with Alt + left click (such a drag may not be a system move loop, so the tiler may not see
  it at all). Alt also opens the menu bar of classic apps when it is released.
- `swap` trades whole slots: dragging the top window of a stack onto a single tile carries the
  stack with it, as `wakuc tile move` does. `stackOnto` moves only the dragged window; the stack it
  leaves shows its next window.
- A drop on a single tile always swaps; there is no drag that makes a new stack. Stacks are made
  with the `tile` commands and keybinds, as today.
- Another monitor is the monitor under the cursor at release. Only the dragged window moves: it
  leaves its stack (the stack shows its next window) and is appended to the workspace of that
  monitor on the shown desktop (`TilingModel::add`, which also removes it from its old workspace).
  Until its first placement there, most of the window can still be on the old monitor, which
  `retile` would read as a move by a hotkey and send back; `dropped_` marks it so that placement
  goes through. Dropping at a chosen spot in the other monitor's layout is left for later (Open
  questions).
- A drag that Windows ends with a Snap (released at a screen edge, Windows resizes it) floats the
  window as today: `MoveSizeEnd` finds the size changed, which is not a swap. See Snap.
- Resize drags are unchanged: in `mainStack` they move the shared edges (ratio and weights).
- Left out: a workspace in monocle (a drag there floats the window or snaps it back, as under
  `"float"`) and a paused tiler (nothing is placed, so a drag floats as under `"float"`).

### Live drop indicator

While the window moves, where it would land is shown: the tile it takes, or on another monitor the
exact spot it gets at the end of that layout. A drop back into its own tile is shown fainter, a
drop that floats is not shown. Nothing is shown before the window leaves its tile, so a click on a
title bar draws nothing. Windows sends location events during a drag only with "Show window
contents while dragging" on (the default); without it the target is decided at the release only.

- **Location events for move drags.** `TilingHooks::trackLocation` follows one window: a
  `mainStack` resize drag (`tracksMouseResize`) or any drag of a managed window under `"swap"`
  (`tracksLocation`), for the drag's length. The hook stays out-of-context and per-window; it is
  removed at `MoveSizeEnd`, at destroy and at stop. A drag whose window changes size is a resize:
  its swap state ends there.
- **Target in the engine.** At `MoveSizeStart` the engine keeps the workspace's tile rectangles
  (`slotsFor` over `activeWindows`), as the resize drag does. Each `LocationChange` reads the
  cursor and finds the tile under it, or for another monitor the last slot of its layout with one
  window more (`spotAtEnd`, worked out once per monitor per drag); only a change of target or
  result goes out. The same lookup decides the drop at `MoveSizeEnd`, so what is shown is what
  happens.
- **Placer calls.** `WindowPlacer::cursorPosition()` (`GetCursorPos`, physical pixels like the
  work area and the slots), `WindowPlacer::monitorAt(point)` (`MonitorFromPoint`) and
  `WindowPlacer::heldKeys()` (`GetAsyncKeyState` for Ctrl, Alt and Shift). The fake placer has
  settable `cursor` and `keys`, and its `place` moves a window to the monitor that holds it.
- **To QML.** `Tiling.dropTarget`: `{ active, kind ("swap", "stack", "monitor", "float", "back"),
  x, y, width, height, screen }` in the logical coordinates of the screen it is on, with a change
  signal. For `"monitor"` the rectangle is the spot on that monitor. Physical to logical goes
  through the screen that holds the monitor: `QScreen::geometry()` keeps the physical origin with
  logical size, as in `physicalScreenRect` (host/app_bar), so per-monitor scale is handled there.
- **Drawing.** waku ships a ready-made component, `TilingDropIndicator` (`tiling/qml/`), that a
  shell adds to its `ShellRoot` with one line. It is a `PanelWindow` per screen on the Overlay
  layer, click-through (`mask: Region {}`) and never focused, on screen only while `dropTarget`
  is `active` on its screen: a rounded rectangle with an accent border and a see-through fill
  that fades in and out and glides from tile to tile. A feature may ship window QML: its QML uses
  the `host` types (`docs/architecture.md`).
- **Colours.** The defaults come from waku's `Theme` (`Theme.accent`, `Theme.radius`), which only
  shells built on `Theme` set. A shell with a colour system of its own (Octalia's `Color`, Material
  3 names, possibly generated from the wallpaper) binds the component's `color`, `borderColor` and
  `radius` to its own values: `TilingDropIndicator { color: Color.mPrimary }`. The bindings follow
  every change of the shell's colours. A shell that wants its own drawing can skip the component
  and read `dropTarget` itself.

### Where it lives

| Part | Change |
|---|---|
| `tiling_engine.*` | `MoveDrag` next to `ResizeDrag` (key, tiles at start, spots on other monitors); `updateMoveDrag` from `onLocationChange`; `dropMoved` in `onMoveSizeEnd` before the float; `dropTarget` and `setDropListener` |
| `window_placer.h`, `win32/window_placer_win.cpp`, `testing/fake_window_placer.h` | `cursorPosition`, `monitorAt`, `heldKeys` |
| `qml/tiling_service.*` | `dragAction`, `dragFloatModifier`, `dropTarget` and its signal, the physical to logical mapping, `trackLocation` for move drags (`tracksLocation`) |
| `qml/TilingDropIndicator.qml` | the indicator window |
| tests | swap, join a stack, own tile, gap, the float key and its setting, a Snap, a stack carried by a swap, another monitor (appended at the spot shown, only the dragged window leaves its stack), a click, the target cleared on destroy and stop, the service's settings and hook tracking |
| docs | this section; `docs/wiki/wakuc.md` has the options and the component |

### Later

- Windows the tiler floats by rule are not tiles, so a drag of one stays a plain move.
- Dropping onto another monitor at the spot under the cursor,
  instead of at the end. Needs an insert at a position in `TilingModel` (today `add` appends) and
  the target monitor's tiles in the drag state.

## Mouse resize

A border drag keeps the window tiled in `mainStack`, `columns`, `rows` and `centerMain`
(`TilingEngine::resizesByMouse`); in `mainStackTop`, `bsp` and `grid` it floats the window (see
Later).

### Behaviour

A drag of an inner edge (one that another tile shares) moves the boundary between the tiles that
share it. Only the dragged window changes while the button is held (Windows sizes it); the others
are placed once, at the release, and until then `Tiling.resizePreview` gives every tile of the
workspace at the size it will get, which `TilingDropIndicator` outlines. An outer edge (on the
work area) goes back at the release, a corner drag changes both of its edges, and a cancelled drag
(Esc) restores the sizes it started with and moves nothing.

The neighbours were first placed live, on every location event. That does not hold up: 2 to 4
`SetWindowPos` calls per mouse event are more than Electron, CEF and Firefox windows repaint, so
they show black and stale frames and the windows of one column drift apart as each lags by its own
amount, and a neighbour at its minimum size covers the dragged window until the release.

| Layout | Edge dragged | What changes |
|---|---|---|
| `mainStack` | between the main window and the stack | `ratio` |
| | between two stacked windows | their height weights |
| `columns` | between two columns | the width weights of the two |
| `rows` | between two bands | the height weights of the two |
| `centerMain` (3 windows or more) | either side of the main window, or the inner edge of a side window | `ratio` |
| | between two windows of the same side column | their height weights |
| `centerMain` (2 windows) | as `mainStack`, which is what `layoutSlots` draws for two | |

- A pair trades share and keeps its sum, each weight stays in `kMinWeight..kMaxWeight` (0.3..3),
  and the other windows of the row or column do not move.
- `centerMain` stays symmetric: one `ratio` sizes the main window and the two side columns stay
  equally wide. Dragging the main window's right edge out by d widens it by 2d; its left edge moves
  out by d as well.
- A drag that moves both edges of an axis at once is not a border drag: it is Windows placing the
  window, a Snap at the release (`ResizeDrag::snapped`). The sizes go back to where they were and
  the window floats, as after a move.
- `tile resize` follows the same settings: `horizontal` is the focused window's width weight in
  `columns`, `vertical` its height weight in `rows` and in a side column of `centerMain`, and the
  ratio elsewhere as before. Only `grid` answers "does not resize".

### Sizes and layouts

- `layoutSlots` reads weights in all four: `columns` and `rows` over every window (the main window
  included, unlike `mainStack`), `centerMain` per side column with the weights of the windows in it.
  Equal weights keep the plain `spans`, so nothing moves in a workspace that was never resized.
- Sizes given by hand belong to the layout they were given in. When a workspace's layout changes
  (`tile layout`, the cycle, or a new `Tiling.layout` for the workspaces that follow it), its ratio
  and weights are dropped (`TilingModel::resetSizes`) and the new layout starts from its defaults.
  Flips and monocle stay. Setting the layout it already has changes nothing.
- Weights belong to windows, as in `mainStack`: a window moved by `tile move` or a swap drag takes
  its width or height with it, and a closed window's weight goes with it.

### Engine

`onLocationChange` takes each moved edge of the dragged slot (start slots from `slotsFor`, so
flips are already in them) and:

- **finds the slot across it** (`slotAcross`): a gap away from that edge, within the tolerance,
  and overlapping it along it (the most, when several do). None: an outer edge. The overlap keeps
  a window of the other side column of `centerMain` out;
- **asks the layout what the edge is** (`edgeRole`, by the layout as drawn: `centerMain` with two
  windows is `mainStack`): a side of the main window is the ratio, an edge between two windows of
  a column or a row is the pair's weights;
- **ratio** (`ratioFromEdge`): `mainStack`, the main width over the width left by the gaps;
  `centerMain`, the main width that keeps it centred, `2 * (edge - (inner.x + gap)) - usable` for
  its right edge and the mirror for its left, over `usable`; clamped to 0.1..0.9;
- **weights** (`shareEdge`): the share the pair needs for the edge to be where it is now;
- **publishes the preview** (`ResizePreview`: `slotsFor` over the tiles at the start, with the
  ratio and weights as they now are) instead of retiling. `MoveSizeEnd`, destroy, stop, pause and
  a Snap clear it; the release retiles the workspace once.

A numeric search was weighed (re-run `layoutSlots` and bisect the setting until the edge lands
under the cursor): it would cover every layout with less code each, but for these the two
primitives are shorter and easier to read. It may suit `bsp` and `grid` later.

### Later

- `mainStackTop`: the `mainStack` table turned on its side, with weights on the bottom row.
- `bsp` and `grid`: new kinds of setting (a share per cut, row and column weights) and decisions of
  their own.

## Snap

Snap Layouts and Snap Assist are not suppressed: a window the user snaps by mouse floats (move and
size end). With `Tiling.dragAction: "swap"` a snap still floats the window (see Drag to swap).

## Commands

## Layouts

The names are case-insensitive and ignore `-`, `_` and spaces (`main-stack-top` is
`mainStackTop`). `Tiling.layout` is the default; `wakuc tile layout <name>` sets one workspace;
`wakuc tile layout next|previous` walks the list below for the focused window's workspace, wrapping
(`tile status` shows the layout in force). `ratio` (0.5 by default) is the share of the first
split: of the width for `mainStack`, `bsp` and `centerMain`, of the height for `mainStackTop`. One
window fills the area in every layout. `tile flip` mirrors the slots of any of them.

```text
mainStack            mainStackTop         columns              rows
+-----+-----+        +-----------+        +----+-----+----+    +-----------+
|     |  2  |        |     1     |        |    |     |    |    |     1     |
|  1  +-----+        +-----+-----+        | 1  |  2  | 3  |    +-----------+
|     |  3  |        |  2  |  3  |        |    |     |    |    |     2     |
+-----+-----+        +-----+-----+        +----+-----+----+    +-----------+
                                                                |     3     |
                                                                +-----------+
bsp                  grid                 centerMain
+-----+-----+        +-----+-----+        +----+-------+----+
|     |  2  |        |  1  |  2  |        | 3  |       | 2  |
|  1  +--+--+        +-----+-----+        +----+   1   +----+
|     |3 |4 |        |  3  |  4  |        | 5  |       | 4  |
+-----+--+--+        +-----+-----+        +----+-------+----+
```

- `mainStack`: main left, the rest stacked right, sized against each other by height weights.
- `mainStackTop`: main on top, the rest side by side below.
- `columns`, `rows`: columns or bands, equal until resized (width or height weights).
- `bsp`: window 1 takes `ratio` of the width; every next window takes the top (then left, then
  top...) half of what is left. 
- `grid`: ceil(sqrt(n)) columns filled by row; a short last row spreads over the width. Does not
  resize.
- `centerMain`: main in the middle; the others alternate right, left, right... in two columns,
  each sized by its windows' height weights. With two windows it is `mainStack`.

`wakuc tile ...` acts on the foreground window (`TilingControl`, `tiling_command.h`; the service
answers `ok: ...` or `error: ...`). The pure logic is in `layouts.cpp` (`flipSlots`,
`neighbourSlot`, weights) and `tiling_model.cpp` (`swap`, `promote`, the per-workspace
`WorkspaceView`); the engine ties them together and a fake placer tests them.

- `focus left|right|up|down`: the nearest tiled window of the workspace by slot centre, the
  sideways offset counting double, so "right" of the main window is the stack window at its
  height. No wrap. `focus next|previous` keep the stack order.
- `move <direction>`: swaps the focused window with that neighbour in the stack order and
  retiles. `promote`: the focused window takes slot 0 (unchanged if it already has it).
- `resize horizontal`: changes the `ratio` of the layout by 5% per call (a stack window
  that grows takes width from the main window), clamped to 0.1..0.9, per workspace; in `columns` it
  is the focused window's width weight. `vertical`: a height weight (step 0.1, 0.3..3) for a
  stacked window of `mainStack` (three or more windows), a band of `rows` or a window of a
  `centerMain` side column that holds two or more. `grid` does not resize. A change of
  `Tiling.ratio` clears the overrides; a change of layout drops the workspace's sizes.
- `flip horizontal|vertical`: mirrors the slots of the layout in the work area; again undoes it.
- `monocle`: the focused window gets the whole work area (inside the gap); the other windows are
  not touched, so they stay behind it. Again re-tiles. A monocle window that leaves the
  stack ends the mode.
- `pause`: no window is placed while paused (windows are still tracked, a drag does not float
  one); resuming retiles. Not the same as `Tiling.enabled = false`, which drops every hook.
- `close`, `minimize`: any visible foreground window, tiled or not, except windows of waku and the
  desktop and taskbar classes. `close` posts `WM_CLOSE` (a window may ask to save), `minimize`
  uses `ShowWindowAsync`; neither blocks on a hung window.

## Stacking

Several windows can share one slot of the layout. A workspace is an
ordered list of slots (`Slot` in `tiling_model.h`); a slot holds an ordered list of windows and
the index of the top one. A workspace with no stacks is a list of one-window slots, so it behaves
as before. The layout counts slots, not windows.

- `stack left|right|up|down`: the focused window leaves its slot and joins the stack of the
  slot nearest in that direction (the same geometry as `focus`). It becomes the top there. The
  slot it left is dropped when it was alone; else the window under it shows.
- `unstack`: the focused window leaves its stack and gets a slot of its own right after the
  stack's slot. A stack of one dissolves by itself.
- `cycle-stack next|previous`: the next or previous window of the focused window's stack (wrapping)
  becomes the top and takes the focus.
- Only the top window is a layout slot. The windows under it are placed at the same rectangle
  (`place`, no activation) and stay managed: they are not minimized or hidden. A window that
  becomes the top of a stack with two windows or more is raised with `SetWindowPos(HWND_TOP,
  SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)` (`WindowPlacer::raise`); nothing else is raised, so
  floating windows keep their order.
- A window under the top that takes the foreground (alt-tab, the taskbar) becomes the top.
- A window that closes, hides, minimizes, floats, or leaves the desktop leaves its stack. When it
  was the top, the next window down the list shows. The height weight and the monocle mark
  follow the top of a slot.
- `focus left|right|up|down`, `move`, `promote`, `resize`, `flip` and `monocle` act on slots: focus
  goes to the top of the neighbouring slot, `move` and `promote` move the whole stack, `resize` and
  `monocle` size the slot. `focus next|previous` visit slots.
- `status` prints `stacks=N`, the number of slots with more than one window.

## Decisions

- State is in memory only. A reload of the shell builds a new service, which scans the open
  windows again; floats chosen with `toggle` are forgotten.
- One layout name per workspace, set by `wakuc tile layout`; `Tiling.layout` is the default for
  workspaces without one and clears the overrides when it changes.
- New windows go to the end of the stack; the first window is the main one.
- No undocumented API. The shown desktop reuses the registry value on the allowlist.

## Open points

- `LocationChange` reaches the engine only during a `mainStack` resize drag or a swap drag, so a
  settling window's own changes are seen at the readbacks only.

- Keyboard Snap (Win+Arrow) and a maximize emit no event the tiler hooks, so a snapped window is
  noticed at the next retile; a window maximized or restored by the user takes or frees its slot
  only then. `retile` fixes it by hand.
- No per-monitor gap and no persistence. Flips, ratio, weights and monocle live in
  memory per workspace.
- Exclusive-fullscreen games are detected only by their bounds.
- UWP frames (`ApplicationFrameWindow`) that animate their size can clamp for a moment.
