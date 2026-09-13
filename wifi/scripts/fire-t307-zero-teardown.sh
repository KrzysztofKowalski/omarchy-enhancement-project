#!/usr/bin/env bash
# Fire T307 (zero-teardown hold, experiment #2 variant A).
#
# Recreates the fire #2 conditions (T276 shared_info poison magic + T277
# console decode + T278 periodic console ladder) and adds T307: on the
# fw-init timeout the fail path NO LONGER tears down (no
# device_release_driver -> no wedge window). Instead: MAILBOXMASK
# (BAR0+0x4C):=0, poll BAR0+0x48 (PCIMailBoxInt) every 1 s for 60 s with
# per-read logging, then a SUMMARY. Probe returns success, the device
# stays bound to the driver, module stays loaded.
#
# T307 answers: does the fw signal D2H (bit 8) at all via the mailbox
# status register, independently of the mask? (PCIMailBoxInt latches
# status; the mask only gates the IRQ line — poll is passive and safe.)
#
# Substrate prerequisites (per CLAUDE.md pre-test checklist):
#   - rebooted into the clean boot config (wl blacklisted, mitigations restored)
#   - lspci -vvv -s 03:00.0 shows MAbort-, CommClk+
#   - brcmfmac.ko built with T307 (rebuild if not)
#
# Usage: sudo scripts/fire-t307-zero-teardown.sh
# NOTE: no rmmod after the fire — rmmod = teardown = wedge risk
# (experiment #1). The module stays loaded, the device stays bound to the
# driver; a reboot clears the state.

set -e

WORK_DIR="$(cd "$(dirname "$0")" && pwd)"
FMAC_DIR="$WORK_DIR/../build/brcm80211/brcmfmac"
LOG_DIR="$WORK_DIR/../logs"
PCI_DEV="03:00.0"

LOG="$LOG_DIR/test.307.journalctl.txt"

echo "=== Fire T307 (zero-teardown, variant A) ==="
echo "Log: $LOG"
echo ""

# Sanity: brcmfmac.ko must exist + have the T307 param
for mod in brcmfmac.ko wcc/brcmfmac-wcc.ko; do
    if [ ! -f "$FMAC_DIR/$mod" ]; then
        echo "ERROR: $FMAC_DIR/$mod not found — run make first"
        exit 1
    fi
done

if ! modinfo "$FMAC_DIR/brcmfmac.ko" | grep -q 'bcm4360_test307_zero_teardown'; then
    echo "ERROR: brcmfmac.ko missing T307 param — rebuild required"
    exit 1
fi
echo "Module has T307 param: OK"

# Substrate check
echo ""
echo "=== PCIe state ==="
lspci -vvv -s "$PCI_DEV" 2>/dev/null | grep -E 'MAbort|CommClk|LnkSta|LnkCtl' || true

echo ""
echo "=== Cmdline (verify mitigations=off NOT present) ==="
grep -oE 'mitigations=[a-z]+' /proc/cmdline || echo "  mitigations: default (good)"

# Unbind any existing driver from BCM4360 (a later insmod after the previous
# test needs an unbind, because the device stays bound to the driver)
if [ -e "/sys/bus/pci/devices/0000:$PCI_DEV/driver" ]; then
    CURRENT=$(basename "$(readlink /sys/bus/pci/devices/0000:$PCI_DEV/driver)")
    echo "Unbinding $CURRENT from $PCI_DEV..."
    echo "0000:$PCI_DEV" > "/sys/bus/pci/devices/0000:$PCI_DEV/driver/unbind" 2>/dev/null || true
    sleep 1
fi

# Remove any loaded brcmfmac
if lsmod | grep -q brcmfmac; then
    echo "Removing loaded brcmfmac stack..."
    rmmod brcmfmac-wcc 2>/dev/null || true
    rmmod brcmfmac-cyw 2>/dev/null || true
    rmmod brcmfmac-bca 2>/dev/null || true
    rmmod brcmfmac 2>/dev/null || true
    sleep 1
fi

# Pre-test BAR0 MMIO check (CTO vs UR distinguisher — see test-brcmfmac.sh comments)
echo ""
echo "Pre-test: BAR0 MMIO probe..."
T_START=$(date +%s%3N)
set +e
dd if=/sys/bus/pci/devices/0000:$PCI_DEV/resource0 bs=4 count=1 of=/dev/null 2>/dev/null
DD_EXIT=$?
set -e
T_END=$(date +%s%3N)
T_MS=$((T_END - T_START))

if [ $DD_EXIT -eq 0 ]; then
    echo "BAR0 MMIO OK — device responding."
