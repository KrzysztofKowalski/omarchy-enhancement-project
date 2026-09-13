// reclocked v4 — auto-reclocking GT 750M (GK107, Kepler) under nouveau.
//
// Builds on v3 (temperature/load policy, dwell counters, hwmon temp,
// mmap BAR0 PMU idle counters, vblank sync). WHAT'S NEW IN v4:
//
//   1. APP-AWARE PROFILES: `default` (cap 07, throttle) vs `preferred` (cap 0e
//      escalating to 0f on sustained high busy). The profile is chosen dynamically
//      based on the active app from Hyprland (hyprctl activewindow + clients).
//   2. CONFIG /etc/reclocked.conf: list of [preferred] classes + per-profile thresholds.
//      Own INI-line parser, no external deps.
//   3. MANUAL OVERRIDE: flag-file `/run/reclocked/override` — the daemon freezes auto
//      while it exists. `pstate.sh set` creates the flag-file, `pstate.sh auto` removes it.
//   4. DAEMON CONTROL: SIGHUP → re-reads the config without restart. systemd unit
//      + `reclockctl` wrapper.
//
// SAFE 3-state ladder [07, 0a, 0e] (indices 0,1,2). Ceiling from the profile
// caps UP. DOWN always available (TERMAL/IDLE). Never a 07→0e jump (UP by 1 step).
//
// 0f (max, 967/2500 MHz) IS EXCLUDED from the LADDER (auto ladder). Instead
// 0f acts as a BOOST TIER above the ladder — it enters ONLY when:
//   * current pstate == ladder top (0e), i.e. g_cur_idx == ceiling,
//   * AND busy > busy-boost (85%) for boost-dwell (5 s) — sustained heavy load,
//   * AND temp < temp-up of the preferred profile (75°C) for temp-dwell (5 s).
// 0f thermal guard (per preferred thresholds): temp > temp-up (75°C) OR temp >=
// temp-down (82°C) → IMMEDIATE drop from 0f to 0e (ladder top). 0f is the
// hottest pstate = first to be cut. Load exit: busy < busy-up
// (80%) → drop from 0f (hysteresis enter@85 / exit@80). 0f is NOT in the LADDER, so
// it is never a transitional ladder step — only a separate boost tier.
// Manual opt-in still available: `pstate.sh set 0f` creates an override (daemon stands).
//
// THERMAL GATING PER-PROFILE (NOT global): default (non-browser) temp-down=65,
// temp-up=58 — cautious throttle for terminals/editors. preferred (browser)
// temp-down=82, temp-up=75 — full performance, throttles only at 82°C.
// The thermal guard (TERMAL DOWN) is prioritized over load in BOTH profiles
// (for default cap=07 at idx 0 there is nothing lower, but the logic is present). Hysteresis
// load 80/40 (40 pp band).
//
// UP 07→0a→0e (UP-LOAD): g_cur_idx < ceiling AND temp < temp_up for temp_dwell
//                        AND busy > busy_up. One level per step.
// DOWN 0e→0a→07 (TERMAL|IDLE|CEILING): temp > temp_down for temp_dwell
//                        OR busy ≤ busy_down for idle_dwell OR g_cur_idx > ceiling.
// BOOST 0e→0f (BOOST-UP): g_cur_idx==ceiling AND busy>busy_boost for boost_dwell
//                        AND temp<temp_up for temp_dwell. Tier above the ladder.
// BOOST-DOWN 0f→0e: temp>temp_up OR temp>=temp_down (IMMEDIATE, priority)
//                        OR busy<busy_up (load hysteresis).
//
// Preferred signal: hyprctl -j activewindow (focus) + hyprctl -j clients
// (running). Preferred active when: focused.class ∈ list OR (running.class ∈
// list AND busy > busy_up). The profile dwell (default 2 s) rate-limits profile
// changes (alt-tab doesn't flicker the ceiling).
//
// Requires root. RUNTIME-ONLY SETTINGS: reboot resets the clocks.
// Safety: validation of pstate ∈ {07,0a,0e,0f}, never writes the same state,
// SIGTERM→restore --exit-state, fail-safe hwmon (no temp → thermal conditions
// skipped, never an emergency UP). No Hyprland/hyprctl → fallback to default.
//
// v4.1 (2026-08-25):
//   A. Hyprland detection BUG-fix: detect() ran once at daemon startup
//      (systemd → before the session), leaving the daemon on default cap=07 forever.
//      Now cyclic re-detect every poll_ms while !hypr_alive.
//   B. GR-idle gate: DOWN transitions (in-flight memory reclocking) deferred while
//      instantaneous busy > gr-idle-promille (default 300‰). vblank protects scanout,
//      not GR mid-render — a live DOWN under render = channel wedge (desktop crash).
//      UP-LOAD/BOOST-UP NOT gated (a rising clock is safer).
//   C. Discord/YouTube detection by window title ([preferred-titles], case-
//      insensitive) — they are browser tabs, with no class of their own.
//   D. [low-power] (terminals): terminal focus → forces default cap=07 with
//      priority over preferred (YouTube in the background doesn't raise the clock).
//   E. JSON escape fix in json_str/json_str_all (\" \\ \n \t).
//
// v4.2 (2026-08-25):
//   F. APPLESMC FAN CONTROL: class Fan — linear temp→RPM curve for
//      both fans (fan1=Left, fan2=Right). RPM ranges (fanN_min/max) read
//      DYNAMICALLY from sysfs at startup (not hardcoded — "per 2ch values" min/
//      max per fan). x = clamp((temp - temp_min) / (temp_max - temp_min), 0,1);
//      rpm = fanN_min + round(x * (fanN_max - fanN_min)). Updated every poll_ms
//      (1 s). Section [fan]: enable/temp-min(51)/temp-max(91).
//   G. FAN OVERRIDE: flag-file `/run/reclocked/fan-override` freezes the auto fan
//      (the user controls manually: cusfan.sh / fullfan.sh). reclockctl fan-off/on.
//   H. FAN FAIL-SAFE: restore_auto() at exit clears fanN_manual=0
//      → SMC takes over auto (never leave fans locked in manual).
//      init() failure (no applesmc / absurd ranges) → fan disabled without error.
//
// v4.3 (2026-08-25):
//   I. TITLE-MATCH = HIGHEST priority of the preferred signal. Window-title
//      matching (Discord/YouTube, section [preferred-titles]) sets the title_pref flag,
//      which bypasses the busy>busy_up rule in UP-LOAD: title-match AND temp<temp_up
//      for temp_dwell AND below ceiling → UP by 1 level WITHOUT the busy-gate (reason
//      UP-TITLE). Why: Discord/YouTube are memory-bound (GR busy 16-36%, never
//      >80%), so v4.1/v4.2 got stuck at 0a — the user felt no difference. Additionally
//      title_pref SUPPRESSES the IDLE downshift (IDLE reason skipped while title_pref)
//      — a focused app holds ceiling 0e. TERMAL DOWN remains active (thermal
//      protection > title priority). CEILING DOWN remains (low-power terminal
//      with focus > title-match — see low-power gate). UP-TITLE is NOT gated
//      by GR-idle (a rising clock is safer).
//
// v4.4 (2026-08-26):
//   J. [caps] — per-class policy. Syntax: "class = floor=0a, max=0e, busy-up=50".
//      floor = resting state (IDLE does not go lower), max = ceiling (own),
//      busy-up = own UP-LOAD threshold (%, 0 = global busy-up 80). A class
//      with an entry is preferred (when focused) and does NOT catch title-priority.
//      Desktop Discord (Electron "discord" + PWA "chrome-discord.com__channels_@me-Default"):
//      base 0a, busy>50% → 0e, idle → 0a. TERMAL still overrides everything (can go down
//      even below floor). A Discord card in a browser (chromium/firefox)
//      is not in [caps] → title-priority force-0e unchanged.
//
// v4.5 (2026-08-26):
//   K. SELF-HEAL AFTER S3: after suspend/resume (deep) the GPU loses power and
//      the busy PMU counter configuration in BAR0 (R_IDLE_CTRL/R_IDLE_MASK) —
//      counters stop counting, sample() returns a stale 1000‰, the IDLE downshift and
//      the GR-idle gate are blocked, the daemon stays at 0e (report 63). In the
//      main loop, read back R_IDLE_CTRL every cycle; when (ctrl & CTRL_VALUE_MASK) !=
//      CTRL_VALUE_ALWAYS (configuration lost — typically after resume), it runs
//      gpu.init_counters() + reset_after_transition() and logs to the journal.
//      The readback is cheap (mmap, every interval_ms) and has no false alarms
//      (does not depend on busy/temp). Works with the system-sleep hook
//      (restart on post-resume) as a safety net.
//
// v4.6 (2026-08-26):
//   L. COMPILER→FAN 100%: when a running compiler is detected
//      (clang/gcc/g++/cc/c++/cc1/cc1plus/make/cmake/ninja/cargo/rustc/... —
//      scan /proc/*/comm with a cmdline fallback), fans go to fan-max%
//      (default 100 = full fans). Section [compiler]: enable/fan-max/names
//      (names = extra names comma-separated). Fan.set_boost(pct) writes
//      manual=1 + output for both fans (linear interpolation between
//      fanN_min and fanN_max). Boost works ONLY in auto-mode — the fan-override
//      flag-file (/run/reclocked/fan-override) takes priority (does not override
//      manual control). When the compiler disappears → return to the temp curve
//      (normal path). Detection every poll_cycles (1 s), threshold — stop the
//      scan when found.
//
// v5.0 (2026-08-26):
//   M. SWITCHD — dGPU power-state + render-routing module (Stage 1: monitor in DIS).
//      Topology (vgaswitcheroo DIS/IGD), dGPU power (manual/runpm), policy
//      (hard/soft promotion, demote, min-residence, cooldown, thermal gate),
//      executor (power-on/off gated by topology), status /run/reclocked/status +
//      /run/switchd/dgpu, NVRAM read of gpu-power-prefs (efivarfs → raw flash),
//      igpu_freq_mhz read-only. In DIS = monitor (zero power changes). Sections
//      [switch]/[dpower]/[dgpu-hard]/[dgpu-soft]/[igpu]/[dgpu-procs].
//   N. DGPU OVERRIDE: flag-file /run/reclocked/dgpu-override (on|off) — pattern
//      like fan-override (G). reclockctl dgpu-on/dgpu-off/dgpu-auto. The daemon in tick()
//      forces the target BEFORE the policy (decide is skipped). Override "off" still
//      passes through the apply() gates: wait_idle (dGPU nodes only — lesson
//      from the G1 live test) + set_off + wait_off. Status: field "override" (""|"on"|"off").
//
// v5.1 (2026-08-27):
//   O. IGPU-ONLY FAN CURVE: a separate fan curve when only the
//      iGPU is on (dGPU OFF — in IGD the nouveau hwmon disappears, temp=CPU/coretemp). Section
//      [fan] gains keys temp-min-igd/temp-max-igd (default 41/91°C) —
//      quieter: fans hit max only at 91°C instead of 67°C. With the dGPU
//      ON → standard curve temp-min/temp-max (51/91). Curve choice by
//      sw.dgpu_off() (dGPU power state), independent of topology.
//
// v5.3 (2026-08-28):
//   P. NVRAM CACHE: cache gpu-power-prefs to /run/reclocked/nvram-prefs. Scanning the
//      raw flash (/dev/mtd0ro) at every startup kills the kbd backlight (read
//      of MTD → ledtrig_mtd_activity → trigger "nand-disk" on smc::kbd_backlight
//      → oneshot blink → LKSB=0; report 80). /run is tmpfs — the cache resets
//      at reboot (correct: prefs only change through firmware at
//      to-igd/to-dis + reboot).
//
// v5.4 (2026-08-28):
//   R. [dgpu-active] — THREE-STAGE PSTATE POLICY for the ENTIRE dGPU-ON:
//      baseline (0a, "efficient power save") / deep idle (07) / heavy (0e).
//      User activity detected through evdev (/dev/input/event*, separate thread
//      with poll(); scroll+keyboard+mouse). 0e EXCLUSIVELY busy-driven
//      (busy > busy-enter 80% + temp < temp-up) — title/video class
//      ([preferred-titles]/[video-classes]) only INFORMATIONAL (status/log).
//      07 = no input for activity-dwell-ms AND busy < deep-idle-busy.
//      [caps] floor = hard minimum (max(cap_floor, floor_dynamic)),
//      low-power ceiling (classes [low-power]), thermals per-profile (or shared
//      via temp-per-profile=false). title_pref (force-0e by title) DISAPPEARS.
//      Section [dgpu-active]; escape hatch enable=false → old logic
//      (profile/ladder) unchanged. Status: dgpu_state / input_active / video.
//
// v5.6 (2026-08-28):
//   S. SWITCHD TUNE (report 86): browsers REMOVED from [dgpu-hard] — Discord/YouTube
//      cards promote by window title ([preferred-titles], icontains)
//      instead of by class; the rest of the cards are neutral (iGPU). Title promotion
//      is NOT a latch: busy < title-idle-busy (default 33%, [switch]) for
//      dwell-out → demote to iGPU (power-off). Focus on a non-Discord/YT → demote
//      after 1 s (regardless of busy — downclock to 07 is done by [dgpu-active]).
//      Switch timers shortened: min-residence/dwell-out/cooldown/min-switch-gap
//      = 1000 ms. [dgpu-active] unchanged.
//
// v5.9 (2026-08-28):
//   Z. FIX MPV NEVER PROMOTED: the hard-promotion branch was gated `if (hard)`,
//      and the class [dgpu-idle] (mpv) does NOT set hard — since v5.7 idle_class never
//      entered the promotion branch (all v5.7-v5.9 gates acted on a class
//      that does not promote). Condition: `if (hard || idle_class)`.
//   Z2. FIX KEEPING mpv WITH A HOT CPU: the busy<5% escape (truly_idle) worked
//      despite a hot CPU → mpv demoted after pstate-settle-ms (busy≈0 — render
//      off the dGPU). Now the escape only with a cool CPU — a hot CPU holds the dGPU.
//   Z3. FIX FALSE BOOSTS: unreadable cmdline (process died mid-scan)
//      → skip, no boost — short-lived LaunchTests caused boosts every few seconds.
//   Z4. FIX UP-FLOOR WITHOUT TEMP GATE: floor [caps] (mpv 0a) after power-on no
//      longer requires temp_low_dwell — a hot chassis held the dGPU at ~65-71°C ≥
//      temp-up (65) and mpv hung on 07 after promotion (user decision: 0a optimal,
//      07 too slow for video). pstate-settle-ms 10000 → 2000 (kernel 0011-0013
//      verified — post-power-on writes are safe; 10 s at 07 chops video).
//   X. 3-POINT FAN CURVE: [fan] temp-mid/fan-mid + temp-mid-igd/fan-mid-igd —
//      two-segment interpolation (% of RPM range), steep to max after temp-mid.
//      temp-max 99→90: full RPM at 90°C. mid=0 → legacy linear.
//   Y. CPU-TEMP-PROMOTE: [switch] cpu-temp-promote=70 — title card (YT/
//      Discord): CPU ≥ threshold → re-promotion without a title change + demote blocked
//      (dGPU < temp-gate, busy ≥ 5%) — CPU offload at high temperature.
//      Guard: the busy<5% escape only after pstate-settle-ms since promotion (busy after
//      power-on is unreliable — no churn when CPU ≥ threshold). After power-on,
//      temp dGPU does not gate for pstate-settle-ms (stale read — a dGPU
//      from OFF has headroom by definition).
//
// v5.10 (2026-08-28):
//   AA. CHASSIS THERMALS [fan-case] (report 97, variant B full a+b+c):
//       measurement of the AVERAGE chassis temperature (applesmc by LABELS tempN_label
//       → SMC key, not by indices; keys from config, negative m°C values
//       skipped — unconnected -127000, idle TCTD -250) + case curve
//       (case-min → case-max linearly, 0..100% of the RPM range from [fan]) + SOAK/HOLD
//       (case > case-min for case-dwell-ms → SOAK for hold-ms with floor = curve
//       at the case entry; exit when the hold expires AND case < case-min−margin
//       for exit-dwell-ms) + RAMP (±ramp-rpm-s per 1 s; compiler boost and
//       fan-override OVERRIDE — they skip the ramp, safety). Section [fan-case]:
//       enable/keys/case-min/case-max/case-dwell-ms/hold-ms/exit-dwell-ms/
//       margin/hold-floor/ramp-rpm-s. Status: fan_case_avg (°C, -1 = no
//       valid reads) + fan_case_state (normal|soak) in /run/reclocked/status
//       and reclockctl. Keyboard = TC0E/TC0F proxy (frame under the keyboard) —
//       OUTSIDE set B (CPU curve duplicate, report 97 §2.3). enable=false
//       (default) → old 1:1 fan algorithm (escape hatch).
//
// v5.12 (2026-08-29, report 98 — case-min 20°C user decision + SOAK-exit fix):
//   Diagnosis: SOAK NEVER exited (case-min−margin = 27/37/17°C — below the
//   physical chassis minimum ~40°C while working; zero "NORMAL" logs in history)
//   + with case-min=40 the SOAK floor = curve(44°C) = 28% — too weak to cool
//   (case stands at 44.2°C, fans 3284 RPM). Changes: (1) case-min 40→20 —
//   the curve is strong in its working zone: (44−20)/(55−20) = 68% → fan1 ≈ 4877 RPM,
//   the case is actively cooled to the physical minimum; (2) SOAK entry via a
//   NEW key soak-enter (default 45°C — above the 40-44°C working zone, NOT
//   case-min: with case-min 20 "case > case-min" is always true); (3)
//   SOAK exit PURELY TIME-BASED — after hold-ms (default 300000 = 5 min, pulses)
//   unconditionally NORMAL (time decay; the only guaranteed exit);
//   re-entry when case again > soak-enter (duty cycle under continuous heat,
//   rest after cooling); (4) SOAK floor = max(entry curve, curve(current)) —
//   v5.11 escalation UNCHANGED (user decision);
//   (5) margin/exit-dwell-ms OBSOLETE — parsed for back-compat, unused.
//
// v5.13 (2026-08-29, reports 99/100):
//   Y. ACTIVITY-AWARE POWER-OFF GATE (D1): an open dGPU render fd does NOT by
//      itself block power-off anymore — chromium holds a passive probe fd
//      on renderD129 PERMANENTLY (empty fdinfo, busy=0, gpu-process on iGPU;
//      lesson of report 99: after EVERY YT promotion the power-off hung on
//      chromium's fd for the process lifetime, ~6 s retry loop, dGPU ON idly).
//      Defer ONLY when the render fd is open AND the dGPU shows activity:
//      busy > 1‰ over the busy-idle-dwell-ms window ([switch], default 3000).
//      Guard g_pmu_config_valid — after losing the busy-PMU counter config
//      (post-resume, v4.5 readback of R_IDLE_CTRL) busy reads 0 despite activity
//      → safely defer. dGPU audio (active PCM playback) still blocks
//      HARD (audio does not touch the GR/CE2 counters — busy could read 0
//      during playback). Dwell measured since the last busy>0 sample (reset at
//      power-on) — protects against the race: a client starts rendering right after OFF.

// v5.15 (2026-09-01, report 116 — crash boot -2):
//   BB. "dGPU DEAD" GATE: a failed power-on (set_on rejected OR recovery
//       after power-on failed — recover_after_power_on() == false) sets a
//       PERSISTENT dead state: file /run/reclocked/dgpu-dead (ts + reason, /run =
//       tmpfs → expires at reboot) + the dgpu_dead field in the status. From that
//       moment AUTOMATIC promotions (title-promo ~30 s — report 110,
//       cpu-temp-promote, soft/hard) NEVER request power-on: the policy
//       decides DGPU, but the gate forces the target back to IGPU (one-time
//       log + rejected counter every 60 s, field dgpu_dead_rejected
//       in the status). This closes what the v5.14 gates did not: rate-limit
//       and boot-guard only DELAYED the retry (after 120 s / 600 s the title
//       pulsation made another attempt on the dead card — the 2nd attempt in
//       boot -2 killed the daemon in a switcheroo write syscall → poisoned
//       vgasr_mutex). The gate expires ONLY through: (a) an explicit `reclockctl dgpu-on` (override
//       "on" clears the dead state and makes ONE attempt — its failure sets dead
//       again, blocking the retry-storm under a held override; the next attempt
//       = dgpu-auto + dgpu-on), (b) reboot (tmpfs). The file survives a daemon
//       restart (the resume hook restarts reclocked on post — the gate must
//       survive; loaded at init()).
//   CC. FATAL-BUT-CLEAN: a power-on failure does NOT kill the daemon — cleanup
//       goes through the existing recovery path (rollback_off → fans/status consistent),
//       the daemon lives on and the dead gate blocks further attempts. The main loop is
//       under try/catch — an unexpected exception → clean exit through restore()
//       (pstate exit + fans auto SMC) instead of abort without cleanup. Audit of
//       exit(): no exit() calls on runtime paths — early main() returns are
//       exclusively before pre-init (config/open_mmio; restart covered by
//       Restart=on-failure + StartLimit* from the unit).
//
// v5.8 (2026-08-28):
//   W. TESTS OUTSIDE THE COMPILER BOOST: test processes — cmake -P LaunchTest.cmake
//      (CTest/GoogleTest), ctest, "make test"/"make check" — do NOT count as a
//      compiler (fan boost only on actual compilation).
//
// v5.7 (2026-08-28):
//   T. CPU-TEMP-GATE (report 87): class [dgpu-idle] (mpv) does NOT demote when the CPU
//      is hot (doing something else — e.g. compiling) and the dGPU has thermal headroom
//      (cpu >= cpu-temp-gate [switch], default 70°C; dGPU < temp-gate). busy
//      < 5% (paused) → demote ALWAYS. Key [switch] cpu-temp-gate (0 = off).
//   U. RE-PROMOTION AFTER A TITLE/FOCUS CHANGE: after demoting a title class (YT/
//      Discord) and [dgpu-idle] (mpv), the dGPU returns ONLY when the window title
//      or the focus class changed (new video/channel), with hold title-idle-hold-ms —
//      a card with busy below the threshold stays on the iGPU for the whole lifetime
//      of that title (zero churn). For [dgpu-idle], an additional re-promotion when the CPU entered a
//      hot range (resuming video after pause).
//   V. [caps] mpv: floor=0a, max=0e, busy-up=50 — on the dGPU mpv does not declock
//      below 0a during playback.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <exception>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <getopt.h>
#include <glob.h>
#include <linux/input.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ------------------------------------------------------------------ version
// THE ONLY source of the daemon version: usage(), startup, the "version" field in the
// JSON status (reclockctl reads it into the switch-status header — zero hard labels).

#define RECLKD_VERSION "5.16"

// ------------------------------------------------------------------ paths

static const char* PCI_RESOURCE = "/sys/bus/pci/devices/0000:01:00.0/resource0";
static const char* PSTATE_FILE  = "/sys/kernel/debug/dri/0000:01:00.0/pstate";
static const char* POWER_CTRL   = "/sys/bus/pci/devices/0000:01:00.0/power/control";
static const char* HWMON_DIR    = "/sys/class/hwmon";
static const char* DRM_CARD     = "/dev/dri/card0"; // dGPU (nouveau) — eDP scanout
static const char* OVERRIDE_FILE = "/run/reclocked/override";
static const char* FAN_OVERRIDE_FILE = "/run/reclocked/fan-override";
static const char* DEFAULT_CONFIG = "/etc/reclocked.conf";

// v5.0: switchd — power/topology/status paths.
static const char* VGA_SWITCHEROO = "/sys/kernel/debug/vgaswitcheroo/switch";
static const char* RUNTIME_STATUS = "/sys/bus/pci/devices/0000:01:00.0/power/runtime_status";
static const char* AUTOSUSPEND_DELAY = "/sys/bus/pci/devices/0000:01:00.0/power/autosuspend_delay_ms";
static const char* IGPU_RPS_CUR = "/sys/class/drm/card1/gt/gt0/rps_cur_freq_mhz";
static const char* SWITCH_STATUS_FILE = "/run/reclocked/status";
static const char* SWITCH_DGPU_FILE = "/run/switchd/dgpu";
static const char* DGPU_OVERRIDE_FILE = "/run/reclocked/dgpu-override";
// v5.15: "dGPU dead" gate — content: "<unix_ts> <reason>". /run = tmpfs →
// expires at reboot (intended); survives daemon restart (resume hook).
static const char* DGPU_DEAD_FILE = "/run/reclocked/dgpu-dead";

// v5.0: BDF of the dGPU (GK107) and its audio (HDA DIS-A 0000:01:00.1). DRM and
// sound nodes resolved by BDF (DGPU_PCI / DGPU_AUDIO_PCI) — the cardN/
// renderDN/controlCN numbering changes between boots (on this machine it happens
// to be card0/renderD129/controlC2; controlC1 is PCH, NOT dGPU audio).
static const char* DGPU_PCI        = "0000:01:00.0";
static const char* DGPU_AUDIO_PCI  = "0000:01:00.1";

// v4.2: SMC applesmc — controlling the MacBook fans. fan1=Left,
// fan2=Right. fanN_manual (1=manual, 0=auto SMC) + fanN_output (target RPM).
// fanN_min/fanN_max read dynamically at startup (HW-dependent).
static const char* FAN_BASE = "/sys/devices/platform/applesmc.768";

// ------------------------------------- PMU registers (BAR0) per gk20a_devfreq.c

static constexpr uint32_t R_IDLE_CTRL      = 0x10A50C;
static constexpr uint32_t R_IDLE_MASK      = 0x10A504;
static constexpr uint32_t R_IDLE_COUNT     = 0x10A508;
static constexpr uint32_t R_IDLE_THRESHOLD = 0x10A8A0;
static constexpr uint32_t R_IDLE_INTR_EN   = 0x10A9E8;
static constexpr uint32_t R_IDLE_INTR_ST   = 0x10A9EC;

static constexpr uint32_t C_TOTAL = 0, C_BUSY = 4;

static constexpr uint32_t CTRL_VALUE_MASK   = 0x3;
static constexpr uint32_t CTRL_VALUE_BUSY   = 0x2;
static constexpr uint32_t CTRL_VALUE_ALWAYS = 0x3;
static constexpr uint32_t CTRL_FILTER_MASK  = 0x4;
static constexpr uint32_t MASK_GR  = 0x1;
static constexpr uint32_t MASK_CE2 = 0x200000;
static constexpr uint32_t COUNT_MASK  = 0x7FFFFFFF;
static constexpr uint32_t COUNT_RESET = 0x80000000;

// --------------------------------------------------------------- states / ladder

// 3 fixed levels of the safe ladder. Indices 0,1,2 = 07, 0a, 0e.
// 0f is NOT in the ladder (manual opt-in via the override flag — see header).
static const uint32_t LADDER[3] = { 0x07, 0x0a, 0x0e };
static const int LADDER_N = 3;

static int state_to_idx(uint32_t st)
{
    for (int i = 0; i < LADDER_N; i++) if (LADDER[i] == st) return i;
    return -1;
}
static bool known_state(int s)
{
    return s == 0x07 || s == 0x0a || s == 0x0e || s == 0x0f;
}
static const char* state_hex(int st)
{
    // Rotating buffers — state_hex called several times in one logf
    // (e.g. "cap=%s boost=%s"), so a single static buffer would cause
    // aliasing (a bug known from v3 state_name). 4 buffers = max 4 calls per logf.
    static char bufs[4][8];
    static int idx = 0;
    char* buf = bufs[idx & 3];
    idx++;
    std::snprintf(buf, 8, "%02x", (unsigned int)st & 0xff);
    return buf;
}

// ------------------------------------------------------------------ profile

struct Profile {
    int  max_pstate   = 0x07;  // ceiling (idx w drabince)
    int  boost_pstate = -1;    // -1 = brak boost; 0f = off-ladder boost tier (honorowane)
    int  busy_boost   = 85;    // % busy avg > → BOOST UP (0e→0f) for boost_dwell
    int  boost_dwell_ms = 5000;
    int  boost_hyst   = 10;    // pp rezerwa histerezy (obecnie boost-exit po busy_up)
    int  temp_down    = 65;    // °C TERMAL DOWN (per-profil; default 65, preferred 82)
    int  temp_up      = 58;    // °C: temp < temp_up → dozwolony UP (default 58, preferred 75)
};

struct Config {
    // List of "preferred" app classes (matched against the hyprctl class).
    std::set<std::string> preferred_classes;
    // v4.1: "preferred" window titles (case-insensitive substring; e.g. "YouTube",
    // "Discord"). Discord/YouTube are browser tabs — no window class of their own,
    // so title-based detection is an extra path alongside the class one.
    std::set<std::string> preferred_titles;
    // v4.1: "low-power" window classes (terminals). When the focused window matches →
    // force the default profile (cap=07) with priority over preferred (even if
    // Discord/YouTube generates load in the background, a focused terminal holds 07).
    std::set<std::string> low_power_classes;
    // v4.4: per-class [caps] policy — floor (resting state, IDLE does not go
    // below), max (ceiling), busy-up (own UP-LOAD threshold, %; 0 = global).
    // Syntax: "class = floor=0a, max=0e, busy-up=50" (each key optional).
    struct ClassCap {
        int floor   = -1;   // idx in the LADDER; -1 = no floor (IDLE to 07)
        int max     = -1;   // idx in the LADDER; -1 = profile ceiling
        int busy_up = 0;    // % busy for UP-LOAD; 0 = global busy-up (80)
    };
    std::map<std::string, ClassCap> class_caps;

    Profile def;       // default profile (non-preferred app)
    Profile preferred; // preferred profile (app from the list)

