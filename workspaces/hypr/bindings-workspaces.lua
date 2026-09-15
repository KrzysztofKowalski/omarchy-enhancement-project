-- ============================================================================
-- Two banks of workspaces (20 instead of 10):
--   SUPER + 1..0               -> switch to workspace 1..10
--   SUPER + SHIFT + 1..0       -> move window to workspace 1..10  (Omarchy default, kept)
--   SUPER + ALT + 1..0         -> switch to workspace 11..20
--   SUPER + ALT + SHIFT + 1..0 -> move window to workspace 11..20
--
-- Two deliberate trade-offs:
--  * SUPER+ALT+1..5 was Omarchy's "Switch to group window 1..5". Bank 2 needs
--    those chords, so that binding is given up.
--  * SUPER+ALT+SHIFT+number is the SAME chord as SUPER+SHIFT+ALT+number
--    (modifier order is irrelevant), which the defaults use for "move window
--    silently to workspace 1..10". Those are unbound here, and the silent move
--    is re-bound on SUPER+CTRL+ALT+number so the behaviour is not lost.
--
-- Untouched: SUPER+CTRL+1..9 (Bar panel 1..9) and the whole bank-1 set.
-- ============================================================================
for workspace = 1, 10 do
  local key = "code:" .. tostring(workspace + 9)

  -- Bank 2: switch. Frees the chord from "Switch to group window 1..5".
  hl.unbind("SUPER + ALT + " .. key)
  o.bind("SUPER + ALT + " .. key, "Switch to workspace " .. (workspace + 10), hl.dsp.focus({ workspace = tostring(workspace + 10) }))

  -- Bank 2: move window. Collides with the default "move silently to 1..10".
  hl.unbind("SUPER + SHIFT + ALT + " .. key)
  o.bind("SUPER + ALT + SHIFT + " .. key, "Move window to workspace " .. (workspace + 10), hl.dsp.window.move({ workspace = tostring(workspace + 10) }))

  -- Relocated bank-1 "move window silently" (follow = false), so nothing is lost.
  o.bind("SUPER + CTRL + ALT + " .. key, "Move window silently to workspace " .. workspace, hl.dsp.window.move({ workspace = tostring(workspace), follow = false }))
end