elif [ $T_MS -lt 40 ]; then
    echo "BAR0 MMIO: UR (${T_MS}ms) — alive, SBR will fix. Proceeding."
else
    echo "FATAL: BAR0 MMIO CTO (${T_MS}ms). Recover via battery drain."
    exit 1
fi

# Fire
echo ""
echo "=== Loading brcmfmac with T276 + T277 + T278 + T307 ==="
dmesg -C  # Clear kernel log for clean capture

modprobe brcmutil 2>/dev/null || true
modprobe cfg80211 2>/dev/null || true
# brcmfmac.ko also links the SDIO/USB backends (forced-config.mk) — without
# mmc_core, insmod fails on "Unknown symbol" (sdio_*/mmc_*) [fix 34fdf8e]
modprobe mmc_core 2>/dev/null || true

# Param choice rationale:
#   test276=1 — shared_info poison magic (0xA5A5A5A5) -> fw-init timeout —
#               exactly the fire #2 conditions T307 is meant to catch
#   test277=1 — console decoder (precondition for T278)
#   test278=1 — periodic console reads t+500ms..t+90s (fw wake observation)
#   test307=1 — THE PROBE: zero-teardown hold + 60s poll of BAR0+0x48
# Explicitly NOT setting:
#   test298=0 — ISR-list walk (baseline-only; omitted to minimize TCM probes
#               in the hold window; fire #2 data already covers it)
#   test305=0 — would write MBM pre-set_active; T307 writes its own MBM:=0
#               in the hold — no interference wanted
#   test306=0 — cfg dump is read-only but adds 48 dwords x3 of config reads;
#               fire #2 data already covers it
echo "insmod brcmfmac.ko \\"
echo "    bcm4360_test276_shared_info=1 \\"
echo "    bcm4360_test277_console_decode=1 \\"
echo "    bcm4360_test278_console_periodic=1 \\"
echo "    bcm4360_test307_zero_teardown=1"

insmod "$FMAC_DIR/brcmfmac.ko" \
    bcm4360_test276_shared_info=1 \
    bcm4360_test277_console_decode=1 \
    bcm4360_test278_console_periodic=1 \
    bcm4360_test307_zero_teardown=1

insmod "$FMAC_DIR/wcc/brcmfmac-wcc.ko"

echo ""
echo "Modules loaded. Sleeping 180s for full ladder + wait + T307 poll..."
# Total time (from the pcie.c code):
#   attach + download fw 442KB (iowrite32) + NVRAM     ~2-5s
#   T276 2s poll                                       2s
#   T277 decode                                        <1s
#   T278 ladder: 0.5 + 4.5 + 25 + 60 =                 90s
#   fw-init wait timeout (BRCMF_PCIE_FW_UP_TIMEOUT)    5s
#   T307 poll: 60 x 1s                                 60s
#   -------------------------------------------------------
#   total                                         ~160-165s -> 180s (headroom)
sleep 180

# Capture
echo ""
echo "=== Capturing dmesg → $LOG ==="
dmesg > "$LOG"

# Quick highlight of the key SUMMARY lines
echo ""
echo "=== T307 SUMMARY ==="
grep -E 'BCM4360 test\.307:.*SUMMARY' "$LOG" || echo "  (no T307 SUMMARY found — check if the hold ran)"

echo ""
echo "=== T307 ENTER + MAILBOXMASK readback ==="
grep -E 'BCM4360 test\.307: (ENTER|MAILBOXMASK)' "$LOG" || echo "  (no T307 ENTER found)"

echo ""
echo "=== T307 bit8 transitions (any changed=1 reads) ==="
grep -E 'BCM4360 test\.307:.*changed=1' "$LOG" || echo "  (no T307 changed reads — mbxint stayed constant)"

echo ""
echo "=== Console wr_idx at end-of-ladder ==="
grep -E 'wr_idx=[0-9]+' "$LOG" | tail -3

echo ""
echo "=== T276 fw_init_done / mbxint poll-end ==="
grep -E 'test\.276: poll-end' "$LOG" || echo "  (no T276 poll-end found)"

echo ""
echo "Full log: $LOG ($(wc -l < "$LOG") lines)"
echo ""
echo "=== END OF FIRE: no rmmod ==="
echo "The module must stay loaded, the device bound to the driver (probe=0)."
echo "rmmod = teardown = wedge risk (experiment #1). After the test: reboot."
echo "Module state (informational):"
lsmod | grep brcm || echo "  (brcmfmac unloaded — something went wrong, device_release_driver may have run)"