    // Common policy parameters (load hysteresis: up 80% / down 40%, 40 pp band):
    int  interval_ms    = 200;
    int  busy_up        = 80;
    int  busy_down      = 40;
    int  temp_dwell_ms  = 5000;
    int  idle_dwell_ms  = 5000;
    int  win_ms         = 1000;
    int  profile_dwell_ms = 2000; // rate-limit of profile changes
    int  poll_ms        = 1000;   // hyprctl polling period (interval multiple)
    int  exit_state     = 0x07;
    // v4.1: instantaneous busy threshold (‰) below which DOWN transitions are
    // allowed (GR-idle gate). In-flight memory reclocking under GR rendering
    // wedges the engine (documented: live 0a→07 → 631 PROP traps). vblank
    // protects scanout, NOT GR mid-render — this gate defers DOWN until
    // busy valley. UP-LOAD/BOOST-UP are NOT gated (a rising memory clock is
    // safer; UP requires busy>80% so a 30% gate would block UP).
    int  gr_idle_promille = 300;
    // v4.2: applesmc fan control. temp→RPM curve: linear interpolation
    // between fanN_min (temp_min) and fanN_max (temp_max), updated every poll_ms
    // (1 s). fanN_min/fanN_max read dynamically from sysfs at startup (not
    // hardcoded — every HW has a different range). Override flag-file stops auto.
    bool fan_enable     = true;
    int  fan_temp_min   = 51;     // °C → min RPM (quietest)
    int  fan_temp_max   = 91;     // °C → max RPM (loudest)
    // v5.1: separate curve when iGPU only (dGPU OFF) — quieter. When the dGPU is OFF the nouveau
    // hwmon disappears, temp = CPU (coretemp); the CPU may run hotter before the fans
    // hit max. Choice in the loop: sw.dgpu_off() → igd curve, otherwise standard.
    int  fan_temp_min_igd = 41;   // °C → min RPM (iGPU-only mode)
    int  fan_temp_max_igd = 91;   // °C → max RPM (iGPU-only mode)
    // v5.9: 3-point curve — inflection point temp-mid + % of the RPM range at
    // it. Two-segment interpolation: temp-min→temp-mid (0→fan-mid%), temp-mid
    // →temp-max (fan-mid%→100%, steep). mid=0 (or out of range) → legacy
    // linear temp-min→temp-max (as before). Separate values for igd.
    int  fan_temp_mid   = 0;      // °C inflection point (dga; 0 = disabled)
    int  fan_mid        = 0;      // % of the RPM range at temp-mid (dga; 0 = disabled)
    int  fan_temp_mid_igd = 0;    // °C inflection point (iGPU-only; 0 = disabled)
    int  fan_mid_igd    = 0;      // % of the RPM range at temp-mid (iGPU-only; 0 = off)
    // v5.10: [fan-case] section — chassis thermals (report 97, variant B full):
    // case curve (applesmc average) + SOAK/HOLD (holding raised RPM
    // for a time) + RAMP (limit of RPM changes per 1 s). enable=false (default) →
    // old 1:1 fan algorithm (escape hatch).
    // v5.12: case-min=20 (user decision — the physical ~40°C working minimum
    // lies INSIDE the curve range → the curve is strong in the working zone: 44°C→68%).
    // SOAK exit PURELY TIME-BASED (after hold-ms, unconditional) — the temperature
    // exit (case < case-min−margin) was physically unreachable
    // (17/27/37°C) → SOAK hung forever (zero exits in the 23:46-01:20 history).
    // SOAK entry via soak-enter (default 45°C — above the typical working
    // zone 40-44°C); floor = max(entry curve, curve(current)) (v5.11).
    struct FanCaseCfg {
        bool enable = false;
        std::vector<std::string> keys;  // SMC keys (label) for the average
        int case_min = 35;              // °C: case ≤ → 0% of the RPM range (forces nothing); 2026-08-29: 20→35 (user decision) — internal SMC sensors read above the chassis surface temperature
        int case_max = 60;              // °C: case ≥ → 100% of the RPM range
        int soak_enter = 45;            // °C: case > → SOAK (above the working zone)
        int dwell_in_ms = 5000;         // case > soak-enter for this long → SOAK
        int hold_ms = 300000;           // v5.12: SOAK DURATION (pulses) — after
                                        // it unconditional exit (time decay; 5 min
                                        // = ~chassis time constant)
        int exit_dwell_ms = 10000;      // v5.12: OBSOLETE — parsed for
        int margin = 3;                 // back-compat, unused (time-based exit)
        int ramp_rpm_s = 150;           // max RPM change per 1 s (0 = no ramp)
    } fan_case;
    // v4.6: [compiler] section — fan boost when a compiler is detected
    // (/proc/*/comm scan with a cmdline fallback). fan_max = % of maximum
    // RPM (100 = full fans). names = extra names to detect (comma-separated).
    bool compiler_enable   = true;
    int  compiler_fan_max  = 100;
    std::set<std::string> compiler_names;
    // v5.0: switchd module — dGPU power-state + render routing.
    struct SwitchCfg {
        bool enable = true;              // [switch] enable — module active (in DIS = monitor)
        int  tick_ms = 1000;             // decision tick (== poll_ms)
        int  dwell_in_ms = 3000;         // entry dwell (soft promotion)
        int  dwell_out_ms = 5000;        // exit dwell
        int  min_residence_ms = 20000;   // min-residence on the dGPU (anti-flapping)
        int  cooldown_ms = 45000;        // cooldown after demote
        int  wait_ready_ms = 2000;       // wait-for-ready after power-on
        int  min_switch_gap_ms = 10000;  // min power-toggle interval
        // v5.14 (report 114): AUTO power-on gates. Title pulsation (report 110,
        // 242/day) after S3 caused a power-on in the resume window → nouveau NULL deref
        // in nvkm_object_init (oops) → daemon dead with a poisoned vgasr_mutex →
        // fans frozen in manual, CPU 98°C. Rate-limit blocks title re-promotion
        // (title-idle-hold 30 s), boot-guard the resume window (hook restarts the
        // daemon on post → 120 s from start covers resume). Override (dgpu-on)
        // bypasses both gates — the user's decision.
        int  poweron_rate_limit_ms = 600000; // min interval of AUTO power-on (0 = off)
        int  poweron_boot_guard_ms = 120000; // no AUTO power-on for N ms from start (0 = off)
        // v5.15 (report 116): "dGPU dead" gate — after a failed power-on
        // (set_on rejected / recovery failed) automatic promotions NEVER
        // request power-on (file /run/reclocked/dgpu-dead + status). Expires:
        // reclockctl dgpu-on (explicit user decision — one attempt) or reboot.
        // false = disabled (escape hatch; NOT recommended — retry-storm from boot -2).
        bool poweron_dead_gate = true;
        int  temp_gate = 82;             // °C — don't promote when the dGPU is hotter
        int  busy_enter = 80;            // % busy enter (soft promotion)
        int  busy_exit = 40;             // % busy exit
        std::string backend = "manual";  // [dpower] manual | runpm
        int  autosuspend_ms = 5000;      // runpm
        int  wait_idle_timeout_ms = 5000; // /proc fd scan before OFF
        int  wait_ready_timeout_ms = 10000; // nouveau reinit after ON
        int  pstate_settle_ms = 10000;   // don't write pstate after power-on (clock settle)
        int  pstate_write_timeout_ms = 2000; // pstate write timeout (kernel hang safety)
        // v5.6: % busy below which a Discord/YouTube card (title promotion)
        // is "idle" → demote to iGPU (power-off) after dwell-out. Configurable
        // in [switch] (title-idle-busy), reload via SIGHUP.
        int  title_idle_busy = 33;
        // v5.6: after demoting a title class (Discord/YouTube) keep the dGPU OFF for
        // this long before re-promoting — prevents churn (YT playing on the iGPU
        // does not re-promote every 3-4 s). Configurable in [switch] (title-idle-hold-ms).
        int  title_idle_hold_ms = 30000;
        // v5.6: % busy below which a [dgpu-idle] class (e.g. mpv) is "idle"
        // → demote to iGPU (power-off) after dwell-out. Configurable in [switch]
        // (class-idle-busy), reload via SIGHUP.
        int  class_idle_busy = 33;
        // v5.7: CPU temp threshold (°C) for demoting the [dgpu-idle] class (mpv). CPU
        // hotter than the threshold (doing something else — e.g. compiling) + dGPU below
        // temp-gate (has thermal headroom) → mpv stays on the dGPU. busy < 5%
        // (paused) → always demote. 0 = disabled. Configurable in [switch]
        // (cpu-temp-gate), reload via SIGHUP.
        int  cpu_temp_gate = 70;
        // v5.9: CPU temp threshold (°C) for thermally offloading title cards
        // (YT/Discord). CPU ≥ threshold → re-promotion to the dGPU even without a
        // title/focus change + demote blocked (dGPU < temp-gate, busy ≥ 5%) —
        // the dGPU takes over rendering, the CPU cools. 0 = disabled. Configura-
        // ble in [switch] (cpu-temp-promote), reload via SIGHUP.
        int  cpu_temp_promote = 70;
        // v5.13 (D1, reports 99/100): activity-aware power-off gate. Time
        // (ms) the dGPU busy must be 0 with an open render fd before
        // power-off passes. Chromium holds a passive probe fd on renderD129
        // PERMANENTLY (busy=0, empty fdinfo) — the dwell distinguishes a passive holder
        // from an active render client (busy>0 → defer). Reset at
        // power-on (fresh cycle = fresh window). 0 = disabled (gate on the
        // fd alone — old behavior from report 98). Key [switch]
        // busy-idle-dwell-ms, reload via SIGHUP.
        int  busy_idle_dwell_ms = 3000;
        // v5.6: Discord/YouTube titles (copied from [preferred-titles] in the parser)
        // — title promotion of a browser card, NOT a latch (idle-release).
        std::set<std::string> preferred_titles;
        std::set<std::string> dgpu_hard;  // [dgpu-hard] classes — hard promotion
        std::set<std::string> dgpu_soft;  // [dgpu-soft] classes — soft (busy-gated)
        std::set<std::string> dgpu_idle;  // [dgpu-idle] classes — hard promotion + idle-demote
        std::set<std::string> igpu;       // [igpu] classes — always iGPU (demotion)
        std::set<std::string> dgpu_procs; // [dgpu-procs] processes — CUDA/DRI_PRIME
    } sw;
    // v5.4: [dgpu-active] section — three-stage pstate policy for the whole
    // dGPU-ON (report 79, user decisions 2026-08-28). enable=false (default) =
    // escape hatch: the daemon works as before (profiles/ladder). enable=true →
    // new logic. 0e EXCLUSIVELY busy-driven (busy > busy-enter) + thermal
    // margin — a video title does NOT give 0e (informational qualifier only).
    struct DgpuActiveCfg {
        bool enable = false;            // escape hatch — DISABLED by default
        int  baseline = 0x0a;           // resting state of a working dGPU
        int  max      = 0x0e;           // ceiling for sustained load
        int  low_power_ceiling = 0x0a;  // ceiling when focused ∈ [low-power]
        std::string activity_source = "evdev";  // evdev | none | cursorpos (→ none)
        int  activity_dwell_ms = 8000;  // no input for X → floor to 07
        int  deep_idle_busy = 20;       // % busy < (and no input) → deep idle 07
        int  busy_enter = 80;           // % busy > sustained → 0e (the only way to 0e)
        int  busy_exit  = 40;           // % busy ≤ → IDLE downshift (hysteresis)
        bool temp_per_profile = true;   // true = temp-down/up from the focus profile
        int  temp_down = 82;            // shared thermal (when temp-per-profile=false)
        int  temp_up   = 75;
        // [video-classes] — player classes (mpv/vlc) qualifying video
        // (informational — status/log; the title does NOT decide the pstate).
        std::set<std::string> video_classes;
    } dgpu;
    bool vblank_sync    = true;
    bool probe = false;
    bool dry   = false;
    int  verbosity = 1;

    // The preferred profile defaults differ from default — set in the constructor.
    // Thermal is PER-PROFILE: default 65/58 (non-preferred apps — cautious),
    // preferred 82/75 (browser — full performance, throttle only at 82).
    Config() {
        preferred.max_pstate   = 0x0e;
        preferred.boost_pstate = -1;   // no boost by default; config uncomments 0f
        preferred.temp_down    = 82;
        preferred.temp_up      = 75;
    }
};

static Config g_cfg;
static std::string g_config_path; // --config lub DEFAULT_CONFIG

// v5.4: state name of [dgpu-active] for status/logs. Mapping by ladder index
// relative to the config: max (0e) = "heavy", baseline (0a) = "active",
// 07 = "deep_idle"; idx<0 = "off". When baseline==max (degenerate) → "heavy".
static const char* dgpu_state_name(int idx, const Config::DgpuActiveCfg& d)
{
    if (idx < 0) return "off";
    if (idx == 0) return "deep_idle";
    if (idx >= state_to_idx(d.max)) return "heavy";
    return "active";
}

// v5.1: fan state for the status (/run/reclocked/status) — current curve + RPM.
// Filled in the fan block of the main loop, read by write_status() (pstate.sh,
// the Omarchy bar). off|override|compiler|igd|dga; tmin/tmax = active range.
static std::string g_fan_curve = "off";
static int  g_fan_tmin = 51, g_fan_tmax = 91;
// v5.9: inflection point of the 3-point curve (0 = legacy linear) — status.
static int  g_fan_tmid = 0, g_fan_pmid = 0;
static int  g_fan_rpm1 = 0, g_fan_rpm2 = 0;
// v5.10: [fan-case] — status (JSON /run/reclocked/status): chassis average (°C,
// -1 = no valid reads) + SOAK state. Filled in the fan block, read by
// write_status() (pstate.sh, the Omarchy bar).
static double g_fan_case_avg = -1.0;
static const char* g_fan_case_state = "normal";

// ------------------------------------------------------------------ helpers

static void logf(int level, const char* fmt, ...)
{
    if (level > g_cfg.verbosity) return;
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char ts[32];
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    std::printf("[%s.%03ld] ", ts, ms.count());
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
    std::fflush(stdout);
}

static volatile std::sig_atomic_t g_stop = 0;
static volatile std::sig_atomic_t g_reload = 0;
static void on_term(int) { g_stop = 1; }
static void on_hup(int)  { g_reload = 1; }

static int write_file(const char* path, const std::string& data)
{
    // v5.0: O_CREAT|O_TRUNC — status files (/run/reclocked/status, /run/switchd/dgpu)
    // do not exist on the first write; without O_CREAT open() returns ENOENT and the
    // write silently dies. For sysfs (fan1_manual, fan1_output, pstate etc.) this is a no-op —
    // files always exist, sysfs ignores O_TRUNC.
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, data.data(), data.size());
    close(fd);
    return n == (ssize_t)data.size() ? 0 : -1;
}

static int read_file(const char* path, std::string& out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n;
    out.clear();
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, n);
    close(fd);
    return n == 0 ? 0 : -1;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// v4.1: case-insensitive substring. Empty needle → false (don't treat an empty
// pattern as a match — prevents false pref signals on empty entries).
static bool icontains(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return false;
    if (needle.size() > hay.size()) return false;
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); j++) {
            if (to_lower((unsigned char)hay[i + j]) != to_lower((unsigned char)needle[j])) {
                match = false; break;
            }
        }
        if (match) return true;
    }
    return false;
}

// ------------------------------------------------------------------ config parser
// Format:
//   [preferred]
//   chromium
//   firefox
//
//   [profile default]
//   max-pstate = 07
//   temp-down = 67
//
//   [profile preferred]
//   max-pstate = 0e
//   boost-pstate = 0f
//   busy-boost = 80
//   boost-dwell-ms = 5000
//   temp-down = 80
//   temp-up = 70
//
// Non-Profile sections may also hold general keys (interval-ms, busy-up, ...).

static int parse_state(const std::string& v)
{
    return (int)std::strtol(v.c_str(), nullptr, 16);
}
static int parse_int(const std::string& v)
{
    return std::atoi(v.c_str());
}

static bool load_config(const std::string& path, Config& cfg)
{
    std::string content;
    if (read_file(path.c_str(), content) != 0) return false;

    std::string section;
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, (eol == std::string::npos
            ? content.size() - pos : eol - pos));
        pos = (eol == std::string::npos) ? content.size() : eol + 1;
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        if (section == "preferred") {
            cfg.preferred_classes.insert(t);
            continue;
        }
        if (section == "preferred-titles") {
            cfg.preferred_titles.insert(t);
            // v5.6: copy to SwitchCfg — switchd title promotion of Discord/YT.
            cfg.sw.preferred_titles.insert(t);
            continue;
        }
        if (section == "low-power") {
            cfg.low_power_classes.insert(t);
            continue;
        }
        // v4.4: "caps" section — per-class policy: "class = floor=0a, max=0e, busy-up=50".
        if (section == "caps") {
            size_t eqc = t.find('=');
            if (eqc == std::string::npos) continue;
            std::string kc = trim(t.substr(0, eqc));
            std::string vc = trim(t.substr(eqc + 1));
            Config::ClassCap cc;
            size_t p0 = 0;
            while (p0 <= vc.size()) {
                size_t csep = vc.find(',', p0);
                std::string tok = trim(vc.substr(p0, csep == std::string::npos
                    ? std::string::npos : csep - p0));
                if (!tok.empty()) {
                    size_t eq = tok.find('=');
                    std::string k, v;
                    if (eq == std::string::npos) { k = tok; v = ""; }
                    else { k = trim(tok.substr(0, eq)); v = trim(tok.substr(eq + 1)); }
                    if      (k == "floor")     cc.floor   = parse_state(v);
                    else if (k == "max")       cc.max     = parse_state(v);
                    else if (k == "busy-up")   cc.busy_up = parse_int(v);
                }
                if (csep == std::string::npos) break;
                p0 = csep + 1;
            }
            cfg.class_caps[kc] = cc;
            continue;
        }
        // v4.2: [fan] section — key=value keys (enable/temp-min/temp-max).
        // v5.1: temp-min-igd/temp-max-igd — curve when iGPU only (dGPU OFF).
        if (section == "fan") {
            size_t eqf = t.find('=');
            if (eqf == std::string::npos) continue;
            std::string keyf = trim(t.substr(0, eqf));
            std::string valf = trim(t.substr(eqf + 1));
            if      (keyf == "enable")      cfg.fan_enable     = (valf == "1" || valf == "true" || valf == "yes");
            else if (keyf == "temp-min")    cfg.fan_temp_min   = parse_int(valf);
            else if (keyf == "temp-max")    cfg.fan_temp_max   = parse_int(valf);
            else if (keyf == "temp-min-igd") cfg.fan_temp_min_igd = parse_int(valf);
            else if (keyf == "temp-max-igd") cfg.fan_temp_max_igd = parse_int(valf);
            // v5.9: 3-point curve — temp-mid (°C of inflection) + fan-mid
            // (% of the RPM range at temp-mid); separate pairs for igd.
            else if (keyf == "temp-mid")    cfg.fan_temp_mid     = parse_int(valf);
            else if (keyf == "fan-mid")     cfg.fan_mid          = parse_int(valf);
            else if (keyf == "temp-mid-igd") cfg.fan_temp_mid_igd = parse_int(valf);
            else if (keyf == "fan-mid-igd") cfg.fan_mid_igd      = parse_int(valf);
            continue;
        }
        // v5.10: [fan-case] section — chassis thermals (report 97, variant B).
        // Keys: enable, keys (SMC keys comma-separated), case-min, case-max,
        // soak-enter (v5.12 — SOAK entry threshold), case-dwell-ms (alias
        // dwell-in, in seconds), hold-ms, exit-dwell-ms (alias exit-dwell,
        // in seconds), margin, hold-floor (curve — the only supported one),
        // ramp-rpm-s. v5.12: exit-dwell-ms/margin parsed for back-compat,
        // UNUSED (time-based SOAK exit after hold-ms).
        if (section == "fan-case") {
            size_t eqh = t.find('=');
            if (eqh == std::string::npos) continue;
            std::string keyh = trim(t.substr(0, eqh));
            std::string valh = trim(t.substr(eqh + 1));
            if      (keyh == "enable")        cfg.fan_case.enable = (valh == "1" || valh == "true" || valh == "yes");
            else if (keyh == "keys") {
                cfg.fan_case.keys.clear();
                size_t p0 = 0;
                while (p0 <= valh.size()) {
                    size_t csep = valh.find(',', p0);
                    std::string tok = trim(valh.substr(p0, csep == std::string::npos
                        ? std::string::npos : csep - p0));
                    if (!tok.empty()) cfg.fan_case.keys.push_back(tok);
                    if (csep == std::string::npos) break;
                    p0 = csep + 1;
                }
            }
            else if (keyh == "case-min")      cfg.fan_case.case_min = parse_int(valh);
            else if (keyh == "case-max")      cfg.fan_case.case_max = parse_int(valh);
            else if (keyh == "soak-enter")    cfg.fan_case.soak_enter = parse_int(valh);
            else if (keyh == "case-dwell-ms") cfg.fan_case.dwell_in_ms = parse_int(valh);
            else if (keyh == "dwell-in")      cfg.fan_case.dwell_in_ms = parse_int(valh) * 1000; // seconds
            else if (keyh == "hold-ms")       cfg.fan_case.hold_ms = parse_int(valh);
            else if (keyh == "exit-dwell-ms") cfg.fan_case.exit_dwell_ms = parse_int(valh); // v5.12: unused (back-compat)
            else if (keyh == "exit-dwell")    cfg.fan_case.exit_dwell_ms = parse_int(valh) * 1000; // as above
            else if (keyh == "margin")        cfg.fan_case.margin = parse_int(valh);      // v5.12: unused (back-compat)
            else if (keyh == "hold-floor") {
                // curve — the only supported value (floor = curve at the case
                // entry into SOAK); other values ignored.
            }
            else if (keyh == "ramp-rpm-s")    cfg.fan_case.ramp_rpm_s = parse_int(valh);
            continue;
        }
        // v4.6: [compiler] section — fan boost when a compiler is detected.
        // Keys: enable (1/0), fan-max (% of maximum RPM), names (comma-separated).
        if (section == "compiler") {
            size_t eqg = t.find('=');
            if (eqg == std::string::npos) continue;
            std::string keyg = trim(t.substr(0, eqg));
            std::string valg = trim(t.substr(eqg + 1));
            if      (keyg == "enable")  cfg.compiler_enable  = (valg == "1" || valg == "true" || valg == "yes");
            else if (keyg == "fan-max") cfg.compiler_fan_max = parse_int(valg);
            else if (keyg == "names") {
                // Comma-separated name list: "ccache, sccache, ..." (optional).
                cfg.compiler_names.clear();
                size_t p0 = 0;
                while (p0 <= valg.size()) {
                    size_t csep = valg.find(',', p0);
                    std::string tok = trim(valg.substr(p0, csep == std::string::npos
                        ? std::string::npos : csep - p0));
                    if (!tok.empty()) cfg.compiler_names.insert(tok);
                    if (csep == std::string::npos) break;
                    p0 = csep + 1;
                }
            }
            continue;
        }
        // v5.0: [switch] section — switchd module (dGPU power-state + render routing).
        if (section == "switch") {
            size_t eqs = t.find('=');
            if (eqs == std::string::npos) continue;
            std::string keys = trim(t.substr(0, eqs));
            std::string vals = trim(t.substr(eqs + 1));
            if      (keys == "enable")            cfg.sw.enable            = (vals == "1" || vals == "true" || vals == "yes");
            else if (keys == "tick-ms")           cfg.sw.tick_ms           = parse_int(vals);
            else if (keys == "dwell-in-ms")       cfg.sw.dwell_in_ms       = parse_int(vals);
            else if (keys == "dwell-out-ms")      cfg.sw.dwell_out_ms      = parse_int(vals);
            else if (keys == "min-residence-ms")  cfg.sw.min_residence_ms  = parse_int(vals);
            else if (keys == "cooldown-ms")       cfg.sw.cooldown_ms       = parse_int(vals);
            else if (keys == "wait-ready-ms")     cfg.sw.wait_ready_ms     = parse_int(vals);
            else if (keys == "min-switch-gap-ms") cfg.sw.min_switch_gap_ms = parse_int(vals);
            // v5.14: AUTO power-on gates (report 114) — see the comment in SwitchCfg.
            else if (keys == "poweron-rate-limit-ms") cfg.sw.poweron_rate_limit_ms = parse_int(vals);
            else if (keys == "poweron-boot-guard-ms") cfg.sw.poweron_boot_guard_ms = parse_int(vals);
            // v5.15: "dGPU dead" gate after a failed power-on (see SwitchCfg).
            else if (keys == "poweron-dead-gate")     cfg.sw.poweron_dead_gate = (vals == "1" || vals == "true" || vals == "yes");
            else if (keys == "temp-gate")         cfg.sw.temp_gate         = parse_int(vals);
            else if (keys == "busy-enter")        cfg.sw.busy_enter        = parse_int(vals);
            else if (keys == "busy-exit")         cfg.sw.busy_exit         = parse_int(vals);
            else if (keys == "pstate-settle-ms")  cfg.sw.pstate_settle_ms  = parse_int(vals);
            else if (keys == "pstate-write-timeout-ms") cfg.sw.pstate_write_timeout_ms = parse_int(vals);
            else if (keys == "title-idle-busy")   cfg.sw.title_idle_busy   = parse_int(vals);
            else if (keys == "title-idle-hold-ms") cfg.sw.title_idle_hold_ms = parse_int(vals);
            else if (keys == "class-idle-busy")  cfg.sw.class_idle_busy   = parse_int(vals);
            // v5.7: cpu-temp-gate — CPU temp threshold (°C) for the [dgpu-idle] demote.
            else if (keys == "cpu-temp-gate")    cfg.sw.cpu_temp_gate     = parse_int(vals);
            // v5.9: cpu-temp-promote — CPU temp threshold (°C) for title cards
            // (YT/Discord): CPU ≥ threshold → re-promotion + demote block (0 = off).
            else if (keys == "cpu-temp-promote") cfg.sw.cpu_temp_promote  = parse_int(vals);
            // v5.13 (D1): busy-idle-dwell-ms — busy=0 dwell for the power-off gate
            // (a passive render fd does not block; active render busy>0 → defer).
            else if (keys == "busy-idle-dwell-ms") cfg.sw.busy_idle_dwell_ms = parse_int(vals);
            continue;
        }
        // v5.0: [dpower] section — dGPU power backend.
        if (section == "dpower") {
            size_t eqd = t.find('=');
            if (eqd == std::string::npos) continue;
            std::string keyd = trim(t.substr(0, eqd));
            std::string vald = trim(t.substr(eqd + 1));
            if      (keyd == "backend")               cfg.sw.backend               = vald;
            else if (keyd == "autosuspend-ms")        cfg.sw.autosuspend_ms        = parse_int(vald);
            else if (keyd == "wait-idle-timeout-ms")  cfg.sw.wait_idle_timeout_ms  = parse_int(vald);
            else if (keyd == "wait-ready-timeout-ms") cfg.sw.wait_ready_timeout_ms = parse_int(vald);
            continue;
        }
        // v5.0: switchd list sections — bare keys (classes/processes).
        if (section == "dgpu-hard")  { cfg.sw.dgpu_hard.insert(t);  continue; }
        if (section == "dgpu-soft")  { cfg.sw.dgpu_soft.insert(t);  continue; }
        if (section == "dgpu-idle")  { cfg.sw.dgpu_idle.insert(t);  continue; }
        if (section == "igpu")       { cfg.sw.igpu.insert(t);       continue; }
        if (section == "dgpu-procs") { cfg.sw.dgpu_procs.insert(t); continue; }
        // v5.4: [video-classes] — player classes qualifying video (mpv/vlc).
        if (section == "video-classes") { cfg.dgpu.video_classes.insert(t); continue; }
        // v5.4: [dgpu-active] section — three-stage pstate policy for dGPU-ON.
        // deep-idle-busy (new key, user decision 2026-08-28); activity-wake-busy
        // accepted as a backward alias. video-* keys from the working version of report 79
        // ARE GONE (0e is no longer video-driven) — unknown keys are ignored.
        if (section == "dgpu-active") {
            size_t eqa = t.find('=');
            if (eqa == std::string::npos) continue;
            std::string keya = trim(t.substr(0, eqa));
            std::string vala = trim(t.substr(eqa + 1));
            if      (keya == "enable")               cfg.dgpu.enable            = (vala == "1" || vala == "true" || vala == "yes");
            else if (keya == "baseline")             cfg.dgpu.baseline          = parse_state(vala);
            else if (keya == "max")                  cfg.dgpu.max               = parse_state(vala);
            else if (keya == "low-power-ceiling")    cfg.dgpu.low_power_ceiling = parse_state(vala);
            else if (keya == "activity-source")      cfg.dgpu.activity_source   = vala;
            else if (keya == "activity-dwell-ms")    cfg.dgpu.activity_dwell_ms = parse_int(vala);
            else if (keya == "deep-idle-busy")       cfg.dgpu.deep_idle_busy    = parse_int(vala);
            else if (keya == "activity-wake-busy")   cfg.dgpu.deep_idle_busy    = parse_int(vala); // alias
            else if (keya == "busy-enter")           cfg.dgpu.busy_enter        = parse_int(vala);
            else if (keya == "busy-exit")            cfg.dgpu.busy_exit         = parse_int(vala);
            else if (keya == "temp-per-profile")     cfg.dgpu.temp_per_profile  = (vala == "1" || vala == "true" || vala == "yes");
            else if (keya == "temp-down")            cfg.dgpu.temp_down         = parse_int(vala);
            else if (keya == "temp-up")              cfg.dgpu.temp_up           = parse_int(vala);
            continue;
        }
        // Sections [profile default] / [profile preferred] and possibly [global].
        size_t eq = t.find('=');
        std::string key, val;
        if (eq == std::string::npos) { key = t; val = ""; }
        else { key = trim(t.substr(0, eq)); val = trim(t.substr(eq + 1)); }

        Profile* prof = nullptr;
        if (section == "profile default")   prof = &cfg.def;
        else if (section == "profile preferred") prof = &cfg.preferred;
        else if (section == "global" || section.empty()) {
            // general keys — handled below
        } else {
            continue; // unknown section — ignore
        }

        if (prof) {
            if      (key == "max-pstate")     prof->max_pstate   = parse_state(val);
            else if (key == "boost-pstate")   prof->boost_pstate = parse_state(val);
            else if (key == "busy-boost")     prof->busy_boost   = parse_int(val);
            else if (key == "boost-dwell-ms") prof->boost_dwell_ms = parse_int(val);
            else if (key == "boost-hyst")     prof->boost_hyst   = parse_int(val);
            else if (key == "temp-down")      prof->temp_down    = parse_int(val);
            else if (key == "temp-up")        prof->temp_up      = parse_int(val);
            continue;
        }
        // global keys
        if      (key == "interval-ms")       cfg.interval_ms       = parse_int(val);
        else if (key == "busy-up")           cfg.busy_up           = parse_int(val);
        else if (key == "busy-down")         cfg.busy_down         = parse_int(val);
        else if (key == "temp-dwell-ms")     cfg.temp_dwell_ms     = parse_int(val);
        else if (key == "idle-dwell-ms")     cfg.idle_dwell_ms     = parse_int(val);
        else if (key == "win-ms")            cfg.win_ms            = parse_int(val);
        else if (key == "profile-dwell-ms")  cfg.profile_dwell_ms  = parse_int(val);
        else if (key == "poll-ms")           cfg.poll_ms           = parse_int(val);
        else if (key == "exit-state")        cfg.exit_state        = parse_state(val);
        else if (key == "vblank-sync")       cfg.vblank_sync       = (val == "1" || val == "true" || val == "yes");
        else if (key == "gr-idle-promille")  cfg.gr_idle_promille  = parse_int(val);
    }
    if (cfg.gr_idle_promille < 0) cfg.gr_idle_promille = 0;
    if (cfg.gr_idle_promille > 1000) cfg.gr_idle_promille = 1000;
    // v4.2: fan sanity — temp_max must be > temp_min (otherwise a degenerate curve).
    if (cfg.fan_temp_max <= cfg.fan_temp_min) {
        logf(0, "config: fan temp-max <= temp-min (%d <= %d) — correcting to 51/91",
             cfg.fan_temp_max, cfg.fan_temp_min);
        cfg.fan_temp_min = 51; cfg.fan_temp_max = 91;
    }
    // v5.1: sanity of the igd curve (iGPU-only) — analogously, correction to 41/91.
    if (cfg.fan_temp_max_igd <= cfg.fan_temp_min_igd) {
        logf(0, "config: fan temp-max-igd <= temp-min-igd (%d <= %d) — correcting to 41/91",
             cfg.fan_temp_max_igd, cfg.fan_temp_min_igd);
        cfg.fan_temp_min_igd = 41; cfg.fan_temp_max_igd = 91;
    }
    // v5.9: sanity of the 3-point curve. fan-mid clamped to [0,100] (100 = full
    // fans from temp-mid). temp-mid: ≤ 0 → disabled (0); outside (temp-min,
    // temp-max) → disabled (0) — legacy linear (two-segment requires an
    // inflection point strictly inside the range).
    if (cfg.fan_mid < 0) cfg.fan_mid = 0;
    if (cfg.fan_mid > 100) cfg.fan_mid = 100;
    if (cfg.fan_mid_igd < 0) cfg.fan_mid_igd = 0;
    if (cfg.fan_mid_igd > 100) cfg.fan_mid_igd = 100;
    if (cfg.fan_temp_mid <= 0 || cfg.fan_temp_mid <= cfg.fan_temp_min ||
        cfg.fan_temp_mid >= cfg.fan_temp_max) cfg.fan_temp_mid = 0;
    if (cfg.fan_temp_mid_igd <= 0 || cfg.fan_temp_mid_igd <= cfg.fan_temp_min_igd ||
        cfg.fan_temp_mid_igd >= cfg.fan_temp_max_igd) cfg.fan_temp_mid_igd = 0;
    // v5.10: [fan-case] sanity — chassis thermals. Wrong values → disable with
    // a log (the fan sanity pattern above — never silent 0).
    if (cfg.fan_case.enable && cfg.fan_case.keys.empty()) {
        logf(0, "config: [fan-case] enable without keys — disabling (unknown case)");
        cfg.fan_case.enable = false;
    }
    if (cfg.fan_case.case_max <= cfg.fan_case.case_min) {
        logf(0, "config: [fan-case] case-max <= case-min (%d <= %d) — disabling",
             cfg.fan_case.case_max, cfg.fan_case.case_min);
        cfg.fan_case.enable = false;
    }
    // v5.12: soak-enter must lie INSIDE the curve range (above case-min —
    // otherwise SOAK always enters; below case-max) — otherwise correct it.
    if (cfg.fan_case.soak_enter <= cfg.fan_case.case_min)
        cfg.fan_case.soak_enter = cfg.fan_case.case_min + 10;
    if (cfg.fan_case.soak_enter >= cfg.fan_case.case_max)
        cfg.fan_case.soak_enter = cfg.fan_case.case_max - 1;
    if (cfg.fan_case.margin < 1) cfg.fan_case.margin = 1;
    if (cfg.fan_case.dwell_in_ms < 0) cfg.fan_case.dwell_in_ms = 0;
    if (cfg.fan_case.exit_dwell_ms < 0) cfg.fan_case.exit_dwell_ms = 0;
    if (cfg.fan_case.hold_ms < 1000) {
        logf(0, "config: [fan-case] hold-ms < 1000 (%d) — correcting to 300000",
             cfg.fan_case.hold_ms);
        cfg.fan_case.hold_ms = 300000;
    }
    if (cfg.fan_case.ramp_rpm_s < 0) cfg.fan_case.ramp_rpm_s = 0;
    // v4.6: compiler sanity — fan-max clamped to [0,100] (100 = full fans).
    if (cfg.compiler_fan_max < 0) cfg.compiler_fan_max = 0;
    if (cfg.compiler_fan_max > 100) cfg.compiler_fan_max = 100;
    // v5.0: switchd sanity.
    if (cfg.sw.tick_ms <= 0) cfg.sw.tick_ms = 1000;
    if (cfg.sw.dwell_in_ms < 0) cfg.sw.dwell_in_ms = 0;
    if (cfg.sw.dwell_out_ms < 0) cfg.sw.dwell_out_ms = 0;
    if (cfg.sw.min_residence_ms < 0) cfg.sw.min_residence_ms = 0;
    if (cfg.sw.cooldown_ms < 0) cfg.sw.cooldown_ms = 0;
    if (cfg.sw.wait_ready_ms < 0) cfg.sw.wait_ready_ms = 0;
    if (cfg.sw.min_switch_gap_ms < 0) cfg.sw.min_switch_gap_ms = 0;
    if (cfg.sw.poweron_rate_limit_ms < 0) cfg.sw.poweron_rate_limit_ms = 0;   // v5.14
    if (cfg.sw.poweron_boot_guard_ms < 0) cfg.sw.poweron_boot_guard_ms = 0;   // v5.14
    if (cfg.sw.temp_gate < 0) cfg.sw.temp_gate = 0;
    if (cfg.sw.busy_enter < 0) cfg.sw.busy_enter = 0;
    if (cfg.sw.busy_enter > 100) cfg.sw.busy_enter = 100;
    if (cfg.sw.busy_exit < 0) cfg.sw.busy_exit = 0;
    if (cfg.sw.busy_exit > 100) cfg.sw.busy_exit = 100;
    if (cfg.sw.backend != "manual" && cfg.sw.backend != "runpm") cfg.sw.backend = "manual";
    if (cfg.sw.autosuspend_ms < 0) cfg.sw.autosuspend_ms = 0;
    if (cfg.sw.wait_idle_timeout_ms < 0) cfg.sw.wait_idle_timeout_ms = 0;
    if (cfg.sw.wait_ready_timeout_ms < 0) cfg.sw.wait_ready_timeout_ms = 0;
    if (cfg.sw.pstate_settle_ms < 0) cfg.sw.pstate_settle_ms = 0;
    if (cfg.sw.pstate_write_timeout_ms < 100) cfg.sw.pstate_write_timeout_ms = 100;
    // v5.6: title-idle-busy sanity — % busy, clamp [0,100]; a wrong value
    // (negative/absurd) → fallback to 33.
    if (cfg.sw.title_idle_busy < 0 || cfg.sw.title_idle_busy > 100) cfg.sw.title_idle_busy = 33;
    // v5.6: title-idle-hold-ms sanity — min 1000 ms (1 s), wrong value → 30000.
    if (cfg.sw.title_idle_hold_ms < 1000) cfg.sw.title_idle_hold_ms = 30000;
    // v5.6: class-idle-busy sanity — % busy, clamp [0,100]; wrong value → 33.
    if (cfg.sw.class_idle_busy < 0 || cfg.sw.class_idle_busy > 100) cfg.sw.class_idle_busy = 33;
    // v5.7: cpu-temp-gate sanity — °C clamp [0,100]; 0 = disabled; wrong → 70.
    if (cfg.sw.cpu_temp_gate < 0 || cfg.sw.cpu_temp_gate > 100) cfg.sw.cpu_temp_gate = 70;
    // v5.9: cpu-temp-promote sanity — °C clamp [0,100]; 0 = disabled; wrong → 70.
    if (cfg.sw.cpu_temp_promote < 0 || cfg.sw.cpu_temp_promote > 100) cfg.sw.cpu_temp_promote = 70;
    // v5.4: [dgpu-active] sanity. States must be known (07/0a/0e/0f) and
    // baseline <= max. Busy thresholds clamped to [0,100]. activity-source:
    // only "evdev" yields an input signal; "none"/"cursorpos" → none (idle after dwell).
    if (cfg.dgpu.activity_source != "evdev" && cfg.dgpu.activity_source != "none" &&
        cfg.dgpu.activity_source != "cursorpos")
        cfg.dgpu.activity_source = "evdev";
    if (cfg.dgpu.activity_dwell_ms < 0) cfg.dgpu.activity_dwell_ms = 0;
    if (cfg.dgpu.deep_idle_busy < 0) cfg.dgpu.deep_idle_busy = 0;
    if (cfg.dgpu.deep_idle_busy > 100) cfg.dgpu.deep_idle_busy = 100;
    if (cfg.dgpu.busy_enter < 0) cfg.dgpu.busy_enter = 0;
    if (cfg.dgpu.busy_enter > 100) cfg.dgpu.busy_enter = 100;
    if (cfg.dgpu.busy_exit < 0) cfg.dgpu.busy_exit = 0;
    if (cfg.dgpu.busy_exit > 100) cfg.dgpu.busy_exit = 100;
    // v5.4: busy-exit MUST be < gr-idle-promille (30%) — otherwise the descent from 0e
    // (busy ≤ exit) would be deferred forever by the GR-idle gate (report 79
    // risk 7). The default 40% is above the threshold — deliberate hysteresis; at
    // gr-idle-promille < 400 the IDLE descent from 0e is deferred under render.
    if (!known_state(cfg.dgpu.baseline)) cfg.dgpu.baseline = 0x0a;
    if (!known_state(cfg.dgpu.max)) cfg.dgpu.max = 0x0e;
    if (!known_state(cfg.dgpu.low_power_ceiling)) cfg.dgpu.low_power_ceiling = 0x0a;
    if (state_to_idx(cfg.dgpu.baseline) > state_to_idx(cfg.dgpu.max)) {
        logf(0, "config: [dgpu-active] baseline > max — correcting max=baseline");
        cfg.dgpu.max = cfg.dgpu.baseline;
    }
    if (cfg.dgpu.temp_down <= cfg.dgpu.temp_up) {
        logf(0, "config: [dgpu-active] temp-down <= temp-up (%d <= %d) — correcting temp-up=temp-down-1",
             cfg.dgpu.temp_down, cfg.dgpu.temp_up);
        cfg.dgpu.temp_up = cfg.dgpu.temp_down - 1;
        if (cfg.dgpu.temp_up < 0) cfg.dgpu.temp_up = 0;
    }
    return true;
}

