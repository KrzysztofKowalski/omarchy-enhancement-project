# Contributing

This repository collects enhancements for **Omarchy on unsupported-ish
hardware** (MacBook Pro Late 2013) — open drivers, configs and local fixes.

## Ground rules

- **English only** for user-facing content (READMEs, docs, comments, echo
  output). Polish working notes live outside the repo (they were not
  published for a reason).
- **No private data**: no absolute user paths (`/home/<user>`), hostnames,
  MACs, machine-id/PARTUUID/resume offsets or local IPs. Use
  `$HOME`/placeholders — the repo is public-surface.
- **No redistribution of proprietary blobs**: firmware/Broadcom content is
  extracted at runtime by the owner; never commit such binaries.
- Keep upstream licenses: derived themes/code keep their
  `LICENSE-upstream-*` attribution files.

## Per-module sanity

- Shell: `bash -n` / `shellcheck` before submitting.
- Python: `python -m py_compile`.
- C/C++: `gcc -fsyntax-only` (or note the missing headers).
- Images: WebP, q88, native panel resolution (2880×1800).

## Working on hardware

Bring-up modules (wifi, gpu) involve real hardware sessions. See the
module README **Warnings** sections — some operations can black-screen the
machine or wedge the PCIe bus. Prefer dry-run/read-only paths first and
follow the reboot discipline.