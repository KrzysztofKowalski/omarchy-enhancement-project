#!/usr/bin/env bash
# Fire T308 (phase 3: first dcmd WLC_GET_VERSION over the olmsg transport).
#
# Variant C (PCI-CDC): brcmfmac with the olmsg transport (patch 0008) instead of
# msgbuf — phase 1 (sharedram-ptr handshake workaround, proto_type=BCDC)
# + phase 2 (txctl/rxctl via the T276 ring, doorbell H2D BAR0+0x48|=0x1,
# D2H bit 8, mask 0x4C|=0x100 per cycle). Phase 3 = first dcmd:
# WLC_GET_VERSION → if the fw answers → version in the logs and wiphy in the system.
#
# Parameters (per notes/wl-olmsg-transport.md §4.1):
#   test276=1 — shared_info handshake + 64 KB ring buffer (REQUIRED)
#   test277=1 — fw console decoder (prerequisite for test278)
#   test278=1 — periodic fw console reads t+500ms..t+90s (observe
#               wr_idx — whether the fw wakes up after the doorbell)
#   test308=1 — THE PROBE: olmsg transport (phases 1+2+3)
#   debug=0x7fffffff — brcmf_dbg(PCIE) visible in dmesg (doorbell/ctl resp)
# Deliberately NOT setting test307 — its hold writes MAILBOXMASK:=0
# (disables the D2H IRQ) and only makes sense on the msgbuf timeout path,
# which the olmsg variant does not take.
#
# Pre-flight (per the discipline after the 30.08 incident): lspci LnkSta/MAbort
# first, then a timed BAR0 MMIO probe — CTO (>=40 ms) = DO NOT FIRE,
# recovery by draining the battery to zero.
#
# Usage: sudo scripts/fire-t308-olmsg-cdc.sh
# NOTE: after the test we do NOT rmmod — rmmod = teardown = wedge risk.
# The module stays loaded; reboot clears the state.

set -e

WORK_DIR="$(cd "$(dirname "$0")" && pwd)"
FMAC_DIR="$WORK_DIR/../build/brcm80211/brcmfmac"
LOG_DIR="$WORK_DIR/../logs"
PCI_DEV="03:00.0"

LOG="$LOG_DIR/test.308.journalctl.txt"

echo "=== Fire T308 (olmsg transport, phase 3: first dcmd) ==="
echo "Log: $LOG"
echo ""

# Sanity: brcmfmac.ko + wcc must exist and carry the T308/T276 parameters
for mod in brcmfmac.ko wcc/brcmfmac-wcc.ko; do
    if [ ! -f "$FMAC_DIR/$mod" ]; then
        echo "ERROR: $FMAC_DIR/$mod not found — run make first"
        exit 1
    fi
done

for parm in bcm4360_test308_olmsg_cdc bcm4360_test276_shared_info; do
    if ! modinfo "$FMAC_DIR/brcmfmac.ko" | grep -q "$parm"; then
        echo "ERROR: brcmfmac.ko missing $parm — rebuild required"
        exit 1
    fi
done
echo "Module params (test308/test276): OK"

# Substrate check
echo ""
echo "=== PCIe state ==="
lspci -vvv -s "$PCI_DEV" 2>/dev/null | grep -E 'MAbort|CommClk|LnkSta|LnkCtl' || true

echo ""
echo "=== Cmdline (verify mitigations=off NOT present) ==="
grep -oE 'mitigations=[a-z]+' /proc/cmdline || echo "  mitigations: default (good)"

# Unbind any existing driver from BCM4360 (a follow-up insmod after the previous
# test requires an unbind, because the device stays bound to the driver)
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

# Pre-test BAR0 MMIO check (CTO vs UR distinguisher — per discipline)
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
    echo "BAR0 MMIO OK — device responding (${T_MS}ms)."
elif [ $T_MS -lt 40 ]; then
    echo "BAR0 MMIO: UR (${T_MS}ms) — alive. Proceeding."