// ---------------------------------------------------------------- HW access (GPU)

class Gpu {
public:
    Gpu() : map_(MAP_FAILED) {}
    ~Gpu() { if (map_ != MAP_FAILED) munmap(map_, MAP_SIZE); }

    bool open_mmio()
    {
        int fd = open(PCI_RESOURCE, O_RDWR | O_SYNC);
        if (fd < 0) { std::perror("open resource0"); return false; }
        map_ = mmap(nullptr, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, MAP_BASE);
        close(fd);
        if (map_ == MAP_FAILED) { std::perror("mmap BAR0"); return false; }
        return true;
    }

    // v5.0: munmap + remap BAR0 (in case the mapping went stale after a power-cycle).
    bool reopen_mmio()
    {
        if (map_ != MAP_FAILED) { munmap(map_, MAP_SIZE); map_ = MAP_FAILED; }
        return open_mmio();
    }

    uint32_t rd(uint32_t reg) const
    {
        return *reinterpret_cast<volatile uint32_t*>(
            static_cast<char*>(map_) + (reg - MAP_BASE));
    }
    void wr(uint32_t reg, uint32_t val)
    {
        *reinterpret_cast<volatile uint32_t*>(
            static_cast<char*>(map_) + (reg - MAP_BASE)) = val;
    }

    void init_counters()
    {
        wr(R_IDLE_INTR_EN, 0);
        wr(R_IDLE_THRESHOLD + C_TOTAL * 4, 0x7FFFFFFF);

        uint32_t v = rd(R_IDLE_CTRL + C_TOTAL * 16);
        v &= ~(CTRL_VALUE_MASK | CTRL_FILTER_MASK);
        v |= CTRL_VALUE_ALWAYS;
        wr(R_IDLE_CTRL + C_TOTAL * 16, v);

        wr(R_IDLE_MASK + C_BUSY * 16, MASK_GR | MASK_CE2);

        v = rd(R_IDLE_CTRL + C_BUSY * 16);
        v &= ~(CTRL_VALUE_MASK | CTRL_FILTER_MASK);
        v |= CTRL_VALUE_BUSY;
        wr(R_IDLE_CTRL + C_BUSY * 16, v);
    }

    uint32_t sample() // 0..1000 ‰
    {
        uint32_t busy  = rd(R_IDLE_COUNT + C_BUSY * 16) & COUNT_MASK;
        uint32_t total = rd(R_IDLE_COUNT + C_TOTAL * 16) & COUNT_MASK;
        uint32_t st    = rd(R_IDLE_INTR_ST) & 1;

        wr(R_IDLE_COUNT + C_BUSY * 16, COUNT_RESET);
        wr(R_IDLE_COUNT + C_TOTAL * 16, COUNT_RESET);

        if (st) { wr(R_IDLE_INTR_ST, 1); return 1000; }
        if (total == 0 || busy > total) return 1000;
        return static_cast<uint32_t>(
            static_cast<uint64_t>(busy) * 1000 / total);
    }

    void dump_raw()
    {
        std::printf("raw: busy=%08x total=%08x ctrl_busy=%08x ctrl_total=%08x mask_busy=%08x\n",
                    rd(R_IDLE_COUNT + C_BUSY * 16),
                    rd(R_IDLE_COUNT + C_TOTAL * 16),
                    rd(R_IDLE_CTRL + C_BUSY * 16),
                    rd(R_IDLE_CTRL + C_TOTAL * 16),
                    rd(R_IDLE_MASK + C_BUSY * 16));
    }

private:
    static constexpr uint32_t MAP_BASE = 0x10A000;
    static constexpr size_t   MAP_SIZE = 0x2000;
    void* map_;
};

// ----------------------------------------------------------- hwmon (temp)

class Hwmon {
public:
    bool init()
    {
        bool nv = scan_nouveau();
        // v5.0 G1: in IGD the dGPU is OFF → nouveau hwmon disappeared → read_temp()=-1.
        // The fan curve must also know the CPU (coretemp) — otherwise fans stay at
        // min with a hot processor. coretemp does NOT disappear on a power-cycle.
        scan_coretemp();
        return nv;
    }
    ~Hwmon() { if (fd_ >= 0) close(fd_); if (cpu_fd_ >= 0) close(cpu_fd_); }

    // v5.0: re-scan of the nouveau hwmon after a power-cycle (node disappeared at
    // power-off; the old fd would be -1 forever → read_temp() = -1 = min curve).
    void reinit()
    {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }
        path_.clear();
        // v5.0 G1: cpu_fd_ (coretemp) deliberately NOT touched — coretemp does not disappear
        // on a dGPU power-cycle, and re-open without close = fd leak. Only re-scan
        // nouveau as before.
        scan_nouveau();
    }

    // v5.14 (report 114): read with re-scan — after an S3 resume the hwmonX numbering
    // MAY CHANGE (coretemp re-registered on another hwmonN; the old fd
    // points at a zombie with an empty name= → read() returns 0 bytes = temp -1 =
    // min curve at 98°C CPU). On failure: re-scan by name= + retry once.
    // 5 s throttle — when hwmon genuinely does not exist (nouveau with the dGPU OFF),
    // don't scan every second.
    int read_temp() // °C, -1 when absent
    {
        int v = read_deg(fd_);
        if (v == -1) { rescan_nouveau(); v = read_deg(fd_); }
        return v;
    }

    // v5.0 G1: CPU temp (coretemp, "Package id 0") — °C, -1 when absent.
    // v5.14: re-scan like read_temp() — coretemp too may change hwmonN.
    int read_cpu_temp()
    {
        int v = read_deg(cpu_fd_);
        if (v == -1) { rescan_coretemp(); v = read_deg(cpu_fd_); }
        return v;
    }
    const std::string& path() const { return path_; }
    bool ok() const { return fd_ >= 0; }

private:
    // v5.14: read °C from fd; -1 when fd<0 or the read is empty/short (zombie hwmon).
    static int read_deg(int fd)
    {
        if (fd < 0) return -1;
        if (lseek(fd, 0, SEEK_SET) < 0) return -1;
        char buf[32];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) return -1;
        buf[n] = 0;
        return std::atoi(buf) / 1000;
    }
    // v5.14: re-scan with throttle (5 s) — see read_temp().
    void rescan_nouveau()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - last_nouveau_scan_ < std::chrono::seconds(5)) return;
        last_nouveau_scan_ = now;
        scan_nouveau();
    }
    void rescan_coretemp()
    {
        auto now = std::chrono::steady_clock::now();
        if (now - last_cpu_scan_ < std::chrono::seconds(5)) return;
        last_cpu_scan_ = now;
        scan_coretemp();
    }
    bool scan_nouveau()
    {
        if (fd_ >= 0) { close(fd_); fd_ = -1; }   // v5.14: don't leak the old fd
        path_.clear();
        for (int i = 0; i < 64; i++) {
            std::string p = std::string(HWMON_DIR) + "/hwmon" + std::to_string(i);
            std::string name;
            if (read_file((p + "/name").c_str(), name) != 0) continue;
            if (name.find("nouveau") == std::string::npos) continue;
            path_ = p + "/temp1_input";
            fd_ = open(path_.c_str(), O_RDONLY);
            if (fd_ < 0) { std::perror(("open " + path_).c_str()); return false; }
            return true;
        }
        return false;
    }
    void scan_coretemp()
    {
        if (cpu_fd_ >= 0) { close(cpu_fd_); cpu_fd_ = -1; }  // v5.14: don't leak
        for (int i = 0; i < 64; i++) {
            std::string p = std::string(HWMON_DIR) + "/hwmon" + std::to_string(i);
            std::string name;
            if (read_file((p + "/name").c_str(), name) != 0) continue;
            if (name.find("coretemp") == std::string::npos) continue;
            cpu_path_ = p + "/temp1_input";
            cpu_fd_ = open(cpu_path_.c_str(), O_RDONLY);
            if (cpu_fd_ < 0) {
                std::perror(("open " + cpu_path_).c_str());
                cpu_fd_ = -1;   // fallback inactive
            }
            return;
        }
    }

    std::string path_;
    int fd_ = -1;
    std::string cpu_path_;
    int cpu_fd_ = -1;
    // v5.14: re-scan throttle (see read_temp()).
    std::chrono::steady_clock::time_point last_nouveau_scan_{};
    std::chrono::steady_clock::time_point last_cpu_scan_{};
};

// ----------------------------------------------------------- fans (applesmc)
// v4.2: MacBook fan control via SMC applesmc. Two fans:
// fan1 (Left), fan2 (Right). RPM ranges (fanN_min/fanN_max) read dynamically
// from sysfs at startup — NOT hardcoded (each HW has a different range; the user wanted
// "ranges generated dynamically per 2ch values" = min/max from sysfs per fan).
//
// temp→RPM curve: linear interpolation. x = clamp((temp - tmin) / (tmax - tmin))
// in [0,1]; rpm = fanN_min + round(x * (fanN_max - fanN_min)). temp ≤ tmin → min
// RPM (quietest), temp ≥ tmax → max RPM (loudest). Updated every poll_ms (1 s)
// — aligned with the hyprctl poll cycle.
//
// Fail-safe: init() returns false when applesmc files are missing → fan disabled without
// error (the daemon works without fan control). restore_auto() at exit
// clears manual (fanN_manual=0) → SMC takes over auto-control (reclocked never
// leaves fans locked in manual after stopping).

class Fan {
public:
    bool init()
    {
        std::string s;
        std::string base = std::string(FAN_BASE) + "/";
        if (read_file((base + "fan1_min").c_str(), s) != 0) return false;
        fan1_min_ = std::atoi(s.c_str());
        if (read_file((base + "fan1_max").c_str(), s) != 0) return false;
        fan1_max_ = std::atoi(s.c_str());
        if (read_file((base + "fan2_min").c_str(), s) != 0) return false;
        fan2_min_ = std::atoi(s.c_str());
        if (read_file((base + "fan2_max").c_str(), s) != 0) return false;
        fan2_max_ = std::atoi(s.c_str());
        // Sanity: min ≤ max and non-zero; otherwise fail-safe (don't write manual=1).
        if (fan1_min_ <= 0 || fan1_max_ <= fan1_min_ ||
            fan2_min_ <= 0 || fan2_max_ <= fan2_min_) {
            logf(0, "fan: absurd sysfs ranges (f1=%d-%d f2=%d-%d) — disabling",
                 fan1_min_, fan1_max_, fan2_min_, fan2_max_);
            return false;
        }
        ok_ = true;
        return true;
    }

    // Set fans by temp. temp<0 (no hwmon) → skip (keep the previous state).
    // tmin>=tmax (bad config) → clamp to max RPM (safer cooling).
    // v5.9: two-segment curve: temp-min → temp-mid (fan-mid% of the range) → temp-max.
    // temp-mid ≤ 0 or outside (temp-min, temp-max) → legacy linear.
    // v5.10: pct computed by pct_from_temp(), written via set_pct() (RAMP active when
    // [fan-case] is enabled — escape hatch: enable=false → old 1:1 algorithm).
    void set(int ft, int tmin, int tmid, int tmax, int pmid)
    {
        if (!ok_) return;
        // v5.0: temp<0 (nouveau hwmon gone — dGPU OFF) → min curve (x=0).
        // Previously the fail-safe "don't touch" left fans at the last
        // value; after a dGPU power-off temp is permanently -1, so the curve must go
        // to min (SMC has thermal protection overriding manual).
        int delta = g_cfg.fan_case.enable ? g_cfg.fan_case.ramp_rpm_s : 0;
        set_pct(pct_from_temp(ft, tmin, tmid, tmax, pmid), delta);
        last_temp_ = ft;
    }

    // v5.10: % of the RPM range computed from temperature (3-point / legacy
    // linear curve) — extracted from set() so the fan block can compose
    // max(pct_cpu, pct_case, soak_floor) before it reaches the write. Returns pct ∈ [0,1].
    double pct_from_temp(int ft, int tmin, int tmid, int tmax, int pmid) const
    {
        if (ft < 0) ft = tmin;
        double pct;
        if (tmid > tmin && tmid < tmax && pmid > 0) {
            if (ft <= tmid)
                pct = (double)(ft - tmin) / (tmid - tmin) * (pmid / 100.0);
            else
                pct = (pmid / 100.0) + (double)(ft - tmid) / (tmax - tmid) * ((100 - pmid) / 100.0);
        } else {
            if (tmax <= tmin) pct = 1.0;         // bad range → max (cool)
            else if (ft <= tmin) pct = 0.0;
            else if (ft >= tmax) pct = 1.0;
            else pct = (double)(ft - tmin) / (tmax - tmin);
        }
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        return pct;
    }

    // v5.10: RPM write from a % of range + RAMP: the change from the previous write is
    // clamped to ±max_delta_rpm per tick (1 s). max_delta=0 → no ramp.
    // Compiler boost and fan-override take PRECEDENCE and skip the ramp (safety).
    void set_pct(double pct, int max_delta_rpm)
    {
        if (!ok_) return;
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        int rpm1 = fan1_min_ + (int)(pct * (fan1_max_ - fan1_min_) + 0.5);
        int rpm2 = fan2_min_ + (int)(pct * (fan2_max_ - fan2_min_) + 0.5);
        if (max_delta_rpm > 0) {
            if (last_rpm1_ > 0)
                rpm1 = std::clamp(rpm1, last_rpm1_ - max_delta_rpm, last_rpm1_ + max_delta_rpm);
            if (last_rpm2_ > 0)
                rpm2 = std::clamp(rpm2, last_rpm2_ - max_delta_rpm, last_rpm2_ + max_delta_rpm);
        }
        write_file((std::string(FAN_BASE) + "/fan1_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan2_manual").c_str(), "1");
        write_file((std::string(FAN_BASE) + "/fan1_output").c_str(), std::to_string(rpm1));
        write_file((std::string(FAN_BASE) + "/fan2_output").c_str(), std::to_string(rpm2));
        last_rpm1_ = rpm1; last_rpm2_ = rpm2;
    }

    // v4.6: BOOST — force pct% of the maximum RPM (100 = full fans). Instead of the
    // temp→RPM curve, a fixed % of the range: rpm = fanN_min + pct/100*(fanN_max
    // - fanN_min). Manual=1 + output write for both fans (like set()).
    // pct clamped to [0,100]. Used when a compiler is detected ([compiler] section).
    // v5.10: written via set_pct() with max_delta=0 — boost OVERRIDES, skips the ramp
    // (fans must reach fan-max% immediately — thermal safety).
    void set_boost(int pct)
    {
        if (!ok_) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        set_pct(pct / 100.0, 0);
        last_temp_ = -1;
    }

    // Fail-safe: give control back to SMC. Called at daemon exit (restore).
    void restore_auto()
    {
        if (!ok_) return;
        write_file((std::string(FAN_BASE) + "/fan1_manual").c_str(), "0");
        write_file((std::string(FAN_BASE) + "/fan2_manual").c_str(), "0");
        logf(1, "fan: restore auto (fanN_manual=0) — SMC takes over");
    }

    bool ok() const { return ok_; }
    int fan1_min() const { return fan1_min_; }
    int fan1_max() const { return fan1_max_; }
    int fan2_min() const { return fan2_min_; }
    int fan2_max() const { return fan2_max_; }
    int last_rpm1() const { return last_rpm1_; }
    int last_rpm2() const { return last_rpm2_; }
    int last_temp() const { return last_temp_; }

private:
    bool ok_ = false;
    int fan1_min_ = 0, fan1_max_ = 0;
    int fan2_min_ = 0, fan2_max_ = 0;
    int last_temp_ = -1, last_rpm1_ = 0, last_rpm2_ = 0;
};

// ----------------------------------------------------------- chassis thermals (case)
// v5.10: measurement of the AVERAGE chassis temperature — [fan-case] (report 97 §2/§4.2).
// Mapping by LABELS (tempN_label → SMC key), NOT by tempN indices
// (the SMC key order may differ between models/firmware). Every tick
// reads only the config keys ([fan-case] keys) via the label→path map.
// Values in m°C; negative ones skipped (unconnected -127000, idle dummy TCTD
// -250). No valid reads → -1 (case component disabled — CPU-only fallback).

class SmcCase {
public:
    void init() { rescan(); }

    // Rebuild the label→path map (cheap — at SIGHUP, in case the SMC added sensors).
    void rescan()
    {
        labels_.clear();
        for (int i = 1; i <= 40; i++) {
            std::string base = std::string(FAN_BASE) + "/temp" + std::to_string(i);
            std::string label;
            if (read_file((base + "_label").c_str(), label) != 0) continue;
            labels_[trim(label)] = base + "_input";
        }
    }

    // Arithmetic mean (°C) of the valid keys from `keys`; -1 when zero valid
    // reads. Missing key (typo/missing sensor) → skipped + warning
    // ONCE per key (missing_logged — the gate_logged_ L2193-2200 pattern).
    double read_avg(const std::vector<std::string>& keys,
                    std::set<std::string>& missing_logged) const
    {
        double sum = 0.0;
        int n = 0;
        for (const auto& k : keys) {
            auto it = labels_.find(k);
            if (it == labels_.end()) {
                if (missing_logged.insert(k).second)
                    logf(0, "fan-case: key \"%s\" not found in applesmc — skipped",
                         k.c_str());
                continue;
            }
            std::string s;
            if (read_file(it->second.c_str(), s) != 0) continue;
            long raw = std::atol(s.c_str());
            if (raw < 0) continue;   // unconnected / idle dummy
            sum += raw / 1000.0;
            n++;
        }
        return n > 0 ? sum / n : -1.0;
    }

    int label_count() const { return (int)labels_.size(); }

private:
    std::map<std::string, std::string> labels_;   // SMC label → tempN_input path
};

// v5.10: case curve — linear interpolation: case ≤ case-min → 0 (forces
// nothing — min RPM), case ≥ case-max → 1 (100% of the RPM range), linear between.
// Works on the same RPM range as [fan]: the pct→RPM mapping is done
// by Fan::set_pct via fanN_min/max (linear — equivalent to computing in RPM).
// case_avg < 0 (no valid reads) → 0 — component disabled.
static double fan_case_pct(double case_avg, int case_min, int case_max)
{
    if (case_avg < 0 || case_avg <= case_min) return 0.0;
    if (case_avg >= case_max) return 1.0;
    return (case_avg - case_min) / (double)(case_max - case_min);
}

static bool fan_override_active()
{
    struct stat st;
    return stat(FAN_OVERRIDE_FILE, &st) == 0;
}

// v5.16: the flag content = FAN FLOOR in % (50-100) — the curve may raise
// RPM above the floor (thermals/boost), never drops below (lower bound,
// NOT a target). Empty/invalid content = 0 → old "hold" (reclockctl fan-off
// creates an empty file — full compatibility).
static int fan_override_pct()
{
    std::string c;
    if (read_file(FAN_OVERRIDE_FILE, c) != 0) return 0;
    int pct = atoi(trim(c).c_str());
    if (pct < 50 || pct > 100) return 0;
    return pct;
}

// ----------------------------------------------------------- compilers (/proc)
// v4.6: detection of running compilers. Scan of /proc/<pid>/comm (process
// basename) with a cmdline fallback (argv[0] basename) — catches e.g. processes
// whose comm is not obvious. Case-sensitive matching; besides the base
// list also version prefixes (gcc-*, g++-*, clang-*) and the config extension
// ([compiler] names). Scan every poll_cycles (1 s), stop once found.

