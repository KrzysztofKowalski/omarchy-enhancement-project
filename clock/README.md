# clock/ — seconds in the Omarchy bar clock

## What it does

Two changes to Omarchy's clock widget, both on a **local plugin clone** so
package updates cannot overwrite them:

| Change | Where |
|---|---|
| The bar clock shows `HH:mm:ss` and **ticks every second** | `BarWidget.qml` + `shell.json` |
| Calendar day names follow the **system locale** instead of hardcoded `en_US` | `Panel.qml` |

## Problem solved

### 1. Seconds need two changes, not one

`SystemClock` only emits `dateChanged` as often as its `precision`. Stock ships

```qml
SystemClock {
  id: clock
  precision: SystemClock.Minutes
  ...
}
```

so it fires **once a minute**. Adding `:ss` to the format alone therefore does
not give you seconds — it gives you a frozen `:00`, because nothing refreshes
that field. You need both:

| File | Change |
|---|---|
| `BarWidget.qml` | `precision: SystemClock.Minutes` → `SystemClock.Seconds` |
| `shell.json`, the clock entry | `"format": "dddd HH:mm"` → `"dddd HH:mm:ss"` |

### 2. Day names are hardcoded to English

`Panel.qml` carries `readonly property var labelLocale: Qt.locale("en_US")` and
derives every day name from it. The clone uses `Qt.locale()` (the system
locale) instead, and trims the trailing period some locales put on the short
form (`"man."` → `"MAN"`) so the grid header stays an even band of capitals.

### Why a clone

Both files live in `/usr/share/omarchy/shell/plugins/panels/clock/`, which
package updates overwrite. `omarchy plugin clone omarchy.clock` copies the
widget into `~/.config/omarchy/plugins/$(id -un).clock/` and points the bar at
the copy. Never edit `/usr/share/omarchy/`.

### Two non-obvious traps

- **Precision must match the finest token in the format.** `Minutes` + `:ss` =
  seconds stuck at `:00`; `Seconds` without `:ss` = a clock that repaints every
  second and shows nothing new. Change them together.

- **Inside a plugin, `bar` is a facade — and one of its properties is
  read-only.** For a *registered plugin* widget, `root.bar` is not the bar: it
  is `qs.Ui.PluginBarApi`, which declares

  ```qml
  readonly property bool centerHoverRevealSuppressed: _centerHoverRevealSuppressed
  ```

  So this "simplification" of upstream's function looks harmless and is not:

  ```js
  // BROKEN — the property exists, so the guard passes and the assignment throws
  if (root.bar && "centerHoverRevealSuppressed" in root.bar)
    root.bar.centerHoverRevealSuppressed = value
  ```

  ```js
  // CORRECT (upstream) — PluginBarApi has the method, so this branch is taken
  if (root.bar && typeof root.bar.setCenterHoverRevealSuppressed === "function")
    root.bar.setCenterHoverRevealSuppressed(value)
  else if (root.bar && "centerHoverRevealSuppressed" in root.bar)
    root.bar.centerHoverRevealSuppressed = value
  ```

  An uncaught exception in QML JavaScript **aborts the function at the throw
  point** — the rest of the body silently never runs. In the clock's `close()`
  that call is the first line and `root.controller.hide()` comes after it, so
  the calendar **opened but would not close**: no crash, no UI error, nothing
  on screen. In `open()` the same call sits inside `Qt.callLater`, i.e. after
  `controller.show()`, which is why opening kept working.

  The shipped `Panel.qml` keeps upstream's version of that function. It is
  documented here because anyone editing a cloned panel will meet it, and
  because the log line is easy to miss:

  ```
  WARN scene: .../Panel.qml[119:-1]: TypeError: Cannot assign to read-only property "centerHoverRevealSuppressed"
  ```

  When a panel opens but will not close, check that first — it names the file
  and the line:

  ```bash
  journalctl -t omarchy-shell | grep -iE 'TypeError|read-only'
  ```

## Contents

```
clock/
├── install.sh                    # installer: install / --check / --uninstall
└── omarchy-plugin/
    ├── BarWidget.qml             # patched clone of omarchy.clock's BarWidget
    └── Panel.qml                 # patched clone of omarchy.clock's Panel
```

Both QML files are upstream Omarchy shell code (MIT, © David Heinemeier
Hansson). Against tag `v4.0.4`, `BarWidget.qml` differs by **one line** (the
precision) and `Panel.qml` by the locale changes plus a comment explaining the
read-only trap above.

`moduleName` is left at the stock `"omarchy.clock"` on purpose: `Bar.qml`
overwrites it with the entry id from the layout anyway, so changing it makes no
difference (A/B tested on a sibling clone).

## Use

```bash
./install.sh              # clone & patch the widget, set the format, restart the shell
./install.sh --check      # report the current state, change nothing
./install.sh --uninstall  # restore the stock widget and format
```

`install.sh` backs up `shell.json` before editing it and is idempotent.
`jq` is used for the JSON edit (`python3` is accepted as a fallback).

The shell **must** be restarted, not just reloaded: hot reload picks up the
format change, but the clock's timer does not reliably re-attach to a new
precision, and the seconds stay at `:00` until it does.

## Verification

| Test | Expected |
|---|---|
| Look at the bar | `HH:mm:ss`, the seconds digits changing every second |
| `./install.sh --check` | `precision: Seconds` and the format carrying `:ss` |
| `journalctl -t omarchy-shell \| grep -iE 'TypeError\|read-only'` | no output |
| Open the calendar, click outside it | it closes |

## Notes

- Right-clicking the clock walks a fixed list of formats
  (`shell/plugins/panels/clock/Model.js`) and writes the result back to
  `shell.json`. A hand-written format is appended to that ring, so a right-click
  can land on one without seconds — that is the widget's own behaviour, not
  something this module changes.
- Vertical bars (`position: left/right`) use `verticalFormat`, a separate key.
  This module only sets the horizontal `format`.
