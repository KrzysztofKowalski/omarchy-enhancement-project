# workspaces/ — 20 workspaces in the Omarchy bar (two banks of 10)

## What it does

Turns Omarchy's 10 workspaces into **20**, addressed as two banks, and makes the
bar show all of them:

| Keys | Action |
|---|---|
| `SUPER + 1..0` | switch to workspace 1..10 *(untouched default)* |
| `SUPER + SHIFT + 1..0` | move window to workspace 1..10 *(untouched default)* |
| `SUPER + ALT + 1..0` | **switch to workspace 11..20** |
| `SUPER + ALT + SHIFT + 1..0` | **move window to workspace 11..20** |
| `SUPER + CTRL + ALT + 1..0` | move window *silently* to 1..10 *(relocated)* |

## Problem solved

Hyprland itself has **no workspace limit** — the "10" is hardcoded in two places
in Omarchy, and neither is configurable upstream (verified against
`basecamp/omarchy`; both are still hardcoded there):

| # | File | Limit |
|---|------|-------|
| 1 | `default/hypr/bindings/tiling.lua` | `for workspace = 1, 10 do` — only binds digits to 1..10 |
| 2 | `shell/plugins/bar/widgets/Workspaces.qml` | `if (id > 0 && id <= 10 …)` — and it pre-seeds only `[1,2,3,4,5]` |

Both live in `/usr/share/omarchy/`, which package updates overwrite — so the fix
is a **local override**, never an edit there.

### Two non-obvious traps

- **Modifier order is irrelevant to Hyprland.** `SUPER+ALT+SHIFT+n` *is*
  `SUPER+SHIFT+ALT+n`, which the defaults already use for "move window silently
  to workspace 1..10". Binding bank-2 moves there would silently eat that
  binding. Fix: unbind the default and re-bind the silent move on
  `SUPER+CTRL+ALT+digit`.
- **Hyprland recycles an empty workspace** the moment you leave it. The stock
  widget lists only *existing* workspaces, so raising its cap alone is not
  enough — bank 2 stays invisible until something lives there. `workspaceIds()`
  must pre-seed the whole range instead of enumerating live workspaces.

## Contents

```
workspaces/
├── install.sh                     # installer: install / --check / --uninstall
├── test.sh                        # round-trip test on a throwaway fake $HOME
├── hypr/bindings-workspaces.lua   # appended to ~/.config/hypr/bindings.lua
└── omarchy-plugin/Workspaces.qml  # patched clone of omarchy.workspaces
```

The QML is upstream Omarchy shell code (MIT, © David Heinemeier Hansson) with
two changes: the id cap `10 → 20`, and `workspaceIds()` seeding `1..20`.
`moduleName` is left at the stock `"omarchy.workspaces"` on purpose — changing it
to the clone's own id makes no difference (A/B tested with a shell restart).

## Use

```bash
./install.sh              # append bindings + clone & patch the bar widget
./install.sh --check      # report the current state, change nothing
./install.sh --uninstall  # restore the stock 10-workspace layout
./test.sh                 # round-trip test on a throwaway fake $HOME
```

`install.sh` backs up `bindings.lua` before touching it and is idempotent. It
reloads Hyprland and rescans the shell plugin — the bar rebuilds over ~10 s, so
give it a moment before judging a screenshot.

The block is **detected by its body text, not by the `-- om-enh:workspaces`
marker it writes**. That matters on a machine where the block was appended by
hand and carries no marker: keying off the marker alone would append a second
copy and `--uninstall` would fail to find the original. Both cases are covered
by `test.sh`.

`--uninstall` restores the cloned widget to the stock QML (the bar shows 10
again) and removes the bindings block, leaving that file byte-for-byte as it
was before install. The now-unused clone directory stays behind — harmless;
delete it and point `shell.json`'s left section back at `omarchy.workspaces` if
you want it gone entirely.

## Warnings

- **`SUPER + ALT + 1..5` is given up** — that was "Switch to group window 1..5"
  (window grouping). Bank 2 needs those chords. If you use grouping, rebind bank
  2 elsewhere; `SUPER+CTRL+1..9` is *not* an option (it is "Bar panel 1..9"),
  which is why ALT was chosen here.
- Workspace 10 still renders as `0` (matching its key), bank 2 as `11`..`20`,
  and the focused workspace always renders as a glyph rather than a number —
  all stock behaviour, left alone.
- Omarchy dispatches through Lua, so `hyprctl dispatch workspace 14` is a syntax
  error. Use `hyprctl dispatch 'hl.dsp.focus({ workspace = "14" })'`.

## Going beyond 20

Widen the `for workspace = 1, N` loop in `hypr/bindings-workspaces.lua` and the
`ids.push` loop in `Workspaces.qml`. A third bank needs a *free* digit chord —
`SUPER+ALT+digit` and `SUPER+CTRL+ALT+digit` are spent by then, so either add a
modifier or switch to a submap picker
([pmpinto/hyprland-workspaces-above-10](https://github.com/pmpinto/hyprland-workspaces-above-10)).