static const std::set<std::string>& default_compiler_names()
{
    static const std::set<std::string> names = {
        "clang", "clang++", "clang-14", "clang-15", "clang-16", "clang-17", "clang-18",
        "gcc", "g++", "cc", "c++", "cc1", "cc1plus", "cc1obj", "cc1objplus",
        "make", "cmake", "ninja", "ninja-build", "cargo", "rustc", "meson",
        "go", "javac", "ld", "ld.lld", "lld", "as", "sccache", "ccache",
    };
    return names;
}

// Returns the name when it matches the compiler list, otherwise "".
static std::string compiler_match(const std::string& name)
{
    if (name.empty()) return "";
    const std::set<std::string>& base = default_compiler_names();
    if (base.count(name)) return name;
    if (g_cfg.compiler_names.count(name)) return name;
    // Versioned binaries: gcc-12, g++-12, clang-19, ... (prefix, case-sensitive).
    if (name.rfind("gcc-", 0) == 0 || name.rfind("g++-", 0) == 0 ||
        name.rfind("clang-", 0) == 0) return name;
    return "";
}

// v5.8: does the process cmdline indicate test execution (rather than compilation)?
// cmdline is raw argv separated by '\0' (read_file without transformation).
static bool is_test_runner(const std::string& cmdline)
{
    if (cmdline.empty()) return false;
    // cmake -P .../GoogleTest/LaunchTest.cmake — CTest runs tests through cmake
    if (cmdline.find("LaunchTest.cmake") != std::string::npos) return true;
    if (cmdline.find("GoogleTest") != std::string::npos) return true;
    // ctest — the CTest test driver
    if (cmdline.find("ctest") != std::string::npos) return true;
    // "make test" / "make check" — Makefile test targets (argv[1]), not compilation
    std::string a0 = cmdline.substr(0, cmdline.find('\0'));
    size_t slash = a0.rfind('/');
    std::string base = slash == std::string::npos ? a0 : a0.substr(slash + 1);
    if (base == "make") {
        size_t p1 = cmdline.find('\0');
        if (p1 != std::string::npos) {
            std::string a1 = cmdline.substr(p1 + 1, cmdline.find('\0', p1 + 1) - (p1 + 1));
            if (a1 == "test" || a1 == "check") return true;
        }
    }
    return false;
}

// Scans /proc and returns the name of a detected compiler, or "" when none.
// Called only from the fan block (every poll_cycles = 1 s).
static std::string compiler_running()
{
    DIR* d = opendir("/proc");
    if (!d) return "";
    struct dirent* e;
    std::string found;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue; // PIDs only
        std::string bp = std::string("/proc/") + e->d_name;
        std::string comm;
        if (read_file((bp + "/comm").c_str(), comm) == 0) {
            comm = trim(comm);
            found = compiler_match(comm);
            if (!found.empty()) {
                // v5.8: the full cmdline decides — test processes (cmake -P
                // LaunchTest.cmake / ctest / "make test"/"make check") are not a
                // compiler; skip and keep scanning.
                // v5.9 fix: unreadable cmdline = the process died mid-scan
                // (short-lived LaunchTests die between comm and cmdline) —
                // skip, do NOT boost (false detections every few seconds; a live
                // compiler will be caught by the next scan in 1 s).
                std::string cmdline;
                if (read_file((bp + "/cmdline").c_str(), cmdline) != 0)
                    continue;
                if (is_test_runner(cmdline))
                    continue;
                break;
            }
        }
        // Fallback: cmdline (argv[0] basename). Skip kernel threads (comm "[...]" —
        // cmdline is empty anyway), saves opening files for ~100 threads.
        if (comm.empty() || comm[0] != '[') {
            std::string cmdline;
            if (read_file((bp + "/cmdline").c_str(), cmdline) == 0 && !cmdline.empty()) {
                size_t nul = cmdline.find('\0');
                std::string a0 = cmdline.substr(0, nul == std::string::npos
                    ? cmdline.size() : nul);
                size_t slash = a0.rfind('/');
                std::string base = slash == std::string::npos ? a0 : a0.substr(slash + 1);
                found = compiler_match(base);
                if (!found.empty()) {
                    // v5.8: as above — a test runner (cmake -P LaunchTest.cmake,
                    // ctest, "make test"/"make check") is excluded from the boost.
                    if (is_test_runner(cmdline)) continue;
                    break;
                }
            }
        }
    }
    closedir(d);
    return found;
}

// v5.0: pstate write with timeout. The kernel may hang in nvkm_pstate_calc
// (wait_event on a workqueue) after a power-cycle — the debugfs write blocks in D state
// forever. Write in a separate thread; main waits max pstate_write_timeout_ms.
// After a timeout: the thread stays (the kernel holds it), the daemon lives on and does NOT write
// pstate until the reset (g_pstate_stuck — cleared by recover_after_power_on
// after the next power-cycle).
static std::atomic<bool> g_pstate_done{false};
static std::atomic<int>  g_pstate_result{-1};
static std::atomic<bool> g_pstate_busy{false};
static std::atomic<bool> g_pstate_stuck{false};

static void pstate_write_worker(uint32_t st)
{
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02x", st);
    int r = write_file(PSTATE_FILE, buf);
    g_pstate_result = r;
    g_pstate_done = true;
}

static int set_pstate(uint32_t st)
{
    if (g_pstate_stuck) {
        logf(0, "pstate: skipping %02x — the previous write got stuck in the kernel "
                "(nvkm_pstate_calc). A dGPU power-cycle (reclockctl dgpu-off/on) "
                "will reset the state.", st);
        return -1;
    }
    // Serialization: wait for the previous write (if still running).
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(g_cfg.sw.pstate_write_timeout_ms);
    while (g_pstate_busy && !g_pstate_done &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_pstate_busy && !g_pstate_done) {
        g_pstate_stuck = true;
        logf(0, "pstate: ERROR — the previous write got stuck in the kernel (nvkm_pstate_calc). "
                "Thread abandoned, the daemon lives. Further pstate writes suspended.");
        return -1;
    }
    g_pstate_done = false;
    g_pstate_result = -1;
    g_pstate_busy = true;
    std::thread(pstate_write_worker, st).detach();
    deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(g_cfg.sw.pstate_write_timeout_ms);
    while (!g_pstate_done && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!g_pstate_done) {
        g_pstate_stuck = true;
        logf(0, "pstate: ERROR — write of %02x did not finish in %dms (kernel hang in "
                "nvkm_pstate_calc). Thread abandoned, the daemon lives. Further pstate writes "
                "suspended until a dGPU power-cycle.",
             st, g_cfg.sw.pstate_write_timeout_ms);
        return -1;
    }
    g_pstate_busy = false;
    return g_pstate_result;
}

// v4.1: GR-idle gate — is the instantaneous busy (‰ over the last interval) below
// the threshold? Used before DOWN transitions (in-flight memory reclocking under GR
// render wedges the engine). b comes from gpu.sample() (PMU BAR0, reset per sample).
static bool gr_idle_ok(uint32_t busy_promille)
{
    return (int)busy_promille <= g_cfg.gr_idle_promille;
}

static std::string current_state()
{
    std::string content;
    if (read_file(PSTATE_FILE, content) != 0) return "";
    size_t star = content.find('*');
    if (star == std::string::npos) return "boot";
    size_t sol = content.rfind('\n', star);
    size_t colon = content.find(':', sol + 1);
    return content.substr(sol + 1, colon - sol - 1);
}

static volatile int g_cur_idx = -1;
// v5.0: the last busy sample (‰) — shared between the main loop and the switchd tick.
static volatile uint32_t g_last_busy = 0;
// v5.13 (D1, reports 99/100): validity of the busy-PMU counter configuration (BAR0,
// readback of R_IDLE_CTRL every cycle — v4.5 self-heal). When the config is lost
// (post-resume) counters stop counting → busy reads 0 despite activity —
// the power-off gate (D1) safely defers with an unreliable counter.
// false only in cycles where readback ≠ CTRL_VALUE_ALWAYS (until the end of
// recovery); after recovery readback returns to ALWAYS → true.
static volatile bool g_pmu_config_valid = true;

// ----------------------------------------------------------- sliding window

struct Ring {
    std::vector<uint32_t> buf;
    size_t cap = 0, head = 0, count = 0;
    uint64_t sum = 0;
    void resize(size_t c) { cap = c ? c : 1; buf.assign(cap, 0); head = count = 0; sum = 0; }
    void clear() { head = count = 0; sum = 0; }
    void push(uint32_t v) {
        if (count < cap) { buf[head] = v; sum += v; head = (head + 1) % cap; count++; }
        else { sum -= buf[head]; sum += v; buf[head] = v; head = (head + 1) % cap; }
    }
    uint32_t avg() const { return count ? (uint32_t)(sum / count) : 0; }
};

// ----------------------------------------------------------- vblank sync

// The PANEL card number (connected eDP/LVDS connector) — the panel card must be
// tracked by the vblank sync. In DIS it is card0 (the dGPU drives the panel), after
// G1/IGD = card1 (iGPU). A hardcoded DRM_CARD (card0) in IGD is wrong: card0 is the
// dGPU without an active CRTC → the WAIT_VBLANK ioctl fails with EBUSY, and the
// daemon's own fd on card0 blocks fd_busy() (the daemon against itself —
// power-off deferred). Returns "" when the panel is not
// found (fallback to DRM_CARD).
static std::string panel_drm_card()
{
    DIR* d = opendir("/sys/class/drm");
    if (!d) return "";
    struct dirent* e;
    std::string card;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card", 0) != 0) continue;
        size_t dash = name.find('-');
        if (dash == std::string::npos || dash <= 4) continue;   // "card<N>-<connector>"
        bool digits = true;
        for (size_t i = 4; i < dash; i++)
            if (!std::isdigit((unsigned char)name[i])) { digits = false; break; }
        if (!digits) continue;
        std::string conn = name.substr(dash + 1);
        if (conn.rfind("eDP", 0) != 0 && conn.rfind("LVDS", 0) != 0) continue;
        std::string st;
        if (read_file(("/sys/class/drm/" + name + "/status").c_str(), st) != 0) continue;
        if (trim(st) == "connected") {
            card = name.substr(4, dash - 4);
            break;
        }
    }
    closedir(d);
    return card;
}

static int g_drm_fd = -1;
static bool drm_open()
{
    // vblank sync must track the PANEL card — in DIS = card0 (dGPU), after G1/IGD =
    // card1 (iGPU). A hardcoded DRM_CARD in IGD means an fd on the dGPU without CRTC (vblank
    // EBUSY) + the own fd blocks fd_busy(). panel_drm_card() resolves the card by
    // connected eDP/LVDS; fallback to DRM_CARD.
    std::string panel = panel_drm_card();
    std::string dev = panel.empty() ? std::string(DRM_CARD) : "/dev/dri/card" + panel;
    g_drm_fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
    if (g_drm_fd < 0) {
        logf(0, "drm: open %s failed (%s)", dev.c_str(), std::strerror(errno));
        return false;
    }
    logf(1, "drm: vblank sync on %s", dev.c_str());
    // Drop KMS master immediately after open(). The first opener of a given card remains
    // the default DRM master; without this, reclocked blocks Hyprland from taking over
    // the card via libseat/logind (EBUSY -> "Found no gpus" -> crash -> black
    // screen). Vblank (_DRM_VBLANK_RELATIVE) works without a master — proof:
    // reclocked coexisted with Hyprland while fbcon was the master. A DROP_MASTER
    // error (EINVAL/ENODEV) when we are not the master is expected and ignored.
    if (ioctl(g_drm_fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL && errno != ENODEV)
        logf(0, "drm: DROP_MASTER error (%s)", std::strerror(errno));
    return true;
}
static void drm_vblank_wait()
{
    if (g_drm_fd < 0) return;
    union drm_wait_vblank v{};
    v.request.type     = _DRM_VBLANK_RELATIVE;
    v.request.sequence = 1;
    if (ioctl(g_drm_fd, DRM_IOCTL_WAIT_VBLANK, &v) < 0)
        logf(0, "vblank: WAIT_VBLANK ioctl error (%s)", std::strerror(errno));
}

// ----------------------------------------------------------- hyprctl (JSON)

// Simple extractor of text fields from JSON (without a full parser; hyprctl gives
// well-formed JSON). Finds the first "key" occurrence and returns the string
// value after it.
//
// v4.1: proper handling of JSON escape sequences (\" \\ \n \t). Previously json_str
// looked for the closing `"` via find('"', k+1) — it truncated the string at the first
// `\"` (e.g. a YouTube title with a quote in the video name). Now a character-by-
// -character scan with unescape. The shared helper json_extract_string removes duplication.

// Scans the JSON string starting from the opening `"` at position `q`.
// Returns the position of the closing `"` (or npos when we hit the end) and fills `out`
// with the unescaped content. Handles \", \\, \n, \t (others: literally backslash+char).
static size_t json_extract_string(const std::string& json, size_t q, std::string& out)
{
    out.clear();
    size_t i = q + 1;
    while (i < json.size()) {
        char c = json[i];
        if (c == '"') return i;            // end of the string
        if (c == '\\' && i + 1 < json.size()) {
            char e = json[i + 1];
            switch (e) {
                case '"':  out.push_back('"');  i += 2; continue;
                case '\\': out.push_back('\\'); i += 2; continue;
                case 'n':  out.push_back('\n'); i += 2; continue;
                case 't':  out.push_back('\t'); i += 2; continue;
                default:   out.push_back('\\'); out.push_back(e); i += 2; continue;
            }
        }
        out.push_back(c);
        i++;
    }
    return std::string::npos;              // no closing `"` — return what was collected
}

static bool json_str(const std::string& json, const std::string& key, std::string& out)
{
    std::string pat = "\"" + key + "\"";
    size_t k = json.find(pat);
    if (k == std::string::npos) return false;
    k = json.find(':', k + pat.size());
    if (k == std::string::npos) return false;
    k = json.find('"', k + 1);
    if (k == std::string::npos) return false;
    size_t end = json_extract_string(json, k, out);
    return end != std::string::npos;
}

static void json_str_all(const std::string& json, const std::string& key,
                         std::vector<std::string>& out)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t k = json.find(':', pos + pat.size());
        if (k == std::string::npos) break;
        k = json.find('"', k + 1);
        if (k == std::string::npos) break;
        std::string val;
        size_t end = json_extract_string(json, k, val);
        if (end == std::string::npos) break;
        out.push_back(val);
        pos = end + 1;
    }
}

// Run hyprctl -j <cmd> and return stdout. Returns false when unavailable.
static bool hyprctl_json(const std::string& cmd, std::string& out)
{
    std::string full = std::string("hyprctl -j ") + cmd + " 2>/dev/null";
    FILE* p = popen(full.c_str(), "r");
    if (!p) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    int st = pclose(p);
    if (st != 0 || out.empty()) return false;
    return true;
}

// ----------------------------------------------------------- HyprCtl: detekcja

class HyprCtl {
public:
    // Find the Hyprland instance: scan /run/user/<uid>/hypr/<his>/hyprland.lock.
    // Set env XDG_RUNTIME_DIR + HYPRLAND_INSTANCE_SIGNATURE so hyprctl
    // (run as root via popen) connects to the user's socket.
    // The .socket.sock has mode srwxr-xr-x — root can connect.
    bool detect()
    {
        DIR* run = opendir("/run/user");
        if (!run) return false;
        struct dirent* e;
        while ((e = readdir(run))) {
            if (e->d_name[0] == '.') continue;
            std::string base = std::string("/run/user/") + e->d_name + "/hypr";
            DIR* hd = opendir(base.c_str());
            if (!hd) continue;
            struct dirent* he;
            while ((he = readdir(hd))) {
                if (he->d_name[0] == '.') continue;
                std::string lock = base + "/" + he->d_name + "/hyprland.lock";
                struct stat st;
                if (stat(lock.c_str(), &st) == 0) {
                    uid_ = std::atoi(e->d_name);
                    his_ = he->d_name;
                    closedir(hd);
                    closedir(run);
                    apply_env();
                    return true;
                }
            }
            closedir(hd);
        }
        closedir(run);
        return false;
    }

    void apply_env()
    {
        std::string xdg = "/run/user/" + std::to_string(uid_);
        setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1);
        setenv("HYPRLAND_INSTANCE_SIGNATURE", his_.c_str(), 1);
    }

    // Active window (focus). Returns false when absent (tty / hyprctl unavailable).
    // v4.1: also returns the title (for Discord/YouTube detection by title — they are
    // browser tabs, with no window class of their own).
    bool activewindow(std::string& cls, std::string& title)
    {
        std::string json;
        if (!hyprctl_json("activewindow", json)) return false;
        if (json.find("\"class\"") == std::string::npos) return false; // empty
        bool ok = json_str(json, "class", cls);
        json_str(json, "title", title); // optional — no title does not break class
        return ok;
    }

    // All running clients (classes). Returns false when unavailable.
    bool clients(std::vector<std::string>& classes)
    {
        std::string json;
        if (!hyprctl_json("clients", json)) return false;
        json_str_all(json, "class", classes);
        return true;
    }

    int uid() const { return uid_; }
    const std::string& his() const { return his_; }

private:
    int uid_ = -1;
    std::string his_;
};

// ----------------------------------------------------------- override flag-file

static bool override_active()
{
    struct stat st;
    return stat(OVERRIDE_FILE, &st) == 0;
}

static std::string override_content()
{
    std::string c;
    read_file(OVERRIDE_FILE, c);
    return trim(c);
}

// ----------------------------------------------------------- switchd: topology
// v5.0: switchd module — dGPU power-state + render routing (Stage 1: monitor in DIS).
// Topology: vgaswitcheroo (root) — "2:DIS:+:Pwr:0000:01:00.0" → DIS (the dGPU
// drives the panel), "0:IGD:+" → IGD. Fallback: /sys/class/drm/card0-*/status
// (eDP-1 connected on card0 = DIS).

enum Topo { DIS, IGD, UNKNOWN };

class Topology {
public:
    Topo detect()
    {
        std::string content;
        if (read_file(VGA_SWITCHEROO, content) == 0) {
            if (content.find(":DIS:+") != std::string::npos) { topo_ = DIS; return topo_; }
            if (content.find(":IGD:+") != std::string::npos) { topo_ = IGD; return topo_; }
        }
        // Fallback: any connector of card0 connected (eDP-1 = panel) → DIS.
        if (card0_has_connected()) { topo_ = DIS; return topo_; }
        topo_ = UNKNOWN;
        return topo_;
    }
    Topo topo() const { return topo_; }
    const char* name() const
    {
        switch (topo_) {
            case DIS: return "dis";
            case IGD: return "igd";
            default:  return "unknown";
        }
    }
private:
    static bool card0_has_connected()
    {
        DIR* d = opendir("/sys/class/drm");
        if (!d) return false;
        struct dirent* e;
        bool found = false;
        while ((e = readdir(d))) {
            std::string name = e->d_name;
            if (name.rfind("card0-", 0) != 0) continue;
            std::string status;
            if (read_file(("/sys/class/drm/" + name + "/status").c_str(), status) == 0) {
                if (trim(status) == "connected") { found = true; break; }
            }
        }
        closedir(d);
        return found;
    }
    Topo topo_ = UNKNOWN;
};

// ----------------------------------------------------------- switchd: dGPU nodes (BDF)
// dGPU DRM and audio nodes resolved by PCI BDF — NOT by cardN/
// renderDN/controlCN numbering (the numbering changes between boots). G1 live context:
// in IGD the compositor (Hyprland) always holds an fd on the iGPU (card1 + renderD128) —
// a "any /dev/dri/*" gate would be forever busy and power-off would never pass
// (lesson from the G1 live test: apply() → "dGPU busy (open /dev/dri fd) —
// power-off deferred"). Hence fd_busy() blocks ONLY the dGPU nodes.
// LESSON 2026-08-29 (stuck power-off after rmmod/modprobe): the dGPU card<N> can also be
// opened PASSIVELY by seat/modeset managers (systemd, systemd-logind,
// Hyprland — they never close). In IGD nobody modesets on the dGPU → a card fd
// on the dGPU = ALWAYS passive, it must not block power-off. Only the dGPU
// renderD<N> nodes block (actual rendering) + active PCM playback (audio).
// LESSON 2026-08-29 v2 (D1, reports 99/100): even renderD<N> can be PASSIVE —
// chromium holds a probe fd on renderD129 PERMANENTLY (empty fdinfo, busy=0).
// Gate v2 (gate_check): a render fd blocks ONLY with dGPU activity (busy > 1%
// over the busy-idle-dwell-ms window); a passive fd (busy=0 over the dwell) does not block.
// dGPU audio (active PCM playback) still blocks HARD.

// readlink + basename of the target (e.g. /sys/class/drm/card0/device → "0000:01:00.0").
// No libgen.h — our own basename, without mutating the buffer.
static std::string readlink_basename(const std::string& path)
{
    char link[256];
    ssize_t n = readlink(path.c_str(), link, sizeof link - 1);
    if (n <= 0) return "";
    link[n] = 0;
    std::string target(link);
    size_t slash = target.find_last_of('/');
    return slash == std::string::npos ? target : target.substr(slash + 1);
}

// dGPU RENDER nodes: /dev/dri/renderD<N> for PCI == DGPU_PCI. Deliberately WITHOUT
// card<N> — systemd/logind/Hyprland open the dGPU card PASSIVELY (seat/modeset
// managers) and never close; in IGD nobody modesets on the dGPU, so
// a card fd on the dGPU = always passive (lesson 2026-08-29: after rmmod/modprobe card0
// became the dGPU and those fds blocked power-off FOREVER). renderD<N> is the only
// ACTIVE dGPU DRM node (render-offload) — only it may block power-off.
static std::set<std::string> dgpu_render_nodes()
{
    std::set<std::string> nodes;
    DIR* d = opendir("/sys/class/drm");
    if (!d) return nodes;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        // Only renderD<N> nodes — card<N> deliberately skipped (passive fds of
        // seat/modeset managers); connectors (cardN-DP-1) anyway have
        // device → cardN, they don't match DGPU_PCI.
        auto is_digits = [&](size_t off) {
            if (name.size() <= off) return false;
            for (size_t i = off; i < name.size(); i++)
                if (!std::isdigit((unsigned char)name[i])) return false;
            return true;
        };
        bool is_rend = name.rfind("renderD", 0) == 0 && is_digits(7);
        if (!is_rend) continue;
        if (readlink_basename("/sys/class/drm/" + name + "/device") == DGPU_PCI)
            nodes.insert("/dev/dri/" + name);
    }
    closedir(d);
    return nodes;
}

// Audio card number of the dGPU (e.g. "2") — the card on PCI == DGPU_AUDIO_PCI.
// (On this machine: card2/controlC2 = HDA NVidia 0000:01:00.1; controlC1 is
// PCH 0000:00:1b.0 — NOT the dGPU. The v5.0 matcher on controlC1 was wrong.)
// LESSON G1 (live): wireplumber (the PipeWire session manager) holds controlC<N>
// PERMANENTLY — it opens every card's control and never closes. So a gate
// on controlC would block power-off forever despite a free dGPU. The audio gate must
// concern ONLY ACTIVE PCM PLAYBACK (fd on /dev/snd/pcmC<N>D...p), never
// control. Returns "" when the card is not found.
static std::string dgpu_snd_playback_card()
{
    DIR* d = opendir("/sys/class/sound");
    if (!d) return "";
    struct dirent* e;
    std::string card;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card", 0) != 0 || name.size() <= 4) continue;
        bool digits = true;
        for (size_t i = 4; i < name.size(); i++)
            if (!std::isdigit((unsigned char)name[i])) { digits = false; break; }
        if (!digits) continue;
        // /sys/class/sound/card<N>/device → PCI. (controlC<N>/device leads
        // to card<N>, so we scan cards, not controls.)
        if (readlink_basename("/sys/class/sound/" + name + "/device") == DGPU_AUDIO_PCI) {
            card = name.substr(4);
            break;
        }
    }
    closedir(d);
    return card;
}

// v5.0: dgpu-override flag-file (/run/reclocked/dgpu-override). reclockctl
// dgpu-on/dgpu-off/dgpu-auto. Pattern of fan-override (G): "" = no file (auto),
// "on" = dGPU forced ON, "off" = dGPU forced OFF.
static std::string dgpu_override()
{
    struct stat st;
    if (stat(DGPU_OVERRIDE_FILE, &st) != 0) return "";
    std::string c;
    read_file(DGPU_OVERRIDE_FILE, c);
    return trim(c);
}

// ----------------------------------------------------------- switchd: dGPU power

class DgpuPower {
public:
    struct State {
        std::string vgasw;    // "Pwr" | "Off" | "Dyn" | "DynOff" | ""
        std::string runtime;  // "active" | "suspended" | "unsupported" | ""
        // vgaswitcheroo is AUTHORITATIVE: after gmux cut-off (DISCRETE_POWER=OFF,
        // PCI D3hot) nouveau reports a stale runtime_status="active" even though the card
        // is dead — runtime alone would lie (report 63: busy=1000‰ on a dead
        // card, pstate boost). "Off"/"DynOff" = OFF. runtime only as a fallback
        // (runpm backend without vgaswitcheroo).
        bool on() const {
            if (!vgasw.empty()) return vgasw == "Pwr" || vgasw == "Dyn";
            return runtime == "active";
        }
    };

    DgpuPower(const std::string& backend, int autosuspend_ms)
        : backend_(backend), autosuspend_ms_(autosuspend_ms),
          last_busy_nonzero_(std::chrono::steady_clock::now()) {}

    void set_recover_cb(std::function<bool()> cb) { recover_cb_ = std::move(cb); }

    State read() const
    {
        State st;
        std::string content;
        if (read_file(VGA_SWITCHEROO, content) == 0) {
            size_t pos = 0;
            while (pos < content.size()) {
                size_t eol = content.find('\n', pos);
                std::string line = content.substr(pos, eol == std::string::npos
                    ? std::string::npos : eol - pos);
                if (line.find("0000:01:00.0") != std::string::npos) {
                    // Format: "2:DIS:+:Pwr:0000:01:00.0" — pole 3 = power state.
                    size_t p0 = 0;
                    int field = 0;
                    while (p0 <= line.size()) {
                        size_t colon = line.find(':', p0);
                        std::string tok = line.substr(p0, colon == std::string::npos
                            ? std::string::npos : colon - p0);
                        if (field == 3) { st.vgasw = tok; break; }
                        field++;
                        if (colon == std::string::npos) break;
                        p0 = colon + 1;
                    }
                    break;
                }
                if (eol == std::string::npos) break;
                pos = eol + 1;
            }
        }
        std::string rs;
        if (read_file(RUNTIME_STATUS, rs) == 0) st.runtime = trim(rs);
        return st;
    }

    bool set_on()
    {
        // v5.13 (D1): a fresh power-cycle = a fresh idle window for the power-off gate
        // (busy=0 after power-on does not "inherit" the old timer — otherwise the
        // busy-idle-dwell-ms dwell would pass OFF immediately after power-on, before
        // a client manages to start rendering; lesson of report 99: from power-on to
        // the first OFF attempt is less than the dwell).
        last_busy_nonzero_ = std::chrono::steady_clock::now();
        if (backend_ == "runpm") {
            write_file(AUTOSUSPEND_DELAY, std::to_string(autosuspend_ms_));
            return write_file(POWER_CTRL, "on") == 0;
        }
        return write_file(VGA_SWITCHEROO, "ON") == 0;
    }

    bool set_off()
    {
        if (backend_ == "runpm") {
            write_file(AUTOSUSPEND_DELAY, std::to_string(autosuspend_ms_));
            return write_file(POWER_CTRL, "auto") == 0;
        }
        return write_file(VGA_SWITCHEROO, "OFF") == 0;
    }

    // wait_ready after power-on → full recover_after_power_on() (the callback is set
    // by Switchd — recovery requires hw/gpu/reset_cb, outside the DgpuPower scope).
    bool wait_ready()
    {
        if (!recover_cb_) {
            logf(0, "switch: wait_ready without a recovery callback");
            return false;
        }
        return recover_cb_();
    }

