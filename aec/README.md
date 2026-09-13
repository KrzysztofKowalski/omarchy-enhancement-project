# aec/ — on-demand PipeWire AEC backend (NLMS, AVX2/FMA)

App-agnostic acoustic echo cancellation for PipeWire 1.6.x, replacing the
stock webrtc AEC3 backend. The DSP core is AVX2+FMA with a runtime dispatch
(scalar fallback). **Runs only on demand** — zero background cost.

## Architecture

- **On-demand**: the module is NOT loaded into PipeWire. `aec.sh start`
  launches `aec-host` — a tiny client process that loads the native
  `module-echo-cancel` (all its paths are `pw_stream`, so they plug into the
  LIVE graph — no PipeWire restart). `aec.sh stop` = kill the host → the
  nodes disappear, 0 CPU.
- **monitor.mode = true**: the echo reference is the monitor of the
  **default** sink — playback can go to any device (BT speaker, built-in)
  and is still removed from the mic. Apps choose their output freely.
- Mic for apps: `echo_cancel_source` ("Mikrofon (echo-cancel)",
  priority.session 5000 → default input while enabled).

Backend: NLMS (80 ms tail) + delay estimator (whitened cross-correlation,
42–600 ms, tick every 0.5 s) + Geigel DTD. Gates: silence on the far end →
FIR/estimator skipped (recording without music = 0% CPU). No NS/AGC — a
noise floor can be added separately as an rnnoise filter chain.

## Files

- `aecn.h` — DSP core (dot/axpy/maxabs: scalar + AVX2/FMA, dispatch)
- `libspa-aec-avx.c` — SPA wrapper (modeled on spa/plugins/aec/aec-null.c)
- `aec-host.c` — client process hosting the module on the live graph
- `aec.sh` — `start [avx|webrtc] | stop | status`
- `use-aec.sh` — wrapper (avx|webrtc|off)
- `bench.c` — scalar vs AVX2 benchmark on synthetic echo (ERLE + %CPU)
- `install.sh` — build + install into `~/.local/lib/spa-0.2` + `~/.local/bin`
- `echo-cancel.autostart.conf.example` — optional return to always-on

## Install

    ./install.sh          # build, install (user-local, no sudo), restart PW
    aec.sh start          # before a call (e.g. via a shell alias)
    aec.sh stop           # after the call

The plugin loads thanks to `SPA_PLUGIN_DIR` (systemd drop-ins for
pipewire.service and pipewire-pulse.service → `~/.local/lib/spa-0.2`).
`aec-host` sets the variable itself on start from `aec.sh`.

## Results (i7-4850HQ, Haswell, AVX2+FMA, 48 kHz stereo)

- benchmark: scalar 14.0% vs AVX2/FMA 4.6% core (3× faster), 200.2 ms delay
  found (corr 0.50)
- live: **24.7–28.7 dB echo attenuation** (440 Hz tone on the BT speaker,
  raw mic vs EC)
- CPU (one core, i7-4850HQ = 8 threads):
  - AEC off: **0%** (nothing exists — nothing to measure)
  - mic recording, no music: **0%** (far-end silence gate skips the FIR)
  - music + active mic (conversation): ~9% (DSP ~4.6% + stream transport)
  - idle PipeWire without EC: ~0.1–1%

## Configuration

AEC args in `aec.sh` (`AECARGS`): `tail_ms=80 delay_max_ms=600 mu=0.35`
- `tail_ms` — adaptive FIR tail length (cost ∝ tail)
- `delay_max_ms` — delay search range (BT ≈ 200 ms)
- `mu` — NLMS step (0.2–0.5)

Related Omarchy system configs:
- `~/.config/wireplumber/wireplumber.conf.d/70-no-hfp-autoswitch.conf`
  (BT autoswitch to the HFP profile disabled)
- `~/.config/wireplumber/wireplumber.conf.d/71-builtin-priority.conf`
  (built-in sound above the USB PCM2912A)