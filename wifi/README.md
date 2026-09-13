# wifi/ — the BCM4360 (14e4:43a0) on our own brcmfmac driver

MacBook Pro Late 2013. The card is a **Broadcom BCM4360** 802.11ac,
`[14e4:43a0]` (rev 03, Apple subsystem). Omarchy preinstalls the proprietary
`wl` (broadcom-wl 6.30.223.271) — unmaintained, the kernel warns about it
at boot, it mishandles 802.11v (BSS Transition) and symptomatically drops
the 5 GHz connection; also `wl.ko` only exists for the stock kernel — on
our own (nvkp/omnkp) kernel Wi-Fi does not start at all because the module
is missing.

## Goal and root cause

Goal: replace `wl` with the open **brcmfmac** plus our patch series.
Upstream mainline does not support the 4360 over PCIe (no `43a0` ID, no
firmware entry, `pcie.c` is msgbuf-only). Root cause: the 4360 firmware
("RTE (PCI-CDC) 6.30.223") does not speak msgbuf — it never publishes the
sharedram pointer in `TCM[ramsize-4]`, so the brcmfmac PCIe handshake always
ends in -ENODEV. The blob's real protocol is CDC over an olmsg ring in host
RAM plus PCIE2 mailbox doorbells — a transport mainline never had.

## What is in this directory

- `patches/` — series (0001–0020 and 0036–0039) for brcmfmac: chip/device
  IDs and firmware mapping, TCM rambase/raminfo, strict 32-bit writes when
  downloading firmware (64-bit `memcpy_toio` hangs the PCIe bus),
  INTERNAL_MEM guard, bring-up instrumentation (T276/T277/T278/T298/
  T305–T320 module params, default 0, gated on chip == 4360), the PCI-CDC
  (olmsg) transport, and the final integration patches (0036–0039:
  zeroing/contract vars, the OLM-WIFI semaphore/DMA gate, and the
  omwifi-bridge module hooks).
- `scripts/build-module.sh` — regenerates the `build/` tree from kernel
  sources, inserts the forced config, applies patches one by one and
  builds modules out-of-tree. **Deprecated for use** — module builds moved
  to `../kernel/scripts/build-om-wifi.sh` (same rule, against the omnkp
  tree); kept for reference.
- `scripts/forced-config.mk` — forced Makefile config block (mirrored from
  the kernel `.config` + explicit `-DDEBUG`); without it DEBUG does not
  propagate out-of-tree and `debug.c` collides with the stubs.
- `scripts/test-brcmfmac.sh` — manual test of the patched module.
- `scripts/fire-t30*.sh` — hardware experiment runners (pre-flight PCIe,
  insmod with the test params, sleep, dmesg dump) — for deliberate test
  sessions only.
- `tools/extract_firmware.py` — extracts the firmware blob
  (`dlarray_4352pci`, 442233 B) from your own `wl.ko`; the blob is not in
  linux-firmware and is Broadcom IP — everyone extracts it themselves.
- `tools/scan_fw_magic.py` — static scan of the firmware binary for magic
  32-bit constants (LE/BE).
- `firmware/brcmfmac4360-pcie.txt` — the card's NVRAM (boardtype, ccode
  `X0`, devid 43a0), installed to `/lib/firmware/brcm/`.

## Quick use

1. `kernel/scripts/build-om-wifi.sh` (builds against the booting kernel's
   vermagic — mirrors `build-module.sh` for the omnkp tree).
2. `wl` blacklisted; firmware `.bin` + `.txt` in `/lib/firmware/brcm/`.
3. Plain test without experiments: `sudo scripts/test-brcmfmac.sh`.
4. Fires (`fire-t30*.sh`) only in a deliberate session.

## Warnings

- **This is not yet a working driver** — the bring-up campaign is ongoing;
  the last prepared fire (T308, olmsg transport) has not been executed.
- Writes to config space / BAR0 can wedge the host with no log trace (SMC
  reset / 10 s power-key); a wired Ethernet cable is mandatory.
- **Iron rule: never touch the wired Ethernet (`enp12s0`, igb)** — it is
  the session channel; no rmmod, no interface/route changes.
- After each on-hardware test: **reboot** (never `rmmod` — teardown is a
  wedge window); only a restart clears the device state.
- Firmware from `wl.ko` is Broadcom property — do not redistribute.