    // Scan /proc/*/fd for open fds on the dGPU renderD<N> (resolved by BDF).
    // Returns the node path ("" when nobody holds it). IMPORTANT: ONLY the dGPU
    // renderD<N>, NEVER the dGPU card<N> — systemd/logind/Hyprland hold the card PASSIVELY
    // and never close; in IGD nobody modesets on the dGPU (lesson
    // 2026-08-29: after rmmod/modprobe card0=dGPU → "dGPU busy" FOREVER).
    // Similarly the iGPU: Hyprland holds card1 + renderD128 of the iGPU — the
    // "/dev/dri/*" matcher from the original v5.0 = forever busy (G1 live lesson).
    // Scan pattern of the compilers (compiler_running) — no lsof.
    std::string open_render_fd()
    {
        std::set<std::string> nodes = dgpu_render_nodes();
        DIR* d = opendir("/proc");
        if (!d) return "";
        struct dirent* e;
        std::string hit;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            // The daemon must not block itself (its own vblank fd on card<N>).
            if (std::strtol(e->d_name, nullptr, 10) == (long)getpid()) continue;
            std::string fddir = std::string("/proc/") + e->d_name + "/fd";
            DIR* fd = opendir(fddir.c_str());
            if (!fd) continue;
            struct dirent* fe;
            while ((fe = readdir(fd))) {
                if (fe->d_name[0] == '.') continue;
                char link[256];
                std::string lp = fddir + "/" + fe->d_name;
                ssize_t n = readlink(lp.c_str(), link, sizeof link - 1);
                if (n <= 0) continue;
                link[n] = 0;
                if (nodes.count(std::string(link))) { hit = link; break; }
            }
            closedir(fd);
            if (!hit.empty()) break;
        }
        closedir(d);
        return hit;
    }

    // dGPU audio: is there ACTIVE PCM playback (fd on /dev/snd/pcmC<N>D*p)? NOT
    // controlC<N> — wireplumber (the sound manager) holds the control permanently,
    // the gate would never release (G1 live lesson). Capture (the "c" suffix) does NOT
    // block. Returns true when active playback is found.
    bool audio_playback_active()
    {
        std::string snd_card = dgpu_snd_playback_card();
        if (snd_card.empty()) return false;
        std::string pcm_prefix = "/dev/snd/pcmC" + snd_card;
        DIR* d = opendir("/proc");
        if (!d) return false;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            if (std::strtol(e->d_name, nullptr, 10) == (long)getpid()) continue;
            std::string fddir = std::string("/proc/") + e->d_name + "/fd";
            DIR* fd = opendir(fddir.c_str());
            if (!fd) continue;
            struct dirent* fe;
            while ((fe = readdir(fd))) {
                if (fe->d_name[0] == '.') continue;
                char link[256];
                std::string lp = fddir + "/" + fe->d_name;
                ssize_t n = readlink(lp.c_str(), link, sizeof link - 1);
                if (n <= 0) continue;
                link[n] = 0;
                std::string target(link);
                // Only active PCM playback (pcmC<N>D*p) — the "p" suffix.
                if (target.rfind(pcm_prefix, 0) == 0 && target.back() == 'p') {
                    closedir(fd);
                    closedir(d);
                    return true;
                }
            }
            closedir(fd);
        }
        closedir(d);
        return false;
    }

    // power-off gate v2 (D1, reports 99/100): the render fd of the dGPU alone does NOT block —
    // chromium holds a passive probe fd on renderD129 PERMANENTLY (empty fdinfo,
    // busy=0, gpu-process on the iGPU; lesson of 2026-08-29). Defer ONLY when the dGPU
    // shows activity: busy > BUSY_ACTIVE_PROMILLE over the busy-idle-dwell-ms window
    // (measured from the last busy>0 sample, reset at power-on — set_on()).
    // Guard g_pmu_config_valid: after losing the counter configuration (post-resume,
    // v4.5 self-heal) busy reads 0 despite activity → safely defer.
    // dGPU audio (active PCM playback) still blocks HARD — audio does not touch
    // the GR/CE2 counters, busy could read 0 during playback.
    // Returns GateCheck{block, passive, why}: passive=true = transient dwell
    // (passive fd, busy=0) — informational, no spam; passive=false = a real
    // blocker (active render / unreliable counter / dGPU PCM).
    static constexpr uint32_t BUSY_ACTIVE_PROMILLE = 10; // 1% — "activity" threshold
    struct GateCheck {
        bool block = false;    // whether power-off is blocked
        bool passive = false;  // transient block (dwell window, busy=0)
        std::string why;       // reason (for last_error/status)
    };
    GateCheck gate_check(int busy_idle_dwell_ms)
    {
        GateCheck r;
        std::string node = open_render_fd();
        if (audio_playback_active()) {
            r.block = true;
            r.why = "active PCM playback on the dGPU";
            return r;
        }
        if (node.empty()) return r;          // nobody holds it → free
        if (!g_pmu_config_valid) {
            r.block = true;
            r.why = node + " open + busy PMU counter unreliable (after recovery) — defer";
            return r;
        }
        auto now = std::chrono::steady_clock::now();
        if (g_last_busy > BUSY_ACTIVE_PROMILLE) {
            last_busy_nonzero_ = now;        // active render — fresh idle window
            r.block = true;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s open, busy=%u%% — active render",
                          node.c_str(), (unsigned)((g_last_busy + 50) / 10));
            r.why = buf;
            return r;
        }
        // Passive fd (probe): busy==0. The dwell protects against the race (a client starts
        // rendering right after the OFF decision — mpv on track B). Once the
        // busy-idle-dwell-ms window is satisfied → free (power-off passes).
        if (busy_idle_dwell_ms <= 0) return r;   // 0 = gate disabled (report 98)
        auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_busy_nonzero_).count();
        if (idle_ms < busy_idle_dwell_ms) {
            r.block = true;
            r.passive = true;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s open passively (busy=0%% for %dms, dwell %dms)",
                          node.c_str(), (int)idle_ms, busy_idle_dwell_ms);
            r.why = buf;
        }
        return r;
    }

    // Wait until power-off can pass (gate v2: no render fd OR the dGPU
    // idle for busy-idle-dwell-ms; dGPU audio blocks hard).
    // Timeout, 500 ms retry.
    bool wait_idle(int timeout_ms, int busy_idle_dwell_ms)
    {
        int waited = 0;
        while (waited < timeout_ms) {
            if (!gate_check(busy_idle_dwell_ms).block) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            waited += 500;
        }
        return !gate_check(busy_idle_dwell_ms).block;
    }

private:
    std::string backend_;
    int autosuspend_ms_;
    std::function<bool()> recover_cb_;
    // v5.13 (D1): timestamp of the last busy>0 sample (power-off gate —
    // busy-idle-dwell-ms). Reset at power-on (set_on) — a fresh cycle = a fresh
    // idle window, otherwise OFF would pass immediately after power-on before
    // a client manages to start rendering (lesson of report 99).
    std::chrono::steady_clock::time_point last_busy_nonzero_;
};

// ----------------------------------------------------------- switchd: consumers (/proc)
// v5.0: /proc scan for dGPU consumers. Two signals:
//   - /proc/*/environ contains DRI_PRIME=... (render-offload onto the dGPU)
//   - /proc/*/comm ∈ [dgpu-procs] (CUDA, blender — processes requiring the dGPU)
// Returns a list of process names (comm) — for the policy (dgpu_procs) and the status.

static std::vector<std::string> dgpu_consumers()
{
    std::vector<std::string> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        std::string bp = std::string("/proc/") + e->d_name;
        std::string comm;
        bool is_consumer = false;
        if (read_file((bp + "/comm").c_str(), comm) == 0) {
            comm = trim(comm);
            if (!comm.empty() && g_cfg.sw.dgpu_procs.count(comm)) is_consumer = true;
        }
        if (!is_consumer) {
            std::string env;
            if (read_file((bp + "/environ").c_str(), env) == 0) {
                size_t p0 = 0;
                while (p0 < env.size()) {
                    size_t nul = env.find('\0', p0);
                    std::string var = env.substr(p0, nul == std::string::npos
                        ? std::string::npos : nul - p0);
                    if (var.rfind("DRI_PRIME=", 0) == 0) { is_consumer = true; break; }
                    if (nul == std::string::npos) break;
                    p0 = nul + 1;
                }
            }
        }
        if (is_consumer && !comm.empty()) out.push_back(comm);
    }
    closedir(d);
    return out;
}

// Is an external display connected to the dGPU (card0) (other than eDP-1)?
static bool external_display_on_dgpu()
{
    DIR* d = opendir("/sys/class/drm");
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.rfind("card0-", 0) != 0) continue;
        if (name == "card0-eDP-1") continue;
        std::string status;
        if (read_file(("/sys/class/drm/" + name + "/status").c_str(), status) == 0) {
            if (trim(status) == "connected") { found = true; break; }
        }
    }
    closedir(d);
    return found;
}

// ----------------------------------------------------------- switchd: policy
// v5.6: title promotion — a Discord/YouTube card in a browser (window title
// matches a [preferred-titles] pattern, icontains) → dGPU, BUT it is not a latch:
// busy < title-idle-busy ([switch], default 33%) for dwell-out → demote
// to iGPU. Focus on a non-Discord/YT → demote after 1 s (regardless of busy).
// Hard promotion ([dgpu-hard]/external/[dgpu-procs]) = latch.

class SwitchPolicy {
public:
    enum Target { DGPU, IGPU };

    struct Input {
        std::string focused_class;
        // v5.6: the window title — title promotion of Discord/YouTube ([preferred-titles]).
        std::string focused_title;
        std::vector<std::string> consumers;   // dgpu_consumers() — comm names
        uint32_t busy = 0;                    // ‰ (g_last_busy)
        int temp = -1;                        // °C (dGPU nouveau hwmon)
        // v5.7: CPU temp (coretemp) — cpu-temp-gate for [dgpu-idle]; -1 when absent.
        int cpu_temp = -1;                    // °C
        bool external_display = false;
    };

    // Decision every tick. Returns the target dGPU power state.
    Target decide(const Input& in, const Config::SwitchCfg& cfg, int tick_ms)
    {
        auto now = std::chrono::steady_clock::now();
        auto since = [&](std::chrono::steady_clock::time_point tp) {
            return (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - tp).count();
        };

        // v5.9: a dGPU OFF (target IGPU) cannot be hot — the stale hwmon
        // value from before the power-off does not block promotion.
        const bool thermal_ok = (target_ == IGPU) || in.temp < 0 || in.temp < cfg.temp_gate;

        // Hard promotion: focused class ∈ dgpu_hard OR external OR dgpu_procs
        // (without a title — v5.6 removed browsers from [dgpu-hard]).
        bool hard = false;
        if (!in.focused_class.empty() && cfg.dgpu_hard.count(in.focused_class)) hard = true;
        if (in.external_display) hard = true;
        if (!hard) {
            for (auto& c : in.consumers)
                if (cfg.dgpu_procs.count(c)) { hard = true; break; }
        }

        // v5.6: [dgpu-idle] — classes with idle-demote (e.g. mpv): promote to the dGPU like
        // hard ones, BUT busy < class_idle_busy → demote to iGPU (symmetric with the
        // title promotion of Discord/YouTube).
        bool idle_class = !in.focused_class.empty() && cfg.dgpu_idle.count(in.focused_class);

        // v5.6: title promotion — a Discord/YouTube card in a browser → dGPU,
        // BUT it is not a latch: busy < title_idle_busy → demote to iGPU.
        bool title_promo = false;
        if (!in.focused_title.empty()) {
            for (auto& t : cfg.preferred_titles)
                if (icontains(in.focused_title, t)) { title_promo = true; break; }
        }
        // busy_known CORRECTION: busy from the PMU is authoritative ONLY while the dGPU is ON.
        // When target_==IGPU (dGPU OFF), the main loop forces busy=0 (PMU of the dead
        // card) — that is NOT an idle signal, just the absence of a measurement. Without this safeguard,
        // title promotion would never start from OFF (idle_title=always).
        const bool busy_known = (target_ == DGPU);
        bool idle_title = busy_known && (int)in.busy < cfg.title_idle_busy * 10;  // ‰
        // v5.9: CPU ≥ cpu-temp-promote + the dGPU has thermal headroom → hold the dGPU
        // (the card renders — CPU offload). busy < 5% (paused) → demote always.
        if (idle_title) {
            bool cpu_offload = cfg.cpu_temp_promote > 0 && in.cpu_temp >= 0 &&
                               in.cpu_temp >= cfg.cpu_temp_promote;
            // v5.9 guard: after power-on busy ≈ 0% (fresh counters) — before
            // pstate-settle-ms since promotion, hold the dGPU (the busy<5% escape applies later).
            bool fresh_on = since(last_promote_) < cfg.pstate_settle_ms;
            // v5.9: after power-on the dGPU temp read may be stale (the last
            // value before OFF / chassis soak with a hot CPU) —
            // for pstate-settle-ms the dGPU is treated as cold (it was OFF).
            bool dgpu_hot = !fresh_on && in.temp >= 0 && in.temp >= cfg.temp_gate;
            if (cpu_offload && !dgpu_hot && ((int)in.busy >= 50 || fresh_on))
                idle_title = false;
        }

        // Soft promotion: focused class ∈ dgpu_soft AND busy > busy_enter.
        bool soft = !in.focused_class.empty() && cfg.dgpu_soft.count(in.focused_class) &&
                    (int)in.busy > cfg.busy_enter * 10;

        // v5.6 demote_sig (simplified per the user's request): focus on a NON-Discord/YT →
        // demote after 1 s ALWAYS (regardless of busy) — the downclock to 07 is done
        // by [dgpu-active], power-off by switchd; busy < busy_exit is covered
        // by !title_promo. Focus on Discord/YT → demote only when idle_title
        // (busy_known && busy < title-idle-busy). igpu-class is kept for [igpu].
        bool demote_sig = !title_promo || idle_title ||
                          (!in.focused_class.empty() && cfg.igpu.count(in.focused_class));

        // Dwell counters (soft promotion — busy-gated).
        dwell_in_ = (soft && thermal_ok) ? dwell_in_ + tick_ms : 0;

        // Hard promotion — latch (as before, early return). A hard signal
        // holds the dGPU EVEN when temp >= temp_gate (the thermal gate blocks promotion,
        // but don't demote under an active requirement — otherwise flapping).
        // v5.9 fix: idle_class (mpv) ALSO enters this branch — without it the
        // [dgpu-idle] class never promoted (hard is set only by dgpu_hard/external/
        // dgpu_procs); all the idle_now/gate logic below is for it.
        if (hard || idle_class) {
            dwell_out_ = 0;   // promotion clears the demote signal
            // v5.6: [dgpu-idle] — idle-demote (e.g. mpv): when busy_known and busy <
            // class_idle_busy → do NOT hold the dGPU (fall through to the common
            // demote path). When the dGPU is OFF (busy_known=false) → promote like a hard class
            // (unknown busy ≠ idle — the same safeguard as title_promo).
            bool idle_now = idle_class && busy_known && (int)in.busy < cfg.class_idle_busy * 10;
            if (idle_now) {
                // v5.7: cpu-temp-gate — CPU hot (doing something else, e.g.
                // compiling) while the dGPU has thermal headroom → hold the dGPU.
                // busy < 5% (paused) → demote always.
                bool cpu_hot  = in.cpu_temp >= 0 && in.cpu_temp >= cfg.cpu_temp_gate;
                // v5.9: after power-on busy is unreliable (~0%, fresh counters) —
                // the busy<5% escape only after pstate-settle-ms since promotion, otherwise
                // the thermal promotion demotes after ~1 s and churns every hold.
                bool fresh_on = since(last_promote_) < cfg.pstate_settle_ms;
                // v5.9: after power-on the dGPU temp read may be stale (the last
                // value before OFF / chassis soak with a hot CPU) —
                // for pstate-settle-ms the dGPU is treated as cold (it was OFF).
                bool dgpu_hot = !fresh_on && in.temp >= 0 && in.temp >= cfg.temp_gate;
                // v5.9 fix: the busy<5% escape ONLY with a cool CPU — with a hot
                // CPU hold the dGPU even when busy≈0 (the CPU needs the offload;
                // the demote returns when the CPU cools < cpu-temp-gate).
                bool truly_idle = (int)in.busy < 50 && !fresh_on && !cpu_hot;
                if (cpu_hot && !dgpu_hot && !truly_idle) {
                    idle_now = false;
                    if (!gate_logged_) {
                        gate_logged_ = true;
                        logf(1, "switch: cpu-temp-gate — CPU %d°C >= %d°C, "
                                "dGPU %d°C < %d°C: holding the dGPU (class %s, busy %d%%)",
                             in.cpu_temp, cfg.cpu_temp_gate, in.temp, cfg.temp_gate,
                             in.focused_class.c_str(), (int)in.busy / 10);
                    }
                }
            }
            if (!idle_now) {
                if (thermal_ok) {
                    if (target_ != DGPU) {
                        // v5.7: after demoting the [dgpu-idle] class, re-promotion only
                        // when the title/focus changed OR the CPU entered the hot
                        // range (resuming video after pause), with a hold like
                        // for titles — no churn during a pause. For other
                        // hard classes: cooldown as before.
                        bool changed = in.focused_title != last_demote_title_ ||
                                       in.focused_class != last_demote_class_;
                        bool cpu_hot_now = in.cpu_temp >= 0 &&
                                           in.cpu_temp >= cfg.cpu_temp_gate;
                        bool cpu_rise = cpu_hot_now && !last_demote_cpu_hot_;
                        bool ok = idle_class
                            ? (changed || cpu_rise) &&
                              since(last_demote_) >= cfg.title_idle_hold_ms
                            : since(last_demote_) >= cfg.cooldown_ms;
                        if (!ok)
                            return target_;
                        last_promote_ = now;
                    }
                    target_ = DGPU;
                }
                // !thermal_ok: don't promote; hold the dGPU if already on.
                return target_;
            }
            // idle_now: fall through — demote via the common path below.
        }

        // v5.6: title promotion — an active Discord/YT card → dGPU (cooldown-
        // gated), WITHOUT early-return (idle can release it). When the dGPU is OFF (busy un-
        // known) promote on the title alone; when ON, neither promote nor hold while idle
        // (anti-flapping). dwell_out_ zeroed while active → demote doesn't tick.
        if (title_promo && !idle_title) {
            dwell_out_ = 0;
            // v5.7: re-promotion only after a title/focus change — a YT/
            // Discord card with busy below the threshold stays on the iGPU for the whole
            // lifetime of the same title (zero churn); new video/channel (the title changes)
            // → re-promotion, the hold still rate-limits.
            // v5.9: thermal CPU offload — CPU ≥ cpu-temp-promote → re-promotion
            // even without a title/focus change (the dGPU takes over rendering, the CPU cools).
            bool cpu_offload = cfg.cpu_temp_promote > 0 && in.cpu_temp >= 0 &&
                               in.cpu_temp >= cfg.cpu_temp_promote;
            bool changed = in.focused_title != last_demote_title_ ||
                           in.focused_class != last_demote_class_;
            if (target_ != DGPU && thermal_ok && (changed || cpu_offload) &&
                since(last_demote_) >= cfg.title_idle_hold_ms) {
                last_promote_ = now;
                target_ = DGPU;
            }
        }

        // Soft promotion — busy-gated via dwell_in.
        if (soft && thermal_ok && dwell_in_ >= cfg.dwell_in_ms) {
            dwell_out_ = 0;   // promotion clears the demote signal
            if (target_ != DGPU && since(last_demote_) < cfg.cooldown_ms) {
                // cooldown — wait
            } else {
                if (target_ != DGPU) last_promote_ = now;
                target_ = DGPU;
            }
            return target_;
        }

        // Demotion — after min-residence, via dwell_out.
        dwell_out_ = (target_ == DGPU && demote_sig) ? dwell_out_ + tick_ms : 0;
        if (target_ == DGPU && demote_sig &&
            since(last_promote_) >= cfg.min_residence_ms &&
            dwell_out_ >= cfg.dwell_out_ms) {
            target_ = IGPU;
            last_demote_ = now;
            // v5.7: remember the demote context — re-promotion (title-based and
            // [dgpu-idle]) only after a title/focus change or the CPU entering the
            // hot range. With cpu_temp_gate=0: cpu_hot always true →
            // demote only at busy<5%, re-promotion only after a title change.
            last_demote_title_ = in.focused_title;
            last_demote_class_ = in.focused_class;
            last_demote_cpu_hot_ = (in.cpu_temp >= 0 &&
                                    in.cpu_temp >= cfg.cpu_temp_gate);
            gate_logged_ = false;   // v5.7: reset the one-time gate log
            dwell_out_ = 0;
        }

        return target_;
    }

    Target target() const { return target_; }

private:
    Target target_ = IGPU;
    int dwell_in_ = 0, dwell_out_ = 0;
    std::chrono::steady_clock::time_point last_promote_{}, last_demote_{};
    // v5.7: context of the last demote — re-promotion (title-based and [dgpu-idle])
    // only after a title/focus change (zero YT/mpv churn) or the CPU entering the
    // hot range. Policy state — MUST survive SIGHUP (not in Config).
    std::string last_demote_title_;
    std::string last_demote_class_;
    bool last_demote_cpu_hot_ = false;
    // v5.7: one-time log when cpu-temp-gate first blocks the [dgpu-idle]
    // demote (no per-tick spam); reset at demote.
    bool gate_logged_ = false;
};

// ----------------------------------------------------------- switchd: status
// v5.0: NVRAM read of gpu-power-prefs (read-only — monitor/status). GUID
// fa4ce28d-b62f-4c99-9cc3-6815686e30f9, 4 B of data: 01 00 00 00 = IGD,
// 00 00 00 00 = DIS. The firmware hides the variable from efivarfs enumeration →
// raw flash fallback (store $VSS 0x610050/0x620050, reports 51/70).

static const char* NVRAM_EFIVAR =
    "/sys/firmware/efi/efivars/gpu-power-prefs-fa4ce28d-b62f-4c99-9cc3-6815686e30f9";
static const char* NVRAM_FLASH_DUMP =
    "$HOME/Projects/nv-kepler/tmp/nvram-dump/flash-8MB.bin";

// GUID in the EFI binary format (mixed-endian): fa4ce28d-b62f-4c99-9cc3-6815686e30f9.
static const uint8_t GPREFS_GUID_BIN[16] = {
    0x8d, 0xe2, 0x4c, 0xfa, 0x2f, 0xb6, 0x99, 0x4c,
    0x9c, 0xc3, 0x68, 0x15, 0x68, 0x6e, 0x30, 0xf9
};
// "gpu-power-prefs" in UTF-16LE (16 chars with null = 32 B).
static const uint8_t GPREFS_NAME_UTF16[32] = {
    0x67,0x00, 0x70,0x00, 0x75,0x00, 0x2d,0x00, 0x70,0x00, 0x6f,0x00,
    0x77,0x00, 0x65,0x00, 0x72,0x00, 0x2d,0x00, 0x70,0x00, 0x72,0x00,
    0x65,0x00, 0x66,0x00, 0x73,0x00, 0x00,0x00
};

static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Scans the VSS store (offset, 64 KiB) for the current gpu-power-prefs generation
// (State 0x7F). Returns "dis"/"igd"/"".
static std::string nvram_prefs_scan_store(int fd, off_t store_off)
{
    const size_t STORE_SIZE = 0x10000;
    off_t pos = store_off + 16;   // after the $VSS header (16 B)
    off_t end = store_off + (off_t)STORE_SIZE;
    while (pos + 36 <= end) {
        uint8_t hdr[36];
        if (pread(fd, hdr, sizeof hdr, pos) != (ssize_t)sizeof hdr) break;
        // StartId 0xAA55 in flash bytes = [0xAA, 0x55] (not an LE u16).
        if (hdr[0] != 0xAA || hdr[1] != 0x55) break;   // end of the variables (padding)
        uint8_t state = hdr[2];
        uint32_t name_size = le32(hdr + 8);
        uint32_t data_size = le32(hdr + 12);
        if (memcmp(hdr + 16, GPREFS_GUID_BIN, 16) == 0 &&
            name_size == sizeof GPREFS_NAME_UTF16) {
            uint8_t name[32];
            if (pread(fd, name, sizeof name, pos + 36) == (ssize_t)sizeof name &&
                memcmp(name, GPREFS_NAME_UTF16, sizeof name) == 0) {
                if (state == 0x7F && data_size >= 1) {
                    uint8_t d;
                    if (pread(fd, &d, 1, pos + 36 + name_size) == 1)
                        return d == 1 ? "igd" : "dis";
                }
            }
        }
        pos += 36 + name_size + data_size;
    }
    return "";
}

// Read of gpu-power-prefs from raw flash (/dev/mtd0ro or the dump). Returns "" when absent.
static std::string nvram_prefs_flash()
{
    const char* paths[] = { "/dev/mtd0ro", NVRAM_FLASH_DUMP };
    for (auto* p : paths) {
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        std::string r = nvram_prefs_scan_store(fd, 0x610050);
        if (r.empty()) r = nvram_prefs_scan_store(fd, 0x620050);
        close(fd);
        if (!r.empty()) return r;
    }
    return "";
}

// Read of gpu-power-prefs from efivarfs. Returns "" when unavailable.
static std::string nvram_prefs_efivarfs()
{
    std::string data;
    if (read_file(NVRAM_EFIVAR, data) != 0) return "";
    if (data.size() < 5) return "";
    uint8_t v = (uint8_t)data[4];   // first data byte (after 4 B of attributes)
    if (v == 1) return "igd";
    if (v == 0) return "dis";
    return "";
}

// Full read: efivarfs → raw flash. Result "dis"|"igd"|"unknown".
//
// v5.3: cache to /run/reclocked/nvram-prefs. Scanning the raw flash (/dev/mtd0ro) at
// every startup kills the kbd backlight: every MTD read → ledtrig_mtd_activity()
// → trigger "nand-disk" on smc::kbd_backlight (applesmc.c:1071) → oneshot blink
// leaves the LED at 0 → LKSB=0 (report 80). /run is tmpfs — the cache resets
// at reboot, which is correct (gpu-power-prefs only changes through the
// firmware at to-igd/to-dis + reboot). Flash read only when the cache is missing.
static const char* NVRAM_PREFS_CACHE = "/run/reclocked/nvram-prefs";

static std::string nvram_prefs_read()
{
    std::string cached;
    if (read_file(NVRAM_PREFS_CACHE, cached) == 0) {
        cached = trim(cached);
        logf(1, "nvram: cache %s (%s)", NVRAM_PREFS_CACHE, cached.c_str());
        return cached.empty() ? "unknown" : cached;
    }
    logf(1, "nvram: no cache %s — scanning efivarfs → raw flash", NVRAM_PREFS_CACHE);
    std::string r = nvram_prefs_efivarfs();
    if (r.empty()) r = nvram_prefs_flash();
    if (r.empty()) r = "unknown";
    write_file(NVRAM_PREFS_CACHE, r);   // best-effort — the cache is only an optimization
    return r;
}

// iGPU (Intel Iris Pro 5200) — read-only monitoring (option A, zero control).
static int read_igpu_freq_mhz()
{
    std::string s;
    if (read_file(IGPU_RPS_CUR, s) != 0) return -1;
    return std::atoi(s.c_str());
}

static std::string json_escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// ----------------------------------------------------------- switchd: executor + verification

class Switchd {
public:
    Switchd(Gpu& gpu, Hwmon& hw, const Config::SwitchCfg& cfg)
        : gpu_(gpu), hw_(hw), cfg_(cfg), power_(cfg.backend, cfg.autosuspend_ms)
    {
        power_.set_recover_cb([this]() { return recover_after_power_on(); });
    }

    void set_reset_cb(std::function<void()> cb) { reset_cb_ = std::move(cb); }

    bool enabled() const { return cfg_.enable; }

    void init()
    {
        topo_.detect();
        power_state_ = power_.read();
        // v5.3: mkdir before nvram_prefs_read — /run/reclocked must exist
        // before the nvram-prefs cache writes a file into it.
        mkdir("/run/reclocked", 0755);
        nvram_prefs_ = nvram_prefs_read();
        mkdir("/run/switchd", 0755);
        // v5.15: the dead gate survives daemon restart (the resume hook restarts
        // reclocked on post-resume — the file in /run survives; expires at reboot).
        refresh_dead();
        if (dead_present_)
            logf(0, "switchd: dGPU DEAD gate active at startup (reason: %s, "
                    "ts=%lld) — automatic power-on blocked", dead_reason_.c_str(), dead_ts_);
        logf(1, "switchd: topology=%s, dGPU=%s, nvram_prefs=%s, backend=%s, "
                "target=%s (in DIS = monitor — zero power changes)",
             topo_.name(), power_state_.on() ? "ON" : "OFF",
             nvram_prefs_.c_str(), cfg_.backend.c_str(), target_name());
    }

    // v5.7: cpu_temp — CPU temp (coretemp, °C, -1 when absent) for the cpu-temp-gate.
    void tick(HyprCtl& hypr, int temp, uint32_t last_busy, int cpu_temp)
    {
        // Own hyprctl (activewindow) — independent of the pstate polling.
        // v5.6: the window title is passed to the policy — title promotion of
        // Discord/YouTube ([preferred-titles]) in decide().
        std::string fcs, ftitle;
        bool a1 = hypr.activewindow(fcs, ftitle);

        // v5.0: dgpu-override — flag-file /run/reclocked/dgpu-override (on|off), the
        // fan-override pattern (G). The file existing → forces the target, policy.decide()
        // is not used. Override "off" still passes through the apply() gates:
        // wait_idle (releases after Bug1 — dGPU nodes only) + set_off + wait_off.
        // Do NOT bypass wait_idle — a safety gate. "on" — power-on with
        // wait_ready as usual. Status: field "override" (""|"on"|"off").
        std::string ovr = dgpu_override();
        // v5.15: refresh the dead gate (stat 1×/tick — cheap; loads the reason when
        // the file appeared — e.g. after a daemon restart).
        refresh_dead();
        if (ovr == "on" || ovr == "off") {
            // v5.15: a FRESH override "on" (reclockctl dgpu-on) clears the dead
            // gate — an explicit user decision = the only path to a power-on attempt besides
            // reboot. A failure of this attempt sets dead again (mark_dead
            // remembers dead_ovr_attempted_ → retry under a held override
            // is blocked; the next attempt = dgpu-auto + dgpu-on).
            if (ovr == "on" && prev_override_ != "on" && dead_present_)
                clear_dead("override dgpu-on (reclockctl)");
            target_ = (ovr == "on") ? SwitchPolicy::DGPU : SwitchPolicy::IGPU;
            override_ = ovr;
            if (prev_override_ != ovr) {
                logf(1, "switch: override — dGPU %s forced (flag-file %s)",
                     ovr == "on" ? "ON" : "OFF", DGPU_OVERRIDE_FILE);
                prev_override_ = ovr;
            }
        } else {
            override_ = "";
            if (!prev_override_.empty()) {
                logf(1, "switch: override REMOVED — resuming the switchd policy");
                prev_override_.clear();
            }
            SwitchPolicy::Input in;
            in.focused_class = a1 ? fcs : "";
            in.focused_title = a1 ? ftitle : "";
            in.consumers = dgpu_consumers();
            in.busy = last_busy;
            in.temp = temp;
            in.cpu_temp = cpu_temp;
            in.external_display = external_display_on_dgpu();

            target_ = policy_.decide(in, cfg_, cfg_.tick_ms);
            // v5.15: the "dGPU dead" gate — automatic promotions NEVER
            // request power-on. The policy may decide DGPU (title-promo etc.),
            // but the gate forces the target back to IGPU — the executor (apply)
            // never sees want_on. One-time log + rejected counter
            // (status: dgpu_dead_rejected; echoed every 60 ticks ≈ 1 min).
            if (dead_present_ && target_ == SwitchPolicy::DGPU) {
                target_ = SwitchPolicy::IGPU;
                dead_rejected_++;
                if (!dead_logged_) {
                    dead_logged_ = true;
                    logf(0, "switch: dGPU DEAD — automatic promotion REJECTED "
                            "(gate v" RECLKD_VERSION ", reason: %s). Unlock: reclockctl "
                            "dgpu-on or reboot", dead_reason_.c_str());
                } else if (dead_rejected_ % 60 == 0)
                    logf(1, "switch: dGPU DEAD — rejected automatic "
                            "promotions: %d", dead_rejected_);
            }
        }
        apply();
        write_status();
    }

    bool dgpu_off() const { return !power_state_.on(); }

    // v5.0: settle after power-on — don't write pstate for pstate_settle_ms after a
    // power-cycle (the kernel nvkm_pstate_calc may hang after D3hot→D0). Set
    // in recover_after_power_on(); the main loop skips the pstate decision in this window.
    bool pstate_settle_active() const {
        return std::chrono::steady_clock::now() < pstate_settle_until_;
    }
    int pstate_settle_remaining_ms() const {
        auto d = pstate_settle_until_ - std::chrono::steady_clock::now();
        return d.count() > 0
            ? (int)std::chrono::duration_cast<std::chrono::milliseconds>(d).count() : 0;
    }

    const char* topo_name() const { return topo_.name(); }
    const char* target_name() const {
        return target_ == SwitchPolicy::DGPU ? "dgpu" : "igpu";
    }
    const char* mode_name() const {
        if (!cfg_.enable) return "off";
        return topo_.topo() == IGD ? "active (IGD)" : "monitor (DIS)";
    }
    const std::string& nvram_prefs() const { return nvram_prefs_; }

    // v5.4: [dgpu-active] status — values set by the main loop every cycle.
    void set_dgpu_active_status(const char* state, int input_active, int video)
    {
        dgpu_state_ = state ? state : "off";
        dgpu_input_active_ = input_active;
        dgpu_video_ = video;
    }

    // Full recovery after a power-cycle (steps 2-10 per the plan). Called from
    // wait_ready (after power-on) and by the S3 self-heal in the main loop.
    bool recover_after_power_on()
    {
        // Step 2: wait for runtime_status=active (wait_ready_timeout_ms timeout).
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.wait_ready_timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = power_.read();
            if (st.runtime == "active") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        auto st = power_.read();
        if (st.runtime != "active") {
            last_error_ = "runtime_status != active after power-on";
            logf(0, "switch: recovery ERROR — runtime_status=%s (expected active)",
                 st.runtime.c_str());
            rollback_off();
            return false;
        }
        // Step 3: verify vgaswitcheroo = Pwr.
        if (st.vgasw != "Pwr") {
            last_error_ = "vgaswitcheroo != Pwr after power-on";
            logf(0, "switch: recovery ERROR — vgaswitcheroo=%s (expected Pwr)",
                 st.vgasw.c_str());
            rollback_off();
            return false;
        }
        // Step 4: verify the pstate debugfs exists (VBIOS came back).
        if (current_state().empty()) {
            last_error_ = "pstate debugfs unavailable after power-on";
            logf(0, "switch: recovery ERROR — pstate debugfs unavailable");
            rollback_off();
            return false;
        }
        // Step 5: hw.reinit() — re-scan of the nouveau hwmon (node disappeared at power-off).
        hw_.reinit();
        // Step 6: gpu.reopen_mmio() — fresh BAR0 mapping after the power-cycle.
        if (!gpu_.reopen_mmio()) {
            last_error_ = "reopen_mmio failed after power-on";
            logf(0, "switch: recovery ERROR — reopen_mmio");
            rollback_off();
            return false;
        }
        // Step 7: gpu.init_counters() — a fresh power-cycle = BAR0 counters lost.
        gpu_.init_counters();
        // Step 8: re-sync g_cur_idx (pstate may differ from what the daemon thinks).
        std::string cur = current_state();
        if (!cur.empty() && cur != "boot") {
            uint32_t cs = (uint32_t)std::strtol(cur.c_str(), nullptr, 16);
            int idx = state_to_idx(cs);
            if (idx >= 0) g_cur_idx = idx;
        }
        // Step 9: re-open DRM (when vblank_sync) — the fd may be stale after a power-cycle.
        if (g_cfg.vblank_sync) {
            if (g_drm_fd >= 0) { close(g_drm_fd); g_drm_fd = -1; }
            drm_open();
        }
        // Step 10: reset_after_transition() — fresh busy window.
        if (reset_cb_) reset_cb_();
        // v5.0: a fresh power-cycle = a fresh clock subsystem → unblock pstate
        // (g_pstate_stuck may remain after an nvkm_pstate_calc hang) and pause
        // pstate writes for pstate_settle_ms (the GPU must stabilize after
        // D3hot→D0 — the first clock change may hang the workqueue).
        g_pstate_stuck = false;
        pstate_settle_until_ = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.pstate_settle_ms);
        logf(1, "switch: recovery after power-on OK (hwmon=%s, idx=%d, pstate-settle=%dms)",
             hw_.ok() ? hw_.path().c_str() : "NONE", g_cur_idx, cfg_.pstate_settle_ms);
        return true;
    }

    // ---------------- v5.15: "dGPU dead" gate (report 116) ----------------
    // After a FAILED power-on a persistent state: file /run/reclocked/dgpu-dead
    // ("<unix_ts> <reason>", /run = tmpfs → expires at reboot). Automatic
    // promotions never request power-on (tick()); expiration: reclockctl
    // dgpu-on (override "on" clears it) or reboot. The file survives daemon
    // restart (resume hook) — read at init().

    // stat + (re)load of the reason when the file appeared/disappeared. Called every tick.
    void refresh_dead()
    {
        struct stat st;
        bool present = (stat(DGPU_DEAD_FILE, &st) == 0);
        if (present && !dead_present_) {
            std::string c;
            if (read_file(DGPU_DEAD_FILE, c) == 0) {
                // Format: "<unix_ts> <reason>" (one line).
                dead_reason_ = trim(c);
                size_t sp = dead_reason_.find(' ');
                if (sp != std::string::npos) {
                    dead_ts_ = std::strtoll(dead_reason_.substr(0, sp).c_str(),
                                            nullptr, 10);
                    dead_reason_ = dead_reason_.substr(sp + 1);
                }
            } else {
                dead_reason_ = "unknown (file read failed)";
            }
            dead_present_ = true;
            dead_logged_ = false;   // one-time log fresh again (e.g. after a restart)
        } else if (!present && dead_present_) {
            dead_present_ = false;
            dead_reason_.clear();
            dead_ts_ = 0;
            dead_rejected_ = 0;
            dead_ovr_attempted_ = false;
        }
    }

    void mark_dead(const std::string& reason)
    {
        if (!cfg_.poweron_dead_gate) {
            logf(0, "switch: power-on failed (%s) — dead gate DISABLED "
                    "(poweron-dead-gate=false)", reason.c_str());
            return;
        }
        dead_present_ = true;
        dead_reason_ = reason;
        dead_ts_ = (long long)std::time(nullptr);
        dead_logged_ = false;
        // An attempt made under a held override "on" — further retries under THE SAME
        // override blocked (apply); a fresh attempt = dgpu-auto + dgpu-on.
        dead_ovr_attempted_ = (override_ == "on");
        char buf[512];
        std::snprintf(buf, sizeof buf, "%lld %s", dead_ts_, reason.c_str());
        if (write_file(DGPU_DEAD_FILE, buf) != 0)
            logf(0, "switch: dGPU DEAD — writing %s failed (gate only in "
                    "process memory)", DGPU_DEAD_FILE);
        logf(0, "switch: dGPU DEAD (gate v" RECLKD_VERSION ") — power-on failed: %s. "
                "Automatic promotions will NO LONGER request power-on. "
                "Unlock: reclockctl dgpu-on (clears the gate, one attempt) "
                "or reboot", reason.c_str());
    }

    void clear_dead(const char* why)
    {
        unlink(DGPU_DEAD_FILE);
        dead_present_ = false;
        dead_reason_.clear();
        dead_ts_ = 0;
        dead_rejected_ = 0;
        dead_ovr_attempted_ = false;
        logf(1, "switch: dGPU-dead gate REMOVED (%s) — power-on attempt allowed",
             why);
    }

    bool dead_active() const { return dead_present_; }

private:
    void apply()
    {
        auto st = power_.read();
        bool cur_on = st.on();
        bool want_on = (target_ == SwitchPolicy::DGPU);

        if (want_on != cur_on) {
            auto now = std::chrono::steady_clock::now();
            auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_switch_).count();
            if (since_last < cfg_.min_switch_gap_ms) {
                logf(2, "switch: min-switch-gap — deferring the toggle (last %dms ago)",
                     (int)since_last);
            } else if (want_on) {
                // v5.14 (report 114): AUTO power-on gates — see the comment in
                // SwitchCfg. Override bypasses (the user's decision, not policy).
                if (override_.empty() &&
                    (up_ms_since_start() < cfg_.poweron_boot_guard_ms ||
                     (last_switch_ != tp_zero() &&
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - last_switch_).count() < cfg_.poweron_rate_limit_ms))) {
                    logf(1, "switch: dGPU power-on BLOCKED (gate v" RECLKD_VERSION ": %s, "
                            "up=%dms, last toggle %dms ago)",
                         up_ms_since_start() < cfg_.poweron_boot_guard_ms
                             ? "boot-guard (resume window)"
                             : "rate-limit",
                         up_ms_since_start(), (int)since_last);
                    last_action_ = "power-on-blocked";
                } else {
                // v5.15: override "on" AFTER a failed attempt under the same
                // override (dead_ovr_attempted_) — blocked; a fresh attempt =
                // reclockctl dgpu-auto + dgpu-on (a new transition clears dead).
                if (dead_present_ && dead_ovr_attempted_) {
                    dead_rejected_++;
                    last_action_ = "power-on-rejected-dead";
                    logf(2, "switch: power-on rejected (dGPU dead gate, "
                            "override 'on' still held) — rejected: %d",
                         dead_rejected_);
                } else {
                logf(1, "switch: dGPU power-on (target=DGPU, topology=%s)", topo_.name());
                if (!power_.set_on()) {
                    last_error_ = "set_on failed";
                    logf(0, "switch: ERROR set_on");
                    // v5.15: a rejected switcheroo write — nothing to retry
                    // automatically (retry-storm from boot -2); the dead gate.
                    mark_dead("set_on failed (write to " +
                              std::string(VGA_SWITCHEROO) + " rejected)");
                } else {
                    last_switch_ = now;
                    if (power_.wait_ready()) {
                        last_action_ = "power-on";
                        last_error_.clear();
                    } else {
                        // v5.15: recovery failed (runtime != active / vgasw
                        // != Pwr / no pstate / reopen_mmio fail) = the card is not
                        // alive. The recovery path already did the cleanup
                        // (rollback_off); the daemon LIVES — the dead gate blocks
                        // further automatic attempts (report 116: the 2nd attempt
                        // on a dead card killed the daemon in a syscall).
                        mark_dead("recovery after power-on failed: " + last_error_);
                    }
                }
                }
                }
            } else {
                if (topo_.topo() != IGD) {
                    logf(1, "switch: monitor — power-off blocked (topology %s)",
                         topo_.name());
                    last_action_ = "blocked-off";
                } else {
                    logf(1, "switch: dGPU power-off (target=IGPU, topology=IGD)");
                    if (!power_.wait_idle(cfg_.wait_idle_timeout_ms,
                                          cfg_.busy_idle_dwell_ms)) {
                        // v5.13 (D1, report 100): activity-aware gate —
                        // a passive render fd (busy=0 over busy-idle-dwell-ms)
                        // does NOT block power-off (chromium probe fd — report 99);
                        // real activity / an unreliable PMU counter / PCM does block.
                        // Dynamic message with the gate's reason (instead of a fixed
                        // "open /dev/dri fd" — misleading with a passive fd).
                        DgpuPower::GateCheck g = power_.gate_check(cfg_.busy_idle_dwell_ms);
                        if (g.why.empty()) g.why = "open dGPU render fd";
                        last_error_ = "dGPU busy (" + g.why + ") — power-off deferred";
                        // A passive dwell (busy=0, 3 s window) = a transient state —
                        // logged at level 2 (no spam every ~6 s); a real blocker
                        // (busy>0 / PMU unreliable / PCM) at level 0.
                        logf(g.passive ? 2 : 0, "switch: %s", last_error_.c_str());
                    } else if (!power_.set_off()) {
                        last_error_ = "set_off failed";
                        logf(0, "switch: ERROR set_off");
                    } else {
                        last_switch_ = now;
                        if (wait_off()) {
                            last_action_ = "power-off";
                            last_error_.clear();
                        }
                    }
                }
            }
        } else {
            last_action_ = "none";
        }

        power_state_ = power_.read();
    }

    bool wait_off()
    {
        auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(cfg_.wait_ready_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = power_.read();
            if (st.runtime == "suspended" || st.vgasw == "Off" || st.vgasw == "DynOff")
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        auto st = power_.read();
        last_error_ = "dGPU did not go off after power-off (runtime=" + st.runtime +
                      ", vgasw=" + st.vgasw + ")";
        logf(0, "switch: ERROR wait_off — %s", last_error_.c_str());
        return false;
    }

    void rollback_off()
    {
        if (topo_.topo() == IGD) {
            if (power_.set_off())
                logf(0, "switch: rollback — dGPU OFF");
            else
                logf(0, "switch: ERROR rollback — set_off failed");
        } else {
            logf(0, "switch: rollback skipped (topology %s — power-off blocked)",
                 topo_.name());
        }
    }

    void write_status()
    {
        auto st = power_.read();
        int igpu_freq = read_igpu_freq_mhz();
        char buf[1024];
        // v5.4: "dgpu_state" carries the [dgpu-active] pstate state (active|deep_idle|
        // heavy|off); when the section is disabled → historical on/off (power). Power
        // on/off stays in "dgpu_power". New fields: input_active (1/0), video (1/0).
        const char* dgpu_state_val = dgpu_state_.c_str();
        if (!g_cfg.dgpu.enable)
            dgpu_state_val = st.on() ? "on" : "off";
        std::snprintf(buf, sizeof buf,
            "{ \"version\": \"" RECLKD_VERSION "\", "
            "\"topology\": \"%s\", \"dgpu_power\": \"%s\", \"dgpu_state\": \"%s\", "
            "\"input_active\": %d, \"video\": %d, "
            "\"target\": \"%s\", \"override\": \"%s\", \"last_action\": \"%s\", "
            "\"last_error\": \"%s\", "
            // v5.15: the "dGPU dead" gate — state + reason + ts + rejected counter
            // of automatic promotions (reclockctl switch-status / the Omarchy bar).
            "\"dgpu_dead\": %s, \"dgpu_dead_reason\": \"%s\", "
            "\"dgpu_dead_since\": %lld, \"dgpu_dead_rejected\": %d, "
            "\"nvram_prefs\": \"%s\", \"igpu_freq_mhz\": %d, "
            "\"fan_curve\": \"%s\", \"fan_tmin\": %d, \"fan_tmax\": %d, "
            "\"fan_tmid\": %d, \"fan_pmid\": %d, "
            "\"fan_rpm1\": %d, \"fan_rpm2\": %d, "
            "\"fan_case_avg\": %.1f, \"fan_case_state\": \"%s\", \"ts\": %lld }\n",
            topo_.name(), st.on() ? "on" : "off", dgpu_state_val,
            dgpu_input_active_, dgpu_video_,
            target_name(), json_escape(override_).c_str(),
            json_escape(last_action_).c_str(),
            json_escape(last_error_).c_str(),
            dead_present_ ? "true" : "false",
            json_escape(dead_reason_).c_str(),
            dead_ts_, dead_rejected_,
            nvram_prefs_.c_str(), igpu_freq,
            g_fan_curve.c_str(), g_fan_tmin, g_fan_tmax, g_fan_tmid, g_fan_pmid,
            g_fan_rpm1, g_fan_rpm2,
            g_fan_case_avg, g_fan_case_state,
            (long long)std::time(nullptr));
        write_file(SWITCH_STATUS_FILE, buf);
        write_file(SWITCH_DGPU_FILE, st.on() ? "on" : "off");
    }

    Gpu& gpu_;
    Hwmon& hw_;
    const Config::SwitchCfg& cfg_;
    DgpuPower power_;
    Topology topo_;
    SwitchPolicy policy_;
    std::function<void()> reset_cb_;

    SwitchPolicy::Target target_ = SwitchPolicy::IGPU;
    DgpuPower::State power_state_;
    std::string nvram_prefs_ = "unknown";
    std::string last_action_ = "none";
    std::string last_error_;
    std::chrono::steady_clock::time_point last_switch_{};
    std::chrono::steady_clock::time_point pstate_settle_until_{}; // v5.0: settle window after a power-on
    // v5.14: daemon start time — AUTO power-on boot-guard (resume window).
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    long long up_ms_since_start() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count();
    }
    static std::chrono::steady_clock::time_point tp_zero() {
        return std::chrono::steady_clock::time_point{};
    }
    std::string override_ = "";       // the active dgpu-override (""|"on"|"off")
    std::string prev_override_ = "";  // previous one — change log (fan-override pattern)
    // v5.15: the "dGPU dead" gate (report 116) — a persistent state after a failed
    // power-on. dead_present_ = the file /run/reclocked/dgpu-dead exists;
    // dead_logged_ = the one-time log was emitted; dead_ovr_attempted_ = an attempt under
    // override "on" already happened and failed (retry under the same
    // override blocked — a fresh attempt = dgpu-auto + dgpu-on).
    bool dead_present_ = false;
    bool dead_logged_ = false;
    bool dead_ovr_attempted_ = false;
    int  dead_rejected_ = 0;          // rejected automatic promotions
    std::string dead_reason_;         // reason for setting the gate (from the file)
    long long dead_ts_ = 0;           // unix ts of when the gate was set
    // v5.4: [dgpu-active] status — set by the main loop.
    std::string dgpu_state_ = "off";
    int dgpu_input_active_ = 0;
    int dgpu_video_ = 0;
};

// ----------------------------------------------------------- input (evdev)
// v5.4: user-activity detection via /dev/input/event* (evdev). A separate thread
// with poll() (500 ms timeout) — it does not steal events from Hyprland: evdev is
// multicast, each reader has its own queue (report 79 §2.1). Every
// EV_KEY/EV_REL/EV_ABS event sets the atomic timestamp last_activity_ms (steady_clock).
// No devices / activity-source != evdev → last_activity_ms() = 0 (idle after
// dwell; wake only via busy/title-change). Re-scan on a removed fd.

class InputReader {
public:
    InputReader() = default;
    ~InputReader() { stop(); }

    // Start the thread only when source == "evdev". Idempotent: stop() → start()
    // allows a restart after SIGHUP (activity-source change). The rescan is
    // synchronous — device_count() is accurate right after start().
    void start(const std::string& source)
    {
        stop();
        if (source != "evdev") return;
        rescan();
        running_ = true;
        thread_ = std::thread([this]() { loop(); });
    }

    void stop()
    {
        running_ = false;
        if (thread_.joinable()) thread_.join();   // poll has a 500 ms timeout — quick join
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : fds_) close(fd);
        fds_.clear();
    }

    // steady_clock ms of the last event; 0 = never (no devices / source != evdev).
    uint64_t last_activity_ms() const
    {
        return last_activity_.load(std::memory_order_relaxed);
    }

    // Number of open evdev devices (0 = no input signal).
    int device_count() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return (int)fds_.size();
    }

private:
    void loop()
    {
        while (running_) {
            std::vector<int> fds;
            {
                std::lock_guard<std::mutex> lk(mu_);
                fds = fds_;
            }
            std::vector<struct pollfd> pfds;
            pfds.reserve(fds.size());
            for (int fd : fds) pfds.push_back({fd, POLLIN, 0});
            int r = poll(pfds.data(), pfds.size(), 500);
            if (r < 0) {
                if (errno == EINTR) continue;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                rescan();
                continue;
            }
            if (r == 0) continue;   // timeout — no new events
            bool dropped = false;
            for (size_t i = 0; i < pfds.size(); i++) {
                if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { dropped = true; continue; }
                if (pfds[i].revents & POLLIN) drain(fds[i]);
            }
            if (dropped) rescan();   // device removed — simple re-open
        }
    }

    void drain(int fd)
    {
        struct input_event ev;
        while (running_) {
            ssize_t n = read(fd, &ev, sizeof ev);
            if (n == (ssize_t)sizeof ev) {
                if (ev.type == EV_KEY || ev.type == EV_REL || ev.type == EV_ABS) {
                    auto now = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
                    last_activity_.store((uint64_t)ms, std::memory_order_relaxed);
                }
                // EV_SYN / EV_MSC — ignore (not user activity)
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;   // queue empty (O_NONBLOCK)
            } else {
                break;   // error / removed device
            }
        }
    }

    void rescan()
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (int fd : fds_) close(fd);
        fds_.clear();
        glob_t g;
        if (glob("/dev/input/event*", GLOB_NOSORT, nullptr, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                // Skip devices that cannot be opened (root — most will open; missing
                // permissions / temporary unavailability = simply skip).
                int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) fds_.push_back(fd);
            }
            globfree(&g);
        }
    }

    std::atomic<uint64_t> last_activity_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;       // protects fds_
    std::vector<int> fds_;        // under mu_
};

// ----------------------------------------------------------- usage

static void usage(const char* argv0)
{
    std::printf(
        "reclocked v" RECLKD_VERSION " — profile policy (app-aware) + fans + [fan-case] + switchd + [dgpu-active] + override + reload + nvram cache + dGPU-dead gate\n"
        "Usage: %s [options]\n"
        "  switchd (v" RECLKD_VERSION "): dGPU power-state + render routing. In DIS = monitor\n"
        "    (zero power changes). Sections [switch]/[dpower]/[dgpu-hard]/[dgpu-soft]\n"
        "    /[igpu]/[dgpu-procs]. Status: /run/reclocked/status.\n"
        "  [dgpu-active] (v5.6): three-stage policy for the ENTIRE dGPU-ON:\n"
        "    baseline (0a) / deep idle (07) / heavy (0e). User activity via\n"
        "    evdev (/dev/input/event*). 0e EXCLUSIVELY busy-driven (busy>busy-enter\n"
        "    + temp<temp-up); title/video class informational only. [caps] floor\n"
        "    = hard minimum, low-power ceiling, per-profile thermals. enable=false\n"
        "    → old logic. Status: dgpu_state/input_active/video.\n"
        "  Profiles: default (cap 07) ↔ preferred (cap 0e).\n"
        "  SAFE AUTO ladder: 07 ↔ 0a ↔ 0e. 0f = BOOST TIER above the ladder\n"
        "  (enters from 0e on sustained busy>busy-boost AND temp<temp-up).\n"
        "  PER-PROFILE thermal guard: default 65/58, preferred 82/75.\n"
        "  Profile from hyprctl (activewindow + clients), class list from the config.\n"
        "  UP 07→0a→0e (UP-LOAD): g_cur_idx<ceiling AND temp<temp_up for\n"
        "                        temp_dwell AND busy>busy_up. One level/step.\n"
        "  BOOST 0e→0f: g_cur_idx==ceiling AND busy>busy_boost for boost_dwell\n"
        "              AND temp<temp_up for temp_dwell.\n"
        "  DOWN 0f→0e: temp>temp_up OR temp>=temp_down (IMMEDIATE) OR busy<busy_up.\n"
        "  DOWN 0e→0a→07: TERMAL (temp>temp_down for temp_dwell) OR IDLE\n"
        "                 (busy≤busy_down for idle_dwell) OR CEILING.\n"
        "  --config PATH      config file (default /etc/reclocked.conf)\n"
        "  --interval MS      sampling period (200)\n"
        "  --poll-ms MS       hyprctl polling period (1000)\n"
        "  --busy-up P        %% busy > → UP by 1 level (80)\n"
        "  --busy-down P      %% busy ≤ → idle dwell (40)\n"
        "  --busy-boost P     %% busy > → BOOST UP 0e→0f (85)\n"
        "  --boost-dwell-ms MS dwell of sustained busy>busy-boost to enter 0f (5000)\n"
        "  --boost-hyst P     pp boost hysteresis reserve (10)\n"
        "  --temp-up C        °C UP/BOOST allowed (default 58, preferred 75)\n"
        "  --temp-down C      °C DOWN TERMAL throttle (default 65, preferred 82)\n"
        "  --temp-dwell-ms MS temperature dwell (5000)\n"
        "  --idle-dwell-ms MS idle dwell (5000)\n"
        "  --profile-dwell-ms MS profile-change rate-limit (2000)\n"
        "  --win-ms MS        busy smoothing window (1000)\n"
        "  --exit-state S     pstate at exit (07)\n"
        "  --vblank-sync / --no-vblank-sync  (default on)\n"
        "  --probe            20 busy samples, no changes\n"
        "  --dry-run          decisions without pstate writes\n"
        "  -v                 more logs\n",
        argv0);
}