else
    echo "FATAL: BAR0 MMIO CTO (${T_MS}ms). Recover via battery drain."
    exit 1
fi

# Fire
echo ""
echo "=== Loading brcmfmac with T276 + T277 + T278 + T308 ==="
dmesg -C  # Clear kernel log for clean capture

modprobe brcmutil 2>/dev/null || true
modprobe cfg80211 2>/dev/null || true
# brcmfmac.ko also links the SDIO/USB backends (forced-config.mk) — without
# mmc_core the insmod fails with "Unknown symbol" (sdio_*/mmc_*) [fix 34fdf8e]
modprobe mmc_core 2>/dev/null || true

echo "insmod brcmfmac.ko \\"
echo "    bcm4360_test276_shared_info=1 \\"
echo "    bcm4360_test277_console_decode=1 \\"
echo "    bcm4360_test278_console_periodic=1 \\"
echo "    bcm4360_test308_olmsg_cdc=1 \\"
echo "    debug=0x7fffffff"

insmod "$FMAC_DIR/brcmfmac.ko" \
    bcm4360_test276_shared_info=1 \
    bcm4360_test277_console_decode=1 \
    bcm4360_test278_console_periodic=1 \
    bcm4360_test308_olmsg_cdc=1 \
    debug=0x7fffffff

insmod "$FMAC_DIR/wcc/brcmfmac-wcc.ko"

echo ""
echo "Modules loaded. Sleeping 160s (full sequence + margin)..."
# Sum of timings (from pcie.c code):
#   attach + download fw 442KB (iowrite32) + NVRAM     ~2-5s
#   T276 2s poll                                       2s
#   faza 1 poll si[+0x2028] (1ms x 2000)               2s
#   T278 ladder: 0.5 + 4.5 + 25 + 60 =                 90s
#   setup + IRQ + bcdc attach + dcmd (response ms      5s
#     or 5s timeout on wrong dngl type)
#   -------------------------------------------------------
#   total                                       ~101-106s -> 160s (margin)
sleep 160

# Capture
echo ""
echo "=== Capturing dmesg → $LOG ==="
dmesg > "$LOG"

echo ""
echo "=== T308 poll si[+0x2028] (phase 1) ==="
grep -E 'test\.308.*(poll|fw_init)' "$LOG" | tail -8 || echo "  (no test.308 poll lines)"

echo ""
echo "=== olmsg transport (doorbell / ctl resp / errors) ==="
grep -E 'olmsg:' "$LOG" | tail -30 || echo "  (no olmsg lines — see errors below)"

echo ""
echo "=== BCDC attach / fw version / wiphy ==="
grep -iE 'Firmware version|bcdc|wlan|wiphy|ieee80211 phy|cfg80211' "$LOG" | tail -15 || echo "  (none — probe may not have reached cfg80211)"

echo ""
echo "=== Errors / timeouts / unknown type ==="
grep -iE 'unknown type|timeout|error|failed|reject' "$LOG" | tail -20 || echo "  (none — clean)"

echo ""
echo "=== Console fw: wr_idx ladder ==="
grep -E 'wr_idx=[0-9]+' "$LOG" | tail -5

echo ""
echo "=== T276 shared_info ==="
grep -E 'test\.276' "$LOG" | tail -5

echo ""
echo "=== Wiphy / interfaces after fire ==="
ls /sys/class/ieee80211/ 2>/dev/null && echo "(phys present ^)" || echo "  (no phy in /sys/class/ieee80211)"
iw dev 2>/dev/null || echo "  (iw dev returned no interfaces)"

echo ""
echo "Full log: $LOG ($(wc -l < "$LOG") lines)"
echo ""
echo "=== END OF FIRE: NO rmmod ==="
echo "The module must stay loaded. rmmod = teardown = wedge risk."
echo "After the test: reboot."
echo "Module state (informational):"
lsmod | grep brcm || echo "  (brcmfmac unloaded — something went wrong)"
