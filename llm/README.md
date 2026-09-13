# llm/ — Haswell-free ollama build as a Claude Code gateway

Builds [ollama](https://github.com/ollama/ollama) v0.33.0-rc2 from source
with the Haswell-class CPU requirements disabled (AVX2/FMA/F16C/BMI2) —
the binary runs on any AVX-capable CPU (Sandy Bridge+). Not for local
inference: it acts as a gateway for Claude Code (Claude Code → ollama →
ollama.com cloud models, authenticated via `ollama signin`).

## Why

Stock ollama builds ship AVX2 kernels and crash with "haswell" errors on
older CPUs (e.g. the MacBook Pro 11,3 / i7-4850HQ). This build disables
those flags.

## Environment the gateway sets (`cmd/launch/claude.go`)

`ollama launch claude` sets these env vars for Claude Code:

| Variable | Value | Purpose |
|---|---|---|
| `CLAUDE_CODE_MAX_OUTPUT_TOKENS` | 64000 | max output tokens per response |
| `CLAUDE_CODE_MAX_CONTEXT_TOKENS` | 1048576 | model context window — lifts Claude Code's default 200K cap for unknown models |
| `CLAUDE_CODE_AUTO_COMPACT_WINDOW` | 1048576 | auto-compact at 1M (cloud models) |

Claude Code never reads the context length from the API — it has a
hardcoded per-model table and defaults unknown models to 200K.
`CLAUDE_CODE_MAX_CONTEXT_TOKENS` is the documented override (requires
Claude Code ≥ 2.1.193).

## Build

```bash
./build-ollama.sh          # patches the source, builds, installs to ~/.local
ollama signin              # authenticate the gateway account(s)
ollama launch claude       # start Claude Code through the gateway
```

Requirements: go ≥ 1.26, cmake, ninja.

## scripts/

- `ollama-account` — switch/manage the gateway account
- `ollama-autostart` — start the gateway at login
- `ollama-ui` — status helper for the desktop
- `ollama-usage` / `ollama-usage-guard` — usage counters / hard guard for
  the shared account

## Status

Working prototype on Omarchy (2026-09). The env-var override for the 1M
window requires a recent Claude Code; older versions fall back to 200K
gracefully.