# gpu/ — nouveau stack for the NVIDIA GT 750M (Kepler GK107M)

## What it does

A complete, self-maintained graphics stack for the **NVIDIA GT 750M Mac
Edition** dGPU (GK107M, PCI `10de:0fe9`, Kepler sm_30, 2 GB) on
Arch/Omarchy with the `-omnkp` 7.1.9 kernel under nouveau: the `reclocked`
daemon (auto-reclock pstate 07/0a/0e with an app-aware profile, applesmc
fans, thermal gate, dGPU power "switchd"), kernel + Mesa patches
(auto-reclock, sm_30 NAK latencies in the nvc0 scheduler) and tooling
(pstate, benchmarks, recovery, gmux). The hybrid's iGPU is the **Intel Iris
Pro 5200** (`8086:0d26`, i915); switching happens via
`apple_gmux`/vgaswitcheroo.

## Problem solved

**Symptom:** after boot nouveau stays on the boot clocks **405/680 MHz**
and never raises them (pstate works manually — `07/0a/0e/0f` from the VBIOS
table, max core 925 MHz, max mem 5016 MHz) → the dGPU desktop is visibly
slow; Mesa/nvc0 ships pessimistic latencies.

**Root cause:** upstream nouveau has the full pstate-change pipeline
(`nvkm_pstate_calc` → worker → `nvkm_pstate_prog`; the `ustate == -2`
mechanism designed for auto policy and used by Tegra DVFS) but **no load
measurement for desktops** (perfmon was removed upstream) — only AC/DC
states are switched by the ACPI notifier; Mesa ships generic latencies
instead of NAK sm_30; 470xx was tested and **failed outright**.

## Contents

`reclocked/` — daemon (C++17, zero deps, built with `make`):
`reclocked.cpp` (busy measurement via idle PMU counters in BAR0, 1:1
semantics with `gk20a_devfreq.c`; 07↔0a↔0e ladder with 0f boost;
app-aware profiles from Hyprland; applesmc fan curve; self-heal after S3;
switchd = dGPU power), `reclocked.conf` (copy to `/etc/reclocked.conf`;
sections `[preferred]`, `[caps]`, `[low-power]`, profiles, `[dgpu-active]`,
`[fan]`, `[switch]`; SIGHUP = re-read), `reclocked.service`
(anti-crash-loop: RestartSec=5, StartLimit 3/120 s — **not installed,
user's decision**), `reclockctl` (wrapper: `start|stop|status|restart|
reload|logs|fan-on|fan-off|switch-status|dgpu-on|dgpu-off|dgpu-auto|
to-igd|to-dis`), `Makefile`, plus **`omarchy/reclockbar.cpp` +
`build-bar-tools.sh`** (status-bar helper for Hyprland: fan floor, dGPU
override, watch).

`patches/` — `kernel/0001-nouveau-auto-reclock.patch` (ondemand policy in
`clk/base.c`: PMU counters, 45/30% thresholds, ≥90% boost, 90/95 °C
thermal gate, `nvkm_clk_astate`; active only with `NvAutoClk=1` or
`echo auto`), `mesa/0002-mesa-nvc0-sched-data.patch` (sm_30 NAK latencies
→ nvc0 SchedDataCalculator: IMul/carry-out 13, EXIT 15, sched default 16,
TEX 17, MEMBAR 16; worst case = stall, not crash), `reclocked/…` (daemon
`[caps]` policy patches).

`scripts/`: `pstate.sh` (manual pstate via debugfs `status`/`set 0a`/
`set ac:0a`/`auto`; `set` writes an override `/run/reclocked/override`),
`reclocked-rebuild.sh` (rebuild, zero-warnings rule + systemd restart +
status; `--reload` = SIGHUP only), `bench-gpu.sh` (glmark2 iGPU vs dGPU,
full-res, offscreen), `mesa-manage.sh` / `build-mesa.sh` (install/rollback
of patched Mesa — `nouveau_dri.so` → `nouveau_dri_patched.so` — and its
build; does not install system-wide), `install-kepler-cuda.sh` (CUDA
10.2 / 470xx — **abandoned**), `recover-gpu.sh` (revert
AQ_DRM_DEVICES/gpu-switch over SSH/TTY), **`nvram-test.sh`** (three-layer
GPU switch tester: NVRAM/gmux/pstate).

`gmux-io/` — `gmux-io.c` + `Makefile`: read/write apple-gmux registers
(INDEXED type, PIO base 0x700) via `/dev/port`; writing = live
switch/power.

`verify/` — CUDA verification programs (`check_cuda.cu`, `vecadd.cu`,
`bandwidth.cu`, `verify.sh`, sm_30) — **stale** without the `nvidia` module
(nouveau has no CUDA).

## Use

1. Kernel with patch `0001-nouveau-auto-reclock` (see `../kernel/`).
2. `sudo cp reclocked/reclocked.conf /etc/reclocked.conf`
3. `sudo cp reclocked/reclocked.service /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable --now reclocked`
4. Control: `reclockctl status|logs|reload`; manual: `pstate.sh set 0f`.
5. Daemon changes: `reclocked-rebuild.sh`; Mesa: `build-mesa.sh` →
   `mesa-manage.sh install` (rollback: `restore`).

## Warnings

- **pstate settings are runtime-only — a reboot resets** to 405/680 MHz
  (hence the daemon). The daemon needs root; a manual `pstate.sh set`
  freezes the daemon (override), `pstate.sh auto` restores auto;
  `bench-gpu.sh` stops the daemon for the measurement (otherwise results
  are skewed).
- **0e/0f = aggressive memory reclock** (4000/5016 MHz) — test via the
  ladder (07 → 0a → 0e); 0f is excluded from the auto ladder — history of
  hangs under clock management.
- After a gmux/NVRAM switch (`reclockctl to-igd|to-dis`) a **reboot** is
  required; black screen after experiments: `recover-gpu.sh` over SSH/TTY.