// ------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    enum { OPT_CONFIG = 1000, OPT_INTERVAL, OPT_POLL, OPT_BUSY_UP, OPT_BUSY_DOWN,
           OPT_BUSY_BOOST, OPT_BOOST_DWELL, OPT_BOOST_HYST,
           OPT_TEMP_UP, OPT_TEMP_DOWN, OPT_TEMP_DWELL, OPT_IDLE_DWELL,
           OPT_PROFILE_DWELL, OPT_WIN, OPT_EXIT, OPT_PROBE, OPT_DRY,
           OPT_VERSION };
    static const option long_opts[] = {
        {"version",         no_argument,       nullptr, OPT_VERSION},
        {"config",          required_argument, nullptr, OPT_CONFIG},
        {"interval",        required_argument, nullptr, OPT_INTERVAL},
        {"poll-ms",         required_argument, nullptr, OPT_POLL},
        {"busy-up",         required_argument, nullptr, OPT_BUSY_UP},
        {"busy-down",       required_argument, nullptr, OPT_BUSY_DOWN},
        {"busy-boost",      required_argument, nullptr, OPT_BUSY_BOOST},
        {"boost-dwell-ms",  required_argument, nullptr, OPT_BOOST_DWELL},
        {"boost-hyst",      required_argument, nullptr, OPT_BOOST_HYST},
        {"temp-up",         required_argument, nullptr, OPT_TEMP_UP},
        {"temp-down",       required_argument, nullptr, OPT_TEMP_DOWN},
        {"temp-dwell-ms",   required_argument, nullptr, OPT_TEMP_DWELL},
        {"idle-dwell-ms",   required_argument, nullptr, OPT_IDLE_DWELL},
        {"profile-dwell-ms",required_argument, nullptr, OPT_PROFILE_DWELL},
        {"win-ms",          required_argument, nullptr, OPT_WIN},
        {"exit-state",      required_argument, nullptr, OPT_EXIT},
        {"probe",           no_argument,       nullptr, OPT_PROBE},
        {"dry-run",         no_argument,       nullptr, OPT_DRY},
        {"vblank-sync",     no_argument,       nullptr, 'V'},
        {"no-vblank-sync",  no_argument,       nullptr, 'N'},
        {"verbose",         no_argument,       nullptr, 'v'},
        {"help",            no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    // The v3 default values in g_cfg are already set (def struct).
    // First parse the CLI to get --config, then load_config, then
    // re-apply the CLI override. Keep the CLI values in separate fields.
    struct CliVal {
        bool set=false; int v=0;
    } cli_interval, cli_poll, cli_busy_up, cli_busy_down, cli_busy_boost,
      cli_boost_dwell, cli_boost_hyst, cli_temp_up, cli_temp_down,
      cli_temp_dwell, cli_idle_dwell, cli_profile_dwell, cli_win, cli_exit;
    bool cli_vblank = false, cli_vblank_val = true;

    int c;
    while ((c = getopt_long(argc, argv, "vh", long_opts, nullptr)) != -1) {
        switch (c) {
        case OPT_CONFIG:       g_config_path = optarg; break;
        case OPT_INTERVAL:     cli_interval.set=true;       cli_interval.v=std::atoi(optarg); break;
        case OPT_POLL:         cli_poll.set=true;           cli_poll.v=std::atoi(optarg); break;
        case OPT_BUSY_UP:      cli_busy_up.set=true;        cli_busy_up.v=std::atoi(optarg); break;
        case OPT_BUSY_DOWN:    cli_busy_down.set=true;      cli_busy_down.v=std::atoi(optarg); break;
        case OPT_BUSY_BOOST:   cli_busy_boost.set=true;     cli_busy_boost.v=std::atoi(optarg); break;
        case OPT_BOOST_DWELL:  cli_boost_dwell.set=true;    cli_boost_dwell.v=std::atoi(optarg); break;
        case OPT_BOOST_HYST:   cli_boost_hyst.set=true;     cli_boost_hyst.v=std::atoi(optarg); break;
        case OPT_TEMP_UP:      cli_temp_up.set=true;        cli_temp_up.v=std::atoi(optarg); break;
        case OPT_TEMP_DOWN:    cli_temp_down.set=true;      cli_temp_down.v=std::atoi(optarg); break;
        case OPT_TEMP_DWELL:   cli_temp_dwell.set=true;     cli_temp_dwell.v=std::atoi(optarg); break;
        case OPT_IDLE_DWELL:   cli_idle_dwell.set=true;     cli_idle_dwell.v=std::atoi(optarg); break;
        case OPT_PROFILE_DWELL:cli_profile_dwell.set=true;  cli_profile_dwell.v=std::atoi(optarg); break;
        case OPT_WIN:          cli_win.set=true;            cli_win.v=std::atoi(optarg); break;
        case OPT_EXIT:         cli_exit.set=true;           cli_exit.v=(int)std::strtol(optarg,nullptr,16); break;
        case OPT_PROBE:        g_cfg.probe = true; break;
        case OPT_DRY:          g_cfg.dry = true; break;
        case 'V':              cli_vblank=true; cli_vblank_val=true;  break;
        case 'N':              cli_vblank=true; cli_vblank_val=false; break;
        case 'v':              g_cfg.verbosity++; break;
        case OPT_VERSION:      std::printf("reclocked v" RECLKD_VERSION "\n"); return 0;
        case 'h':              usage(argv[0]); return 0;
        default:               usage(argv[0]); return 2;
        }
    }

    if (geteuid() != 0) {
        std::fprintf(stderr, "reclocked: requires root (mmap BAR0 + debugfs write)\n");
        return 1;
    }

    // Load the config (if it exists). Default values already in g_cfg.
    if (g_config_path.empty()) g_config_path = DEFAULT_CONFIG;
    bool cfg_loaded = load_config(g_config_path, g_cfg);
    if (cfg_loaded)
        logf(1, "config loaded: %s", g_config_path.c_str());
    else
        logf(1, "config %s unavailable — using the built-in defaults",
             g_config_path.c_str());

    // Apply CLI overrides (they take precedence over the config).
    if (cli_interval.set)       g_cfg.interval_ms      = cli_interval.v;
    if (cli_poll.set)           g_cfg.poll_ms          = cli_poll.v;
    if (cli_busy_up.set)        g_cfg.busy_up          = cli_busy_up.v;
    if (cli_busy_down.set)      g_cfg.busy_down        = cli_busy_down.v;
    if (cli_busy_boost.set)     { g_cfg.def.busy_boost = g_cfg.preferred.busy_boost = cli_busy_boost.v; }
    if (cli_boost_dwell.set)    { g_cfg.def.boost_dwell_ms = g_cfg.preferred.boost_dwell_ms = cli_boost_dwell.v; }
    if (cli_boost_hyst.set)     { g_cfg.def.boost_hyst = g_cfg.preferred.boost_hyst = cli_boost_hyst.v; }
    if (cli_temp_up.set)        { g_cfg.def.temp_up = g_cfg.preferred.temp_up = cli_temp_up.v; }
    if (cli_temp_down.set)      { g_cfg.def.temp_down = g_cfg.preferred.temp_down = cli_temp_down.v; }
    if (cli_temp_dwell.set)     g_cfg.temp_dwell_ms    = cli_temp_dwell.v;
    if (cli_idle_dwell.set)     g_cfg.idle_dwell_ms    = cli_idle_dwell.v;
    if (cli_profile_dwell.set)  g_cfg.profile_dwell_ms = cli_profile_dwell.v;
    if (cli_win.set)            g_cfg.win_ms           = cli_win.v;
    if (cli_exit.set)           g_cfg.exit_state       = cli_exit.v;
    if (cli_vblank)             g_cfg.vblank_sync      = cli_vblank_val;

    // Validation of the profile states.
    auto prof_ok = [](const Profile& p) {
        if (!known_state(p.max_pstate)) return false;
        if (p.boost_pstate >= 0 && !known_state(p.boost_pstate)) return false;
        // boost must be OUTSIDE the ladder (off-ladder, state_to_idx == -1) and higher
        // than max_pstate (e.g. 0f when max=0e). On-ladder boost <= max = an error.
        if (p.boost_pstate >= 0) {
            int bi = state_to_idx(p.boost_pstate);
            if (bi >= 0 && bi <= state_to_idx(p.max_pstate)) return false;
        }
        return true;
    };
    if (!prof_ok(g_cfg.def) || !prof_ok(g_cfg.preferred)) {
        std::fprintf(stderr, "reclocked: invalid states in the config profiles\n");
        return 2;
    }
    if (!known_state(g_cfg.exit_state)) {
        std::fprintf(stderr, "reclocked: exit-state must be 07|0a|0e|0f\n");
        return 2;
    }
    if (g_cfg.interval_ms <= 0) g_cfg.interval_ms = 200;
    if (g_cfg.poll_ms < g_cfg.interval_ms) g_cfg.poll_ms = g_cfg.interval_ms;
    if (g_cfg.gr_idle_promille < 0) g_cfg.gr_idle_promille = 0;
    if (g_cfg.gr_idle_promille > 1000) g_cfg.gr_idle_promille = 1000;

    Gpu gpu;
    if (!gpu.open_mmio()) return 1;

    if (g_cfg.probe) {
        gpu.dump_raw();
        gpu.init_counters();
        std::printf("probe: 20 samples every %d ms (busy in %%) + temp\n", g_cfg.interval_ms);
        Hwmon hw; bool hok = hw.init();
        for (int i = 0; i < 20; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.interval_ms));
            uint32_t b = gpu.sample();
            std::printf("  sample %2d: busy=%5.1f%% temp=%d°C\n", i + 1,
                        b / 10.0, hok ? hw.read_temp() : -1);
            std::fflush(stdout);
        }
        return 0;
    }

    Hwmon hw;
    bool hw_ok = hw.init();
    if (!hw_ok)
        logf(0, "WARNING: nouveau hwmon unavailable — thermal conditions skipped (fail-safe)");

    // v4.2: applesmc fans. init reads fanN_min/max dynamically from sysfs.
    Fan fan;
    bool fan_ok = false;
    if (g_cfg.fan_enable) {
        fan_ok = fan.init();
        if (fan_ok)
            logf(1, "fan: applesmc OK — fan1=%d-%d RPM, fan2=%d-%d RPM, "
                    "curve %d-%d°C (dGPU ON) / %d-%d°C (iGPU-only)",
                 fan.fan1_min(), fan.fan1_max(), fan.fan2_min(), fan.fan2_max(),
                 g_cfg.fan_temp_min, g_cfg.fan_temp_max,
                 g_cfg.fan_temp_min_igd, g_cfg.fan_temp_max_igd);
        else
            logf(0, "WARNING: applesmc unavailable — fan control disabled (fail-safe)");
    } else {
        logf(1, "fan: disabled in the config (enable=false)");
    }

    // v5.10: chassis thermals — [fan-case]. Init = scan of applesmc labels
    // (label → tempN_input path); case measured only when the section is enabled.
    SmcCase smc_case;
    if (g_cfg.fan_case.enable) {
        smc_case.init();
        logf(1, "fan-case: applesmc labels: %d found, keys=%zu, "
                "case curve %d-%d°C, soak-enter=%d°C, dwell-in=%dms, "
                "soak-hold=%dms (time-based exit), ramp=%d RPM/s",
             smc_case.label_count(), g_cfg.fan_case.keys.size(),
             g_cfg.fan_case.case_min, g_cfg.fan_case.case_max,
             g_cfg.fan_case.soak_enter, g_cfg.fan_case.dwell_in_ms,
             g_cfg.fan_case.hold_ms, g_cfg.fan_case.ramp_rpm_s);
    } else {
        logf(1, "fan-case: disabled in the config (missing/disable [fan-case]) — old 1:1 algorithm");
    }

    // v4.6: compiler detection (fan boost). Status at startup.
    if (g_cfg.compiler_enable)
        logf(1, "compiler: /proc detection ACTIVE (boost fan-max=%d%%)",
             g_cfg.compiler_fan_max);
    else
        logf(1, "compiler: disabled in the config (enable=false)");

    std::string old_pm;
    bool pm_saved = read_file(POWER_CTRL, old_pm) == 0;
    if (pm_saved) write_file(POWER_CTRL, "on");

    auto restore = [&]() {
        if (!g_cfg.dry) {
            if (set_pstate((uint32_t)g_cfg.exit_state) == 0)
                logf(1, "exit: pstate -> %s", state_hex(g_cfg.exit_state));
        }
        // v4.2: give the fans back to SMC auto (fail-safe — don't leave manual).
        if (fan_ok) fan.restore_auto();
        if (pm_saved) write_file(POWER_CTRL, old_pm);
    };
    std::signal(SIGINT, on_term);
    std::signal(SIGTERM, on_term);
    std::signal(SIGHUP, on_hup);

    gpu.init_counters();
    if (g_cfg.vblank_sync) drm_open();

    // Detect Hyprland (the user socket). Missing → fallback to default.
    HyprCtl hypr;
    bool hypr_ok = hypr.detect();
    if (hypr_ok)
        logf(1, "hyprctl: uid=%d HIS=%s", hypr.uid(), hypr.his().c_str());
    else
        logf(0, "WARNING: Hyprland not detected (/run/user/*/hypr/*/hyprland.lock) — "
                "fallback to the default profile (cap 07)");

    // Initialize the current ladder index.
    g_cur_idx = -1;
    std::string cur = current_state();
    if (!cur.empty() && cur != "boot") {
        uint32_t cs = (uint32_t)std::strtol(cur.c_str(), nullptr, 16);
        g_cur_idx = state_to_idx(cs);
    }

    Ring ring;
    ring.resize((size_t)(g_cfg.win_ms / std::max(g_cfg.interval_ms, 1)));

    // Dwell-countery.
    int temp_low_dwell  = 0;
    int temp_high_dwell = 0;
    int idle_dwell      = 0;
    int boost_up_dwell  = 0; // busy > busy_boost (BOOST UP 0e→0f)

    // Boost tier: when true, the GPU is on 0f (off-ladder), g_cur_idx stays at
    // ceiling (0e). Every boost change resets the dwell counters.
    bool g_boost_active = false;

    // v5.4: [dgpu-active] — machine state (status + signals).
    // last_activity_ts = steady_clock ms of the last activity (evdev input /
    // title/class change / busy >= deep-idle-busy); 0 = no activity ever.
    uint64_t last_activity_ts = 0;
    std::string prev_focused_class;
    std::string prev_focused_title;
    std::string dgpu_state_str = "off";   // "off"|"active"|"deep_idle"|"heavy"
    int dgpu_input_active = 0;
    int dgpu_video = 0;
    bool prev_title_video = false;   // informational log of the video-status change

    auto reset_after_transition = [&]() {
        ring.clear();
        temp_low_dwell = temp_high_dwell = idle_dwell = 0;
        boost_up_dwell = 0;
    };

    // v5.0: switchd module — dGPU power-state + render routing.
    Switchd sw(gpu, hw, g_cfg.sw);
    sw.set_reset_cb(reset_after_transition);
    sw.init();

    // v5.4: user-activity detection via evdev (separate thread).
    InputReader input;
    std::string current_input_source = g_cfg.dgpu.activity_source;
    input.start(current_input_source);

    // Profile state + rate-limit.
    bool pref_active = false;
    int pref_dwell = 0, def_dwell = 0;

    // Override state (log entry/exit).
    bool prev_override = false;

    // hyprctl polling: every poll_ms (cycle counter).
    int poll_cycles = std::max(1, g_cfg.poll_ms / g_cfg.interval_ms);
    int cycle = 0;
    std::string focused_class;
    std::string focused_title;   // v4.1: for Discord/YouTube detection by title
    std::vector<std::string> running_classes;
    bool hypr_alive = hypr_ok;
    // v4.2: previous RPMs for change logging (level 1) vs every-1s log (level 2).
    int prev_fan_rpm1 = -1, prev_fan_rpm2 = -1;
    bool prev_fan_override = false;
    int prev_fan_floor = 0;          // v5.16: log floor changes (60→90 etc.)
    // v5.10: [fan-case] — SOAK state machine (patterns: dwell counter L2267-2270,
    // steady_clock deadline L1390-1407). State survives SIGHUP (like SwitchPolicy).
    enum class FanCaseState { NORMAL, SOAK };
    FanCaseState fan_case_state = FanCaseState::NORMAL;
    int fan_case_dwell_in = 0;        // ms of case > soak-enter (NORMAL → SOAK)
    double fan_case_entry_avg = -1.0; // case_avg at SOAK entry (floor)
    std::chrono::steady_clock::time_point fan_case_hold_until{}; // v5.12: end of the SOAK pulse
    std::set<std::string> missing_case_keys;   // warning about wrong keys — once

    // v5.4: [dgpu-active] — status in the startup log.
    std::string dgpu_active_log = "off";
    if (g_cfg.dgpu.enable) {
        dgpu_active_log = std::string("yes baseline=") + state_hex(g_cfg.dgpu.baseline) +
                          " max=" + state_hex(g_cfg.dgpu.max) +
                          " evdev=" + std::to_string(input.device_count()) + " dev.";
    }

    logf(1, "start v" RECLKD_VERSION ": interval=%dms poll=%dms, "
            "default[cap=%s temp-up=%d temp-down=%d], "
            "preferred[cap=%s boost=%s busy-boost=%d%% boost-dwell=%dms "
            "temp-up=%d temp-down=%d], "
            "busy-up=%d%% busy-down=%d%%, temp-dwell=%dms idle-dwell=%dms, "
            "profile-dwell=%dms, hwmon=%s, hypr=%s, vblank=%d, "
            "gr-idle=%d‰, preferred-titles=%zu, low-power=%zu, "
            "fan=%s temp[%d-%d] igd[%d-%d] fan1[%d-%d] fan2[%d-%d], state=%s, switch=%s, "
            "dgpu-active=%s, fan-case=%s case[%d-%d] keys=%zu soak=%dms ramp=%d",
         g_cfg.interval_ms, g_cfg.poll_ms,
         state_hex(g_cfg.def.max_pstate), g_cfg.def.temp_up, g_cfg.def.temp_down,
         state_hex(g_cfg.preferred.max_pstate),
         g_cfg.preferred.boost_pstate >= 0 ? state_hex(g_cfg.preferred.boost_pstate) : "-",
         g_cfg.preferred.busy_boost, g_cfg.preferred.boost_dwell_ms,
         g_cfg.preferred.temp_up, g_cfg.preferred.temp_down,
         g_cfg.busy_up, g_cfg.busy_down, g_cfg.temp_dwell_ms, g_cfg.idle_dwell_ms,
         g_cfg.profile_dwell_ms,
         hw_ok ? hw.path().c_str() : "NONE",
         hypr_ok ? "yes" : "no", g_cfg.vblank_sync ? 1 : 0,
         g_cfg.gr_idle_promille,
         g_cfg.preferred_titles.size(), g_cfg.low_power_classes.size(),
         fan_ok ? "yes" : (g_cfg.fan_enable ? "NONE" : "off"),
         g_cfg.fan_temp_min, g_cfg.fan_temp_max,
         g_cfg.fan_temp_min_igd, g_cfg.fan_temp_max_igd,
         fan.fan1_min(), fan.fan1_max(), fan.fan2_min(), fan.fan2_max(),
         cur.c_str(), sw.mode_name(), dgpu_active_log.c_str(),
         g_cfg.fan_case.enable ? "yes" : "off",
         g_cfg.fan_case.case_min, g_cfg.fan_case.case_max,
         g_cfg.fan_case.keys.size(), g_cfg.fan_case.hold_ms,
         g_cfg.fan_case.ramp_rpm_s);

    const int busy_up_pp   = g_cfg.busy_up   * 10;
    const int busy_down_pp = g_cfg.busy_down * 10;

    // v5.15 (report 116): fatal-but-clean — an unexpected exception in the loop
    // (e.g. bad_alloc under OOM) must NOT kill the process without cleanup. Exiting
    // the loop → restore() (pstate exit + SMC auto fans) → return 0; systemd
    // (Restart=on-failure + StartLimit*, v5.15 unit) brings the daemon back.
    try {
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.interval_ms));
        if (g_reload) {
            g_reload = 0;
            Config nc;
            // Preserve CLI-only flags (probe/dry/verbosity) and config_path.
            nc.probe = g_cfg.probe; nc.dry = g_cfg.dry; nc.verbosity = g_cfg.verbosity;
            if (load_config(g_config_path, nc)) {
                // Re-apply the CLI overrides.
                if (cli_busy_boost.set)  { nc.def.busy_boost = nc.preferred.busy_boost = cli_busy_boost.v; }
                if (cli_boost_dwell.set) { nc.def.boost_dwell_ms = nc.preferred.boost_dwell_ms = cli_boost_dwell.v; }
                if (cli_boost_hyst.set)  { nc.def.boost_hyst = nc.preferred.boost_hyst = cli_boost_hyst.v; }
                if (cli_temp_up.set)     { nc.def.temp_up = nc.preferred.temp_up = cli_temp_up.v; }
                if (cli_temp_down.set)   { nc.def.temp_down = nc.preferred.temp_down = cli_temp_down.v; }
                if (cli_busy_up.set)     nc.busy_up = cli_busy_up.v;
                if (cli_busy_down.set)   nc.busy_down = cli_busy_down.v;
                if (cli_temp_dwell.set)  nc.temp_dwell_ms = cli_temp_dwell.v;
                if (cli_idle_dwell.set)  nc.idle_dwell_ms = cli_idle_dwell.v;
                if (cli_profile_dwell.set) nc.profile_dwell_ms = cli_profile_dwell.v;
                if (cli_interval.set)    nc.interval_ms = cli_interval.v;
                if (cli_poll.set)        nc.poll_ms = cli_poll.v;
                if (cli_win.set)         nc.win_ms = cli_win.v;
                if (cli_exit.set)        nc.exit_state = cli_exit.v;
                if (cli_vblank)          nc.vblank_sync = cli_vblank_val;
                g_cfg = nc;
                // v5.10: fresh applesmc label map (in case the SMC added sensors).
                if (g_cfg.fan_case.enable) smc_case.rescan();
                logf(1, "SIGHUP: config reloaded (%s)", g_config_path.c_str());
            } else {
                logf(0, "SIGHUP: config reload failed — keeping the old one");
            }
        }

        int temp = hw.read_temp();   // v5.0: moved up (shared)

        // v5.4: restart the InputReader when SIGHUP changed the activity-source.
        if (current_input_source != g_cfg.dgpu.activity_source) {
            logf(1, "input: activity-source %s -> %s (reader restart)",
                 current_input_source.c_str(), g_cfg.dgpu.activity_source.c_str());
            current_input_source = g_cfg.dgpu.activity_source;
            input.start(current_input_source);
        }

        // v5.0: switchd tick (every poll_cycles) — ALWAYS, regardless of the dGPU state.
        // Its own hyprctl (activewindow) + g_last_busy + temp.
        // v5.7: + hw.read_cpu_temp() (coretemp, -1 when absent) — cpu-temp-gate.
        if (sw.enabled() && (cycle % poll_cycles) == 0)
            sw.tick(hypr, temp, g_last_busy, hw.read_cpu_temp());

        // v5.0: fan block (every poll_cycles) — MOVED BEFORE the gate (also runs
        // when the dGPU is OFF; temp=-1 when the nouveau hwmon disappeared → min curve,
        // the compiler boost works).
        if (fan_ok && g_cfg.fan_enable && (cycle % poll_cycles) == 0) {
            bool fov = fan_override_active();
            // v5.16: the flag content = FLOOR % (50-100) — the curve may raise RPM
            // above (thermals/boost), never drops below. Empty content
            // = hold (previous behavior; reclockctl fan-off = empty file).
            int fan_floor_pct = fov ? fan_override_pct() : 0;
            if (fov && !prev_fan_override)
                logf(1, "fan-override ACTIVE (flag=/run/reclocked/fan-override)");
            if (fan_floor_pct != prev_fan_floor)
                logf(1, "fan-override FLOOR %d%%: curve active, RPM will not drop below%s",
                     fan_floor_pct, fan_floor_pct > 0 ? "" : " (hold — empty flag)");
            prev_fan_floor = fan_floor_pct;
            if (!fov && prev_fan_override)
                logf(1, "fan-override REMOVED — resuming auto fans");
            prev_fan_override = fov;
            if (!fov || fan_floor_pct > 0) {
                // v4.6: compiler → boost. When a running compiler is detected
                // (clang/gcc/make/cmake/... — /proc scan, compiler_running()),
                // fans go to fan-max% (default 100 = full fans). Boost
                // ONLY in auto-mode — the fan-override flag takes priority (do not
                // override manual control). When the compiler disappears →
                // the normal path: the temp curve below.
                bool boost_on = false;
                std::string cname;
                if (g_cfg.compiler_enable) {
                    cname = compiler_running();
                    boost_on = !cname.empty();
                }
                // v5.0 G1: react to the hottest source — the dGPU when ON, CPU (coretemp) when OFF
                // (lesson: temp=-1 with dGPU OFF → min curve, the 58°C CPU heats up).
                int ft = temp;
                int ctemp = hw.read_cpu_temp();
                if (ctemp > ft) ft = ctemp;
                // v5.1: the active curve — standard (dGPU ON) or igd (iGPU-only,
                // dGPU OFF → temp=CPU). Chosen by the dGPU power state (not the topology).
                // v5.9: + the 3-point curve inflection point per the active curve.
                int tmin = g_cfg.fan_temp_min;
                int tmax = g_cfg.fan_temp_max;
                int tmid = g_cfg.fan_temp_mid;
                int pmid = g_cfg.fan_mid;
                bool igd_curve = sw.enabled() && sw.dgpu_off();
                if (igd_curve) {
                    tmin = g_cfg.fan_temp_min_igd; tmax = g_cfg.fan_temp_max_igd;
                    tmid = g_cfg.fan_temp_mid_igd; pmid = g_cfg.fan_mid_igd;
                }
                // v5.1: remember the state for the status (the active curve + RPM).
                if (boost_on) {
                    // v5.16: boost takes precedence, but the floor still applies (max).
                    fan.set_boost(std::max(g_cfg.compiler_fan_max, fan_floor_pct));
                    g_fan_curve = "compiler";
                } else {
                    // v5.10: [fan-case] — chassis curve + SOAK/HOLD + RAMP
                    // (report 97, variant B). Priorities: max(pct_cpu, pct_case,
                    // soak_floor); written via set_pct (RAMP ±ramp-rpm-s).
                    // Compiler boost (above) and fan-override are overriding —
                    // they skip the ramp (safety). The igd/dga curve choice unchanged:
                    // the case works independently of the dGPU state (set B does not include
                    // TG0P — no RPM jump at power-on).
                    double pct_cpu = fan.pct_from_temp(ft, tmin, tmid, tmax, pmid);
                    double pct_target = pct_cpu;
                    double case_avg = -1.0;
                    if (g_cfg.fan_case.enable) {
                        case_avg = smc_case.read_avg(g_cfg.fan_case.keys,
                                                     missing_case_keys);
                        double pct_case = fan_case_pct(case_avg,
                                                       g_cfg.fan_case.case_min,
                                                       g_cfg.fan_case.case_max);
                        auto now = std::chrono::steady_clock::now();
                        if (fan_case_state == FanCaseState::NORMAL) {
                            // v5.12: NORMAL → SOAK: case > soak-enter for
                            // dwell-in (counter, the dwell_out_ L2267-2270 pattern).
                            // Threshold = soak-enter (default 45°C — above the typical
                            // working zone 40-44°C), NOT case-min: case-min lies
                            // below the working zone → "case > case-min" is
                            // always true → SOAK would enter forever.
                            fan_case_dwell_in = (case_avg > g_cfg.fan_case.soak_enter)
                                ? fan_case_dwell_in + g_cfg.poll_ms : 0;
                            if (case_avg > g_cfg.fan_case.soak_enter &&
                                fan_case_dwell_in >= g_cfg.fan_case.dwell_in_ms) {
                                fan_case_state = FanCaseState::SOAK;
                                fan_case_entry_avg = case_avg;
                                fan_case_hold_until = now + std::chrono::milliseconds(
                                    g_cfg.fan_case.hold_ms);
                                logf(1, "fan-case: SOAK (case %.1f°C > %d°C for %d ms, "
                                        "pulse %d ms, floor=curve at %.1f°C)",
                                     case_avg, g_cfg.fan_case.soak_enter,
                                     fan_case_dwell_in, g_cfg.fan_case.hold_ms,
                                     fan_case_entry_avg);
                            }
                        } else {
                            // v5.12: SOAK → NORMAL: PURELY TIME-BASED exit after
                            // hold-ms (time decay) — the ONLY guaranteed
                            // exit: the previous one (case < case-min−margin) was
                            // physically unreachable (17/27/37°C) → SOAK hung
                            // forever (zero exits in the 23:46-01:20 history).
                            // Re-entry when case again > soak-enter for dwell-in
                            // (a hot case → another pulse; cooled → the
                            // normal state). The guaranteed pulse end = no
                            // permanent SOAK.
                            if (now >= fan_case_hold_until) {
                                fan_case_state = FanCaseState::NORMAL;
                                fan_case_dwell_in = 0;
                                logf(1, "fan-case: NORMAL (the %d ms pulse elapsed, "
                                        "case %.1f°C)",
                                     g_cfg.fan_case.hold_ms, case_avg);
                            }
                        }
                        pct_target = std::max(pct_target, pct_case);
                        if (fan_case_state == FanCaseState::SOAK) {
                            // v5.11: SOAK — target = max(entry_floor,
                            // curve(current case)): fans ESCALATE with
                            // the current chassis temperature (they don't stand rigidly
                            // at the entry floor); the entry floor = lower
                            // bound (fans don't drop below while the case
                            // hasn't cooled — a downward ratchet).
                            double soak_floor =
                                fan_case_pct(fan_case_entry_avg,
                                             g_cfg.fan_case.case_min,
                                             g_cfg.fan_case.case_max);
                            pct_target = std::max(pct_target,
                                std::max(soak_floor, pct_case));
                            g_fan_case_state = "soak";
                        } else {
                            g_fan_case_state = "normal";
                        }
                    }
                    g_fan_case_avg = case_avg;
                    // v5.16: floor — the curve may raise, never drop below.
                    double pct_out = fan_floor_pct > 0
                        ? std::max(pct_target, fan_floor_pct / 100.0) : pct_target;
                    fan.set_pct(pct_out,
                                g_cfg.fan_case.enable ? g_cfg.fan_case.ramp_rpm_s : 0);
                    g_fan_curve = fan_floor_pct > 0 ? "floor" : (igd_curve ? "igd" : "dga");
                    g_fan_tmin = tmin; g_fan_tmax = tmax;
                    g_fan_tmid = tmid; g_fan_pmid = pmid;
                }
                int r1 = fan.last_rpm1(), r2 = fan.last_rpm2();
                g_fan_rpm1 = r1; g_fan_rpm2 = r2;
                if (r1 != prev_fan_rpm1 || r2 != prev_fan_rpm2) {
                    if (boost_on)
                        logf(1, "fan: COMPILER detected (%s) -> fan1=%d fan2=%d RPM (boost %d%%)",
                             cname.c_str(), r1, r2, g_cfg.compiler_fan_max);
                    // v5.9: 3-point curve — log with the inflection point
                    // (tmin-tmid-tmax); mid disabled → legacy 2-point format.
                    else if (tmid > tmin && tmid < tmax && pmid > 0)
                        logf(1, "fan: temp=%d°C -> fan1=%d fan2=%d RPM (curve %d-%d-%d°C%s)",
                             ft, r1, r2, tmin, tmid, tmax, igd_curve ? " igd" : "");
                    else
                        logf(1, "fan: temp=%d°C -> fan1=%d fan2=%d RPM (curve %d-%d°C%s)",
                             ft, r1, r2, tmin, tmax, igd_curve ? " igd" : "");
                    prev_fan_rpm1 = r1; prev_fan_rpm2 = r2;
                } else if (g_cfg.verbosity >= 2) {
                    if (boost_on)
                        logf(2, "fan: COMPILER detected (%s) fan1=%d fan2=%d RPM (unchanged, boost %d%%)",
                             cname.c_str(), r1, r2, g_cfg.compiler_fan_max);
                    else
                        logf(2, "fan: temp=%d°C fan1=%d fan2=%d RPM (unchanged)",
                             ft, r1, r2);
                }
            } else {
                // v5.1/v5.16: an override with an empty flag = hold — the daemon does not write,
                // state for the status. With content (floor) — the curve active above.
                g_fan_curve = "override";
            }
        }

        // v5.0: pstate gate — when the dGPU is OFF, skip sample() (BAR0 after a power-cut =
        // garbage = 1000‰ = boost to 0e on a dead card; lesson of report 63).
        if (sw.enabled() && sw.dgpu_off()) {
            g_last_busy = 0;   // zero phantom busy — otherwise g_last_busy stays frozen
                               // at the last value (e.g. 1000‰) and the switchd tick
                               // (soft busy-gated promotion) sees busy from the dead
                               // card. After the on() fix (= vgasw Off) this is the real state.
            // v5.4: status — dGPU OFF, [dgpu-active] inactive.
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = "off"; dgpu_input_active = 0; dgpu_video = 0;
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            cycle++; continue;
        }

        // v5.0: settle after power-on — don't write pstate for pstate_settle_ms after a
        // power-cycle (the kernel nvkm_pstate_calc may hang after D3hot→D0; the first
        // clock change may hang the workqueue → the next write hangs in D state).
        if (sw.enabled() && sw.pstate_settle_active()) {
            if (g_cfg.verbosity >= 2)
                logf(2, "pstate: settle after power-on (%dms) — skipping the decision",
                     sw.pstate_settle_remaining_ms());
            // v5.4: status — the new logic is paused until the settle ends.
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = (g_cur_idx >= 0 && g_cur_idx < LADDER_N)
                                 ? dgpu_state_name(g_cur_idx, g_cfg.dgpu) : "settle";
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            cycle++; continue;
        }

        uint32_t b = gpu.sample();
        g_last_busy = b;

        // v4.5: self-heal after S3. After suspend/resume (deep) the GPU loses the
        // busy-PMU counter configuration in BAR0 — total does not count, sample()
        // returns a stale 1000‰, which blocks the IDLE downshift and defers DOWN forever
        // through the GR-idle gate (the daemon stays at 0e; report 63). The readback of
        // R_IDLE_CTRL is cheap (mmap, every interval_ms) and has no false alarms:
        // CTRL_VALUE_ALWAYS is the normal state, any other value = configuration
        // lost (the typical state after resume). After re-init the counters count again
        // and the daemon descends to the correct idle itself.
        uint32_t ctrl_v = gpu.rd(R_IDLE_CTRL + C_TOTAL * 16);
        if ((ctrl_v & CTRL_VALUE_MASK) != CTRL_VALUE_ALWAYS) {
            // v5.13 (D1): unreliable counter → the power-off gate (D1) defers —
            // busy could read 0 despite activity (lesson of reports 99/100).
            g_pmu_config_valid = false;
            logf(0, "PMU busy counters config lost (post-resume?) — full recovery (ctrl=%08x)", ctrl_v);
            // v5.0: full recovery (steps 2-10) — the counters are only the beginning;
            // a power-cycle also requires hw.reinit, g_cur_idx re-sync, DRM re-open.
            if (sw.enabled())
                sw.recover_after_power_on();
            else {
                gpu.init_counters();
                reset_after_transition();   // ring.clear() + dwell=0 — fresh busy window
            }
        } else {
            // v5.13 (D1): readback OK — the counter is reliable (the power-off gate can
            // trust busy). If the recovery fails, the readback stays ≠ ALWAYS →
            // flag false until a successful re-init (the gate safely defers).
            g_pmu_config_valid = true;
        }

        ring.push(b);
        uint32_t busy_avg = ring.avg(); // ‰

        // v4.3: title-match (Discord/YouTube by window title) = the highest priority
        // of the preferred signal. Set in the pref_sig section below; reset each cycle.
        // When true: UP to ceiling without the busy-gate + IDLE downshift suppressed
        // (TERMAL remains — thermals > title). See UP-LOAD and IDLE DOWN.
        bool title_pref = false;

        // Override flag-file — freezes auto.
        bool ov = override_active();
        if (ov && !prev_override) {
            std::string oc = override_content();
            logf(1, "override ACTIVE: hold (flag=%s)", oc.empty() ? "?" : oc.c_str());
        }
        if (!ov && prev_override) {
            logf(1, "override REMOVED — resuming auto");
            // Re-synchronize the current state with debugfs.
            std::string cs = current_state();
            if (!cs.empty() && cs != "boot") {
                uint32_t v = (uint32_t)std::strtol(cs.c_str(), nullptr, 16);
                int idx = state_to_idx(v);
                if (idx >= 0) g_cur_idx = idx;
            }
            reset_after_transition();
        }
        prev_override = ov;
        if (ov) {
            if (g_cfg.verbosity >= 2)
                logf(2, "override hold, busy=%.0f%%, temp=%d°C", busy_avg/10.0f, temp);
            continue;
        }

        // hyprctl polling every poll_cycles.
        // v4.1 Bug-fix: when !hypr_alive (the daemon started before the Hyprland session
        // — typical for a systemd service), cyclically re-detect every poll_ms. Previously
        // the block was gated `if (hypr_alive && ...)` — when the startup detection failed, re-detect
        // never fired and the daemon stayed on default cap=07 for the whole session.
        if ((cycle % poll_cycles) == 0) {
            if (hypr_alive) {
                std::string fcs, ftitle;
                std::vector<std::string> rcs;
                bool a1 = hypr.activewindow(fcs, ftitle);
                bool a2 = hypr.clients(rcs);
                if (a1) { focused_class = fcs; focused_title = ftitle; }
                else    { focused_class.clear(); focused_title.clear(); }
                if (a2) running_classes = rcs; else running_classes.clear();
                // If both failed — maybe Hyprland restarted. Re-detect.
                if (!a1 && !a2) {
                    logf(0, "hyprctl unavailable — re-detecting the instance");
                    if (hypr.detect()) {
                        hypr.apply_env();
                        logf(1, "hyprctl re-detected: uid=%d HIS=%s",
                             hypr.uid(), hypr.his().c_str());
                    } else {
                        hypr_alive = false;
                        logf(0, "Hyprland gone — fallback to default");
                    }
                }
            } else {
                // !hypr_alive: cyclic re-detect (the session may come up after the daemon start).
                if (hypr.detect()) {
                    hypr.apply_env();
                    hypr_alive = true;
                    logf(1, "hyprctl detected: uid=%d HIS=%s (session ready)",
                         hypr.uid(), hypr.his().c_str());
                    // Immediate poll this cycle — pref_sig doesn't wait another poll_ms.
                    std::string fcs, ftitle;
                    std::vector<std::string> rcs;
                    if (hypr.activewindow(fcs, ftitle)) { focused_class = fcs; focused_title = ftitle; }
                    if (hypr.clients(rcs)) running_classes = rcs;
                }
                // else: stay !hypr_alive, retry next poll_ms.
            }
        }
        cycle++;

        // Preferred signal.
        // v4.1: low-power gate — when the focused window is a terminal (class ∈
        // low_power_classes), force default (cap=07) with priority over preferred.
        // Even if Discord/YouTube generates busy in the background (running-busy), the terminal
        // with focus holds 07.
        bool low_power_focused = hypr_alive && !focused_class.empty() &&
                                 g_cfg.low_power_classes.count(focused_class) > 0;
        // v4.4: a class with a [caps] entry — per-class policy (floor/max/busy-up).
        // Applied when the class is in FOCUS; a browser card (chromium etc.)
        // has no entry → title-priority force-0e unchanged.
        const Config::ClassCap* ccap = nullptr;
        if (hypr_alive && !focused_class.empty()) {
            auto cit = g_cfg.class_caps.find(focused_class);
            if (cit != g_cfg.class_caps.end()) ccap = &cit->second;
        }
        bool cap_class_focused = (ccap != nullptr);

        bool pref_sig = false;
        if (hypr_alive && !low_power_focused) {
            // focused ∈ list ([preferred] or [caps])?
            if (!focused_class.empty() &&
                (g_cfg.preferred_classes.count(focused_class) ||
                 cap_class_focused))
                pref_sig = true;
            // running ∈ list ([preferred]) AND busy > busy_up?  [caps] only when focused.
            if (!pref_sig && (int)busy_avg > busy_up_pp) {
                for (auto& r : running_classes) {
                    if (g_cfg.preferred_classes.count(r)) { pref_sig = true; break; }
                }
            }
            // v4.1: focused.title matches a preferred_titles pattern? (Discord/YouTube
            // are browser tabs — detection by window title, case-insensitive.)
            // v4.3: title-match has the HIGHEST priority — sets title_pref=true, which
            // forces UP to ceiling without the busy-gate and suppresses IDLE (see UP-LOAD/IDLE).
            // v4.4: skip title-match for classes with [caps] — desktop Discord is
            // handled by [caps] (floor/busy-gate), not force-0e by title.
            // A Discord card in a browser (chromium etc., NOT in [caps]) keeps the
            // title-priority force-0e.
            // v5.4: with [dgpu-active], the title does NOT set title_pref (force-0e goes away) —
            // it remains a video qualifier (computed below); pref_sig (profile for
            // the thermal thresholds) still receives the title as a fallback for browsers
            // outside [preferred].
            if (!focused_title.empty() && !cap_class_focused) {
                for (auto& t : g_cfg.preferred_titles) {
                    if (icontains(focused_title, t)) {
                        pref_sig = true;
                        if (!g_cfg.dgpu.enable) title_pref = true;
                        break;
                    }
                }
            }
        }
        if (pref_sig) { pref_dwell += g_cfg.interval_ms; def_dwell = 0; }
        else          { def_dwell += g_cfg.interval_ms; pref_dwell = 0; }

        if (!pref_active && pref_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = true; pref_dwell = 0;
            logf(1, "profile -> PREFERRED (focused=%s, title=%s, busy=%.0f%%, title-pref=%s)",
                 focused_class.c_str(), focused_title.c_str(), busy_avg/10.0f,
                 title_pref ? "YES" : "no");
        } else if (pref_active && def_dwell >= g_cfg.profile_dwell_ms) {
            pref_active = false; def_dwell = 0;
            logf(1, "profile -> DEFAULT (def_dwell=%dms)", g_cfg.profile_dwell_ms);
        }

        const Profile& prof = pref_active ? g_cfg.preferred : g_cfg.def;
        int ceiling = state_to_idx(prof.max_pstate);
        // v4.4: [caps] — ceiling (max), floor (resting state) and an own busy-up.
        int cap_floor = -1;                       // idx in the LADDER; -1 = none
        // v5.4: with [dgpu-active] the general UP-LOAD threshold = the section's busy-enter
        // (default 80), not the global busy-up. [caps] busy-up still per-class.
        int class_busy_up_pp = g_cfg.dgpu.enable ? g_cfg.dgpu.busy_enter * 10
                                                 : busy_up_pp; // ‰ busy for UP-LOAD
        if (ccap) {
            if (ccap->max >= 0) {
                int max_idx = state_to_idx(ccap->max);
                if (max_idx >= 0 && max_idx < ceiling) ceiling = max_idx;
            }
            if (ccap->floor >= 0) {
                int fl = state_to_idx(ccap->floor);
                if (fl >= 0 && fl <= ceiling) cap_floor = fl;
            }
            if (ccap->busy_up > 0) class_busy_up_pp = ccap->busy_up * 10;
        }
        int busy_boost_pp = prof.busy_boost * 10;

        // boost active when: defined, valid, OFF-LADDER (state_to_idx<0,
        // i.e. 0f) — an on-ladder boost is treated as disabled (it makes no sense
        // as a tier above the ladder). prof_ok already validated it at startup.
        const bool boost_enabled = (prof.boost_pstate >= 0 &&
                                    known_state(prof.boost_pstate) &&
                                    state_to_idx(prof.boost_pstate) < 0);

        // v5.4: [dgpu-active] — signals + activation. title_video is INFORMATIONAL
        // (status/log "video: ...", user decision #4) — it does NOT decide the pstate
        // state: 0e enters exclusively through busy > busy-enter (+ thermals).
        bool title_video = false;
        if (g_cfg.dgpu.enable && hypr_alive && !cap_class_focused) {
            if (!focused_title.empty()) {
                for (auto& t : g_cfg.preferred_titles)
                    if (icontains(focused_title, t)) { title_video = true; break; }
            }
            if (!title_video && !focused_class.empty() &&
                g_cfg.dgpu.video_classes.count(focused_class))
                title_video = true;
        }

        // v5.4: video title = INFORMATIONAL — log on change (user decision #4).
        if (g_cfg.dgpu.enable && title_video != prev_title_video) {
            logf(1, "dgpu-active: video=%s (title: \"%s\", class: %s) — informational, "
                    "0e exclusively via busy",
                 title_video ? "YES" : "NO",
                 focused_title.c_str(), focused_class.c_str());
            prev_title_video = title_video;
        }

        // v5.4: last_activity_ts — activity sources: evdev input, title/class
        // change of the window, busy >= deep-idle-busy (the GPU renders — an animated
        // page without scroll holds floor 0a). no_input_ms is the time since
        // the latest of them; the floor drops to 07 when ≥ dwell.
        uint64_t now_ms = 0;
        uint64_t no_input_ms = 0;   // 0 when the activity is fresh; UINT64_MAX when never
        if (g_cfg.dgpu.enable) {
            auto now = std::chrono::steady_clock::now();
            now_ms = (uint64_t)std::chrono::duration_cast<
                std::chrono::milliseconds>(now.time_since_epoch()).count();
            uint64_t ia = input.last_activity_ms();
            if (ia > last_activity_ts) last_activity_ts = ia;
            if (focused_class != prev_focused_class || focused_title != prev_focused_title) {
                last_activity_ts = now_ms;
                prev_focused_class = focused_class;
                prev_focused_title = focused_title;
            }
            if ((int)busy_avg >= g_cfg.dgpu.deep_idle_busy * 10)
                last_activity_ts = now_ms;
            no_input_ms = (last_activity_ts == 0)
                          ? UINT64_MAX : (now_ms > last_activity_ts ? now_ms - last_activity_ts : 0);
        }

        // v5.4: thermals of [dgpu-active] — per-profile (by focus) or shared.
        // When the section is disabled: td/tu = the profile values (temp-per-profile=true
        // default) — exactly as before.
        const int td = g_cfg.dgpu.temp_per_profile ? prof.temp_down : g_cfg.dgpu.temp_down;
        const int tu = g_cfg.dgpu.temp_per_profile ? prof.temp_up   : g_cfg.dgpu.temp_up;

        // Dwell counters.
        if (temp >= 0) {
            temp_low_dwell  = (temp <  tu) ? temp_low_dwell  + g_cfg.interval_ms : 0;
            temp_high_dwell = (temp >  td) ? temp_high_dwell + g_cfg.interval_ms : 0;
        } else {
            temp_low_dwell = temp_high_dwell = 0;
        }
        // v5.4: IDLE threshold — with [dgpu-active] the section's busy-exit (default 40),
        // otherwise the global busy-down (40). Hysteresis 80/40 in both cases.
        const int idle_down_pp = g_cfg.dgpu.enable ? g_cfg.dgpu.busy_exit * 10
                                                   : busy_down_pp;
        idle_dwell = ((int)busy_avg <= idle_down_pp)
                     ? idle_dwell + g_cfg.interval_ms : 0;
        boost_up_dwell = ((int)busy_avg >  busy_boost_pp)
                         ? boost_up_dwell + g_cfg.interval_ms : 0;

        // If the current state is outside the ladder (e.g. 0a, boot) → force a transition
        // to the lowest one (07) in the next decision step.
        if (g_cur_idx < 0) {
            // v4.1: init is a DOWN to 07 — GR-idle gated (init mid-render is risky).
            if (!g_cfg.dry && !gr_idle_ok(b)) {
                logf(2, "GR-idle gate: defer init -> 07 (busy=%d%% > %d%%)",
                     b / 10, g_cfg.gr_idle_promille / 10);
                continue;
            }
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(LADDER[0]) == 0) {
                    logf(1, "init: state outside the ladder -> %s", state_hex(LADDER[0]));
                    g_cur_idx = 0;
                    g_boost_active = false;
                    reset_after_transition();
                }
            } else {
                g_cur_idx = 0;
            }
            // v5.4: status — stan po init (deep_idle 07).
            if (g_cfg.dgpu.enable) {
                dgpu_state_str = dgpu_state_name(g_cur_idx, g_cfg.dgpu);
                sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
            }
            continue;
        }

        int target = g_cur_idx;
        const char* reason = nullptr;
        bool next_boost = g_boost_active;
        // v4.1: does this transition require a GR-idle before the write? DOWN (memory
        // clock drop) = true (wedge risk of GR mid-render); UP-LOAD/BOOST-UP = false
        // (a rising clock is safer; UP requires busy>80% so a gate would block it).
        bool needs_gr_idle = false;
        // v5.4: [dgpu-active] — dynamic ceiling/floor (for the verbose log).
        int dgpu_ceiling = -1, dgpu_floor = -1;

        // ---- v5.4: [dgpu-active] — three-stage state machine for the dGPU-ON ----
        // When enabled: DYNAMIC ceiling/floor for all applications (report 79
        // §3.6 + user decisions 2026-08-28). Boost (0f) disabled; title_pref does not
        // exist (title = INFORMATIONAL — does not decide 0e). 0e exclusively through
        // busy > busy-enter + a thermal margin. Branch order: WAKE → TERMAL →
        // IDLE → CEILING → UP-FLOOR → UP-LOAD (TERMAL > DOWN > UP, like the old
        // logic). States: DEEP_IDLE(07) / ACTIVE(baseline 0a) / HEAVY(max 0e).
        // Transitions always 1 level (never a 07→0e jump). The GR-idle gate protects
        // all DOWNs.
        if (g_cfg.dgpu.enable) {
            const int baseline_idx = state_to_idx(g_cfg.dgpu.baseline);
            const int max_idx      = state_to_idx(g_cfg.dgpu.max);

            // --- ceiling (dynamic, generalized) ---
            //   low-power focus (terminal) → low-power-ceiling (0a) — the background does
            //     not get 0e (user decision #7)
            //   busy > busy-enter ([caps] busy-up per-class) sustained → max (0e)
            //   otherwise → baseline (0a) — 0e EXCLUSIVELY busy-driven (user decision #2)
            int ceiling_dg;
            if (low_power_focused)
                ceiling_dg = state_to_idx(g_cfg.dgpu.low_power_ceiling);
            else if ((int)busy_avg > class_busy_up_pp)
                ceiling_dg = max_idx;
            else
                ceiling_dg = baseline_idx;
            if (ccap && ccap->max >= 0) {   // [caps] own ceiling — respect it
                int m = state_to_idx(ccap->max);
                if (m >= 0 && m < ceiling_dg) ceiling_dg = m;
            }
            if (ceiling_dg < 0) ceiling_dg = 0;

            // --- floor (dynamic, shared by all apps) ---
            //   activity (evdev input / fresh title / busy >= deep-idle-busy) →
            //     baseline (0a) — YouTube (~25-36% busy) holds 0a
            //   no activity for ≥ activity-dwell-ms AND busy < deep-idle-busy →
            //     07 (deep idle) — 480p (busy < 20%) may go to 07 (decision #3)
            //   [caps] floor = HARD minimum: floor = max(cap_floor, dynamic)
            bool floor_active = (last_activity_ts != 0) &&
                                no_input_ms < (uint64_t)g_cfg.dgpu.activity_dwell_ms;
            bool busy_above_wake = (int)busy_avg >= g_cfg.dgpu.deep_idle_busy * 10;
            int floor_dynamic;
            if (floor_active || busy_above_wake)
                floor_dynamic = baseline_idx;
            else if (no_input_ms >= (uint64_t)g_cfg.dgpu.activity_dwell_ms)
                floor_dynamic = 0;              // 07 — deep idle
            else
                floor_dynamic = baseline_idx;   // dwell not elapsed — hold baseline
            int floor = std::max(cap_floor, floor_dynamic);
            dgpu_ceiling = ceiling_dg;
            dgpu_floor = floor;

            // --- WAKE (W1): fast 07→0a path (≤1 s) after activity. Doesn't wait
            // for temp_low_dwell (responsiveness — user decision #4); gate: temp <
            // temp_down (one sample) — don't wake up during a thermal throttle.
            if (g_cur_idx < floor && floor >= baseline_idx &&
                temp >= 0 && temp < td) {
                target = g_cur_idx + 1;
                reason = "WAKE-ACTIVITY";
                needs_gr_idle = false;
            }
            // --- DOWN ---
            else if (temp >= 0 && temp_high_dwell >= g_cfg.temp_dwell_ms && g_cur_idx > 0) {
                target = g_cur_idx - 1;
                reason = "TERMAL";               // T1/T2 — overriding (may go below floor)
                needs_gr_idle = true;
            }
            // D2/D3 (IDLE): g_cur_idx > floor — descent to the floor. 0e→0a when
            // busy dropped (ceiling returned to baseline); 0a→07 when the floor dropped
            // after inactivity (no-input ≥ activity-dwell AND busy < deep-idle-busy).
            else if (g_cur_idx > 0 && idle_dwell >= g_cfg.idle_dwell_ms &&
                     g_cur_idx > floor) {
                target = g_cur_idx - 1;
                reason = "IDLE";
                needs_gr_idle = true;
            }
            else if (g_cur_idx > ceiling_dg) {
                target = g_cur_idx - 1;
                reason = "CEILING";
                needs_gr_idle = true;
            }
            // --- UP ---
            // UP-FLOOR: return to the floor (e.g. [caps] 0a after TERMAL; or 07→0a when
            // WAKE didn't pass due to temp). Requires temp_low_dwell (safe).
            else if (floor >= 0 && g_cur_idx < ceiling_dg && g_cur_idx < floor &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms) {
                target = g_cur_idx + 1;
                reason = "UP-FLOOR";
            }
            // W3 (UP-LOAD): busy > busy-enter sustained (80%; [caps] busy-up
            // per-class) AND temp < temp-up (thermal margin). The only way
            // to 0e. UP not GR-idle gated.
            else if (g_cur_idx < ceiling_dg && temp >= 0 &&
                     temp_low_dwell >= g_cfg.temp_dwell_ms &&
                     (int)busy_avg > class_busy_up_pp) {
                target = g_cur_idx + 1;
                reason = "UP-LOAD";
            }

            // dgpu-active doesn't use boost — if the daemon is on 0f (transition from
            // the non-dgpu-active mode after a SIGHUP reload), descend to the ladder.
            if (g_boost_active) {
                next_boost = false;
                target = std::min(ceiling_dg, LADDER_N - 1);
                reason = "DGPU-ACTIVE-EXIT-BOOST";
                needs_gr_idle = true;
            }

            // Status (written every poll_cycles via sw.tick → write_status).
            // video = INFORMATIONAL (from the title, user decision #4) — does not decide
            // the state; dgpu_state is chosen by busy. input_active = RAW evdev
            // input (diagnostic) — the floor uses the composite floor_active
            // (input + title change + busy ≥ deep-idle-busy).
            uint64_t ev_last = input.last_activity_ms();
            bool ev_active = (ev_last != 0) && now_ms > ev_last &&
                             (now_ms - ev_last) < (uint64_t)g_cfg.dgpu.activity_dwell_ms;
            dgpu_input_active = ev_active ? 1 : 0;
            dgpu_video = title_video ? 1 : 0;
            dgpu_state_str = (g_cur_idx >= 0 && g_cur_idx < LADDER_N)
                             ? dgpu_state_name(g_cur_idx, g_cfg.dgpu) : "off";
            sw.set_dgpu_active_status(dgpu_state_str.c_str(), dgpu_input_active, dgpu_video);
        } else {
        // ---- BOOST TIER (0f) — above the ladder, thermal guard prioritized ----
        if (g_boost_active) {
            // EXIT boost — TERMAL (IMMEDIATE, priority over load):
            //   temp > temp_up (58°C) OR temp >= temp_down (65°C) → drop from 0f.
            //   0f = the hottest pstate = first to be cut.
            if (temp >= 0 && (temp > prof.temp_up || temp >= prof.temp_down)) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1); // return to 0e (or lower)
                reason = "BOOST-TERMAL";
                needs_gr_idle = true;            // DOWN from 0f
            }
            // EXIT boost — CEILING: the profile lowered the cap below the ladder top
            // (e.g. preferred→default). Descend from 0f to the new ceiling.
            else if (g_cur_idx > ceiling) {
                next_boost = false;
                target = std::min(ceiling, LADDER_N - 1);
                reason = "BOOST-CEILING";
                needs_gr_idle = true;            // DOWN from 0f
            }
            // EXIT boost — LOAD hysteresis: busy < busy_up (80%) → drop from 0f.
            // Hysteresis enter@busy_boost(85) / exit@busy_up(80) = a 5 pp band.
            else if ((int)busy_avg < busy_up_pp) {
                next_boost = false;
                target = g_cur_idx; // == ceiling, return to 0e
                reason = "BOOST-IDLE";
                needs_gr_idle = true;            // DOWN from 0f
            }
            // else: boost hold — stay on 0f.
        }
        // ENTER boost — only when: boost_enabled AND at the ladder top (0e)
        // AND temp<temp_up for temp_dwell AND busy>busy_boost for boost_dwell
        // AND busy still >busy_boost (refresh). One entry condition, no skip.
        else if (boost_enabled &&
                 g_cur_idx == ceiling &&
                 temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms &&
                 boost_up_dwell >= prof.boost_dwell_ms &&
                 (int)busy_avg > busy_boost_pp) {
            next_boost = true;
            target = g_cur_idx; // g_cur_idx stays at ceiling; pstate=0f below
            reason = "BOOST-UP";
            // UP to 0f — NOT gated (requires busy>busy_boost; a rising clock).
        }

        // ---- LADDER (07↔0a↔0e ladder) — only while the boost is unchanged ----
        if (!g_boost_active && !next_boost) {
            // DOWN (priority: TERMAL > IDLE > CEILING). One level per step.
            // TERMAL works PER-PROFILE (default 65/58, preferred 82/75) —
            // the thermal guard is not skipped for non-preferred.
            if (temp >= 0 && temp_high_dwell >= g_cfg.temp_dwell_ms && g_cur_idx > 0) {
                target = g_cur_idx - 1;
                reason = "TERMAL";
                needs_gr_idle = true;            // DOWN — TERMAL may go BELOW the [caps] floor
            } else if (g_cur_idx > 0 && idle_dwell >= g_cfg.idle_dwell_ms &&
                       !title_pref &&
                       (cap_floor < 0 || g_cur_idx > cap_floor)) {
                // v4.3: !title_pref — suppress the IDLE downshift on a title-match.
                // v4.4: [caps] — IDLE stops at the floor (e.g. 0a); a class without a
                // floor (cap_floor<0) keeps the old behavior (IDLE to 07).
                target = g_cur_idx - 1;
                reason = "IDLE";
                needs_gr_idle = true;            // DOWN
            } else if (g_cur_idx > ceiling) {
                // The profile lowered the ceiling below the current state — descend a step
                // (the idle/thermal dwell handles the rest; a shortcut when the ceiling hard-
                // blocks the current level, e.g. default cap=07 while we are on 0a/0e).
                target = g_cur_idx - 1;
                reason = "CEILING";
                needs_gr_idle = true;            // DOWN
            }
            // UP (1 level, never a 07→0e jump).
            // v4.4: UP-FLOOR — a [caps] class returns to the resting floor
            // (e.g. 0a) without the busy-gate (no jump >1 here).
            // v5.9 fix: NO temp_low_dwell gate — in a hot chassis the dGPU right
            // after power-on sits at ~65-71°C ≥ temp-up (65) and the dwell never
            // grew → mpv after promotion hung on 07 (too slow). The floor is a
            // MINIMUM — we raise unconditionally; TERMAL (above) takes priority
            // and will descend back if needed.
            else if (cap_floor >= 0 && g_cur_idx < ceiling && g_cur_idx < cap_floor) {
                target = g_cur_idx + 1;
                reason = "UP-FLOOR";
            }
            // v4.3: title_pref bypasses busy_up (Discord/YouTube in the browser).
            // v4.4: [caps] has its own busy-up (e.g. 50%) instead of the global one (80%).
            // UP stays NOT GR-idle gated (a rising clock is safer).
            else if (g_cur_idx < ceiling &&
                     temp >= 0 && temp_low_dwell >= g_cfg.temp_dwell_ms &&
                     ((int)busy_avg > class_busy_up_pp || title_pref)) {
                target = g_cur_idx + 1;
                reason = title_pref ? "UP-TITLE" : "UP-LOAD";
            }
        }
        }

        // ---- EXECUTION ----
        const bool boost_changed = (next_boost != g_boost_active);
        // Source state for the log: 0f if boost active, otherwise LADDER[g_cur_idx].
        auto cur_hex = [&]() -> const char* {
            return g_boost_active ? state_hex((uint32_t)prof.boost_pstate)
                                  : state_hex(LADDER[g_cur_idx]);
        };

        // v4.1: GR-idle gate for DOWN transitions. If the GR engine is busy
        // (busy > gr_idle_promille) — defer the write by one cycle. Do NOT reset
        // the dwell counters / g_cur_idx / g_boost_active (defer ≠ executed transition;
        // the dwell keeps growing, the transition executes when busy valleys
        // between frames). UP-LOAD/BOOST-UP have needs_gr_idle=false → not deferred.
        const bool defer = needs_gr_idle && !gr_idle_ok(b);
        if (defer) {
            const char* tgt = next_boost ? state_hex((uint32_t)prof.boost_pstate)
                             : (target >= 0 && target < LADDER_N
                                ? state_hex(LADDER[target]) : "?");
            logf(2, "GR-idle gate: defer %s %s->%s (busy=%d%% > %d%%)",
                 reason ? reason : "?", cur_hex(), tgt,
                 b / 10, g_cfg.gr_idle_promille / 10);
            continue;
        }

        if (boost_changed) {
            uint32_t write_state = next_boost
                ? (uint32_t)prof.boost_pstate            // entry: 0f
                : LADDER[std::max(0, std::min(target, LADDER_N - 1))]; // exit: 0e/lighter
            float busy_pct = busy_avg / 10.0f;
            logf(1, "%s BOOST pstate change: %s -> %s (busy=%.0f%%, temp=%d°C, %s, profile=%s)",
                 g_cfg.dry ? "dry-run:" : ">>",
                 cur_hex(), state_hex(write_state),
                 busy_pct, temp, reason ? reason : "?",
                 pref_active ? "preferred" : "default");
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(write_state) == 0) {
                    g_boost_active = next_boost;
                    if (!next_boost) g_cur_idx = std::max(0, std::min(target, LADDER_N - 1));
                    // boost entry: g_cur_idx stays at ceiling (0e)
                    reset_after_transition();
                } else {
                    logf(0, "ERROR: pstate (boost) write failed");
                }
            } else {
                g_boost_active = next_boost;
                if (!next_boost) g_cur_idx = std::max(0, std::min(target, LADDER_N - 1));
                reset_after_transition();
            }
        } else if (!next_boost && target != g_cur_idx &&
                   target >= 0 && target < LADDER_N) {
            float busy_pct = busy_avg / 10.0f;
            if (g_cfg.dgpu.enable)
                logf(1, "%s dgpu-active: %s -> %s (busy=%.0f%%, temp=%d°C, %s, "
                        "profile=%s, input=%d, video=%d)",
                     g_cfg.dry ? "dry-run:" : ">>",
                     dgpu_state_name(g_cur_idx, g_cfg.dgpu),
                     dgpu_state_name(target, g_cfg.dgpu),
                     busy_pct, temp, reason ? reason : "?",
                     pref_active ? "preferred" : "default",
                     dgpu_input_active, dgpu_video);
            else
                logf(1, "%s pstate change: %s -> %s (busy=%.0f%%, temp=%d°C, %s, profile=%s)",
                     g_cfg.dry ? "dry-run:" : ">>",
                     state_hex(LADDER[g_cur_idx]), state_hex(LADDER[target]),
                     busy_pct, temp, reason ? reason : "?",
                     pref_active ? "preferred" : "default");
            if (!g_cfg.dry) {
                if (g_cfg.vblank_sync) drm_vblank_wait();
                if (set_pstate(LADDER[target]) == 0) {
                    g_cur_idx = target;
                    reset_after_transition();
                } else {
                    logf(0, "ERROR: pstate write failed");
                }
            } else {
                g_cur_idx = target;
                reset_after_transition();
            }
        } else if (g_cfg.verbosity >= 2) {
            float busy_pct = busy_avg / 10.0f;
            if (g_cfg.dgpu.enable) {
                logf(2, "dgpu-active state %s, busy=%.0f%%, temp=%d°C, ceiling=%d, "
                        "floor=%d, input=%d, video=%d, noinput=%llu ms, "
                        "dwell[low=%d hi=%d idle=%d]",
                     dgpu_state_name(g_cur_idx, g_cfg.dgpu),
                     busy_pct, temp, dgpu_ceiling, dgpu_floor,
                     dgpu_input_active, dgpu_video,
                     (unsigned long long)no_input_ms,
                     temp_low_dwell, temp_high_dwell, idle_dwell);
            } else {
                logf(2, "state %s, busy=%.0f%%, temp=%d°C, profile=%s, ceiling=%d, "
                        "boost=%s, dwell[low=%d hi=%d idle=%d bu=%d] pref=%d",
                     g_boost_active ? state_hex((uint32_t)prof.boost_pstate)
                                    : state_hex(LADDER[g_cur_idx]),
                     busy_pct, temp,
                     pref_active ? "pref" : "def", ceiling,
                     g_boost_active ? "ON" : (boost_enabled ? "off" : "-"),
                     temp_low_dwell, temp_high_dwell, idle_dwell,
                     boost_up_dwell, pref_dwell);
            }
        }
    }
    } catch (const std::exception& e) {
        logf(0, "reclocked: exception (%s) — clean exit (restore: pstate + "
                "SMC auto fans)", e.what());
    } catch (...) {
        logf(0, "reclocked: unknown exception — clean exit (restore: "
                "pstate + SMC auto fans)");
    }

    restore();
    logf(1, "exit.");
    return 0;
}