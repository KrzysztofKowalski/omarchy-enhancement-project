#!/usr/bin/env bash
# update-kernel.sh — update the omnkp kernel to a new stable tag (e.g. 7.1.9 → 7.2.3)
#
# The full "new upstream kernel" path in one script, without building (the build
# is run by USER, or via --build). Phase order:
#   [1/5] fetch tag vX.Y.Z into tmp/linux-omkp (stable; idempotent — no fetch
#         when the tag is already present)
#   [2/5] SEQUENTIAL verification of patches 0001–NNNN against vX.Y.Z (patch-check.sh,
#         scratch worktree; GATE — on fail>0 the main tree is NOT touched)
#   [3/5] sync the main tree: git checkout -f vXY.Z (+ backup/restore .config,
#         clean lib/raid/raid6)
#   [4/5] apply patches + stamp .omnkp-patches.stamp (1:1 format with
#         build-om-kernel.sh → build step 2 then SKIPs)
#   [5/5] check the om-wifi series (38 brcmfmac patches) against vX.Y.Z — sequential
#         apply on a discarded copy of brcm80211 (git archive from the tag); does
#         NOT touch ../om-wifi
#
# On success: build → UKI → reboot (exact commands in the summary at the end).
# Equivalent of the manual flow from README-SKRYPTY.md ("update on a new
# upstream kernel"), except patches apply BEFORE the build and with a .config
# backup (mrproper with --clean deletes .config — the backup saves it).
#
# Usage:
#   ./scripts/update-kernel.sh                    # full run to v7.2.3 (default)
#   ./scripts/update-kernel.sh --kver=7.2.5       # different tag (upstream has newer ones too)
#   ./scripts/update-kernel.sh --check-only       # only phases 1+2 (+5), no sync/apply
#   ./scripts/update-kernel.sh --no-wifi          # skip the om-wifi series check
#   ./scripts/update-kernel.sh --build            # after the phases run build-om-kernel.sh
#   ./scripts/update-kernel.sh --build --uki      # …then build-uki-omnkp.sh (limine)
#   ./scripts/update-kernel.sh --jobs=1           # build on 1 thread (passed to --build)
#                                                 # (without --build: --jobs only in the summary)
set -euo pipefail

# PROJ = ROOT repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$PROJ/tmp/linux-omkp"
PATCH_KERNEL="$PROJ/patches/kernel"
PATCH_GENERIC="$PROJ/patches"
LOCALVERSION="-omnkp"
STABLE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
KVER="7.2.3"
CHECK_ONLY=false
NO_WIFI=false
DO_BUILD=false
DO_UKI=false
JOBS=""

usage() { sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --kver=*)     KVER="${1#*=}" ;;
    --check-only) CHECK_ONLY=true ;;
    --no-wifi)    NO_WIFI=true ;;
    --build)      DO_BUILD=true ;;
    --uki)        DO_UKI=true ;;
    --jobs=*)     JOBS="${1#*=}" ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $1" >&2; usage; exit 1 ;;
  esac
  shift
done
TARGET="v${KVER#v}"
if [ -n "$JOBS" ]; then
  case "$JOBS" in *[!0-9]*) echo "ERROR: --jobs=$JOBS is not a number" >&2; exit 1 ;; esac
  [ "$JOBS" -ge 1 ] || { echo "ERROR: --jobs must be ≥1" >&2; exit 1; }
fi

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

echo "=== update-kernel.sh — updating the omnkp tree to $TARGET ==="
echo "    src: $SRC"

# ----------------------------------------------------------------------------
# [0/5] Preflight
# ----------------------------------------------------------------------------
[ -d "$SRC/.git" ] || die "$SRC is not a git clone — run ./scripts/setup-tree.sh first"
mapfile -t PATCH_NAMES < <(cd "$PATCH_KERNEL" && shopt -s nullglob && printf '%s\n' 0*-*.patch | LC_ALL=C sort -V)
[ "${#PATCH_NAMES[@]}" -ge 1 ] || die "no patches (0*-*.patch) in $PATCH_KERNEL"
WIFI_DRIVER="$PROJ/../om-wifi/driver"
HAVE_WIFI=false
if [ -d "$WIFI_DRIVER/patches" ] && [ -f "$WIFI_DRIVER/scripts/forced-config.mk" ]; then
  HAVE_WIFI=true
fi
log "    patches: ${#PATCH_NAMES[@]} (series 0001–${PATCH_NAMES[-1]%%-*}), om-wifi check: $HAVE_WIFI"
log

# ----------------------------------------------------------------------------
# [1/5] Fetch the tag (idempotent)
# ----------------------------------------------------------------------------
log ">>> [1/5] Tag $TARGET"
if git -C "$SRC" rev-parse --verify "$TARGET^{commit}" >/dev/null 2>&1; then
  log "    tag $TARGET already in repo — no fetch"
else
  log "    fetching $TARGET (shallow, from stable; may take a few minutes)..."
  git -C "$SRC" fetch --depth 1 stable tag "$TARGET" 2>/dev/null \
    || git -C "$SRC" fetch --depth 1 origin tag "$TARGET" \
    || die "fetch of $TARGET failed — check the network (git.kernel.org) and retry"
fi
log

# ----------------------------------------------------------------------------
# [2/5] Sequential patch verification (GATE)
# ----------------------------------------------------------------------------
# patch-check.sh always exits with code 0 — we parse the result from its log.
PCHK_WT="$PROJ/tmp/linux-patch-check-${KVER}"
PCHK_LOG="$PROJ/tmp/patch-check-${KVER}.log"
log ">>> [2/5] Sequential patch-check on $TARGET (scratch worktree $PCHK_WT)"
bash "$PROJ/scripts/patch-check.sh" --tag "$KVER" --worktree "$PCHK_WT" --log "$PCHK_LOG" >/dev/null
wynik="$(grep -E 'WYNIK: ok=[0-9]+ fail=[0-9]+' "$PCHK_LOG" | tail -1 || true)"
ok="$(printf '%s' "$wynik" | sed -n 's/.*ok=\([0-9]*\).*/\1/p')"
fail="$(printf '%s' "$wynik" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')"
[ -n "$ok" ] && [ -n "$fail" ] || die "could not read the patch-check result from $PCHK_LOG — inspect it manually"
if [ "$fail" -ne 0 ]; then
  log "ERROR: $fail/${#PATCH_NAMES[@]} patches do NOT apply on $TARGET." >&2
  log "  Full log: $PCHK_LOG (section [4/4] — FAIL with context)." >&2
  log "  Main tree UNCHANGED (still on $(git -C "$SRC" describe --tags 2>/dev/null || echo '?'))." >&2
  log "  Porting the patches is a manual/agent task — I will not force it." >&2
  exit 1
fi
log "    OK: $ok/${#PATCH_NAMES[@]} patches apply sequentially on $TARGET (log: $PCHK_LOG)"
log

# ----------------------------------------------------------------------------
# [3/5] Sync the main tree to the tag
# ----------------------------------------------------------------------------
if [ "$CHECK_ONLY" = true ]; then
  log ">>> [3/5] skipping sync (--check-only)"
else
  log ">>> [3/5] Main tree sync: checkout -f $TARGET"
  if [ "$(git -C "$SRC" describe --tags 2>/dev/null || true)" = "$TARGET" ]; then
    log "    tree already on $TARGET"
  else
    log "    WARNING: checkout -f DISCARDS uncommitted changes (applied 7.1.9"
    log "    patches and experiments) — phase [4/5] applies the proper patches fresh."
    # .config backup: mrproper/--clean deletes .config, and it has manual settings
    # (CONFIG_MOUSE_BCM5974=m). checkout -f does not touch untracked files, but the
    # backup stays regardless — cheap insurance.
    oldtag="$(git -C "$SRC" describe --tags 2>/dev/null || git -C "$SRC" rev-parse --short HEAD)"
    if [ -f "$SRC/.config" ]; then
      mkdir -p "$PROJ/tmp/config-backup"
      cp -a "$SRC/.config" "$PROJ/tmp/config-backup/.config-$oldtag"
      log "    .config backup ($oldtag) → tmp/config-backup/.config-$oldtag"
    fi
    git -C "$SRC" checkout -f "$TARGET" || die "checkout -f $TARGET failed"
    git -C "$SRC" clean -fd lib/raid/raid6 2>/dev/null || true
    # if .config vanished (mrproper/manual) — restore it from the backup
    if [ ! -f "$SRC/.config" ] && [ -f "$PROJ/tmp/config-backup/.config-$oldtag" ]; then
      cp -a "$PROJ/tmp/config-backup/.config-$oldtag" "$SRC/.config"
      log "    .config restored from backup"
    fi
    log "    tree: $(git -C "$SRC" describe --tags 2>/dev/null || git -C "$SRC" rev-parse --short HEAD)"
  fi
fi
log

# ----------------------------------------------------------------------------
# [4/5] Apply patches + stamp (1:1 format with build-om-kernel.sh)
# ----------------------------------------------------------------------------
PATCH_STAMP="$SRC/.omnkp-patches.stamp"

find_patch() {
  local name="$1"
  if [ -f "$PATCH_KERNEL/$name" ]; then printf '%s\n' "$PATCH_KERNEL/$name"; return 0; fi
  if [ -f "$PATCH_GENERIC/$name" ]; then printf '%s\n' "$PATCH_GENERIC/$name"; return 0; fi
  return 1
}

patches_digest() {
  # Hash of content + NAMES of all patches in order — identical to
  # build-om-kernel.sh, so the stamp is interchangeable between the scripts.
  local p
  (
    for name in "$@"; do
      printf '%s\n' "[$name]"
      p="$(find_patch "$name" 2>/dev/null || true)"
      [ -n "$p" ] && cat "$p" 2>/dev/null || true
    done
  ) | sha256sum | cut -d' ' -f1
}

apply_patch() {
  local p="$1" name
  name="$(basename "$p")"
  if git apply --check "$p" 2>/dev/null; then
    git apply "$p"
    log "    OK   $name"
  elif git apply --check --reverse "$p" 2>/dev/null; then
    log "    SKIP $name — already applied"
  else
    die "$name does not apply cleanly and is not already applied (tree in $SRC — resolve manually)"
  fi
}

if [ "$CHECK_ONLY" = true ]; then
  log ">>> [4/5] skipping apply (--check-only)"
else
  log ">>> [4/5] Applying ${#PATCH_NAMES[@]} patches in the main tree"
  cd "$SRC"
  digest="$(patches_digest "${PATCH_NAMES[@]}")"
  stamp_new="$(git rev-parse HEAD):$digest"
  if [ -f "$PATCH_STAMP" ] && [ "$(cat "$PATCH_STAMP" 2>/dev/null)" = "$stamp_new" ]; then
    log "    SKIP — stamp matches ($PATCH_STAMP)"
  else
    if ! git diff --quiet; then
      log "    tree has changes — reset to the clean base (git checkout -f)"
      git checkout -f
    fi
    for name in "${PATCH_NAMES[@]}"; do
      p="$(find_patch "$name")" || die "patch $name not found"
      apply_patch "$p"
    done
    printf '%s\n' "$stamp_new" > "$PATCH_STAMP"
    log "    stamp written: $PATCH_STAMP (build-om-kernel.sh step 2 SKIPs afterwards)"
  fi
  cd "$PROJ"
fi
log

# ----------------------------------------------------------------------------
# [5/5] Check the om-wifi series (brcmfmac) on the same tag
# ----------------------------------------------------------------------------
if [ "$NO_WIFI" = true ] || [ "$HAVE_WIFI" = false ]; then
  log ">>> [5/5] om-wifi check skipped ($([ "$NO_WIFI" = true ] && echo --no-wifi || echo no ../om-wifi))"
else
  log ">>> [5/5] Checking the om-wifi series (38+ brcmfmac patches) on $TARGET"
  WIFICHK="$PROJ/tmp/wifi-check-${KVER}"
  rm -rf "$WIFICHK"
  mkdir -p "$WIFICHK"
  # copy of brcmfmac+include FROM THE TAG (git archive — no checkout), like
  # build-om-wifi.sh does from the tree; brcmsmac/bwss are not needed
  git -C "$SRC" archive "${TARGET}:drivers/net/wireless/broadcom/brcm80211" \
    -- brcmfmac include | tar -x -C "$WIFICHK"
  # same trick as build-om-wifi.sh: forced-config after Makefile line 6
  sed -i '6r '"$WIFI_DRIVER/scripts/forced-config.mk" "$WIFICHK/brcmfmac/Makefile"
  mapfile -t WIFINAMES < <(cd "$WIFI_DRIVER/patches" && shopt -s nullglob && printf '%s\n' 00[0-9][0-9]-*.patch | LC_ALL=C sort -V)
  wok=0; wfirst=""
  for wname in "${WIFINAMES[@]}"; do
    if patch -p1 -d "$WIFICHK" --no-backup-if-mismatch -i "$WIFI_DRIVER/patches/$wname" >/dev/null 2>&1; then
      wok=$((wok+1))
    else
      wfirst="$wname"
      break
    fi
  done
  if [ -n "$wfirst" ]; then
    log "    WARNING: om-wifi series STALLS on $TARGET: $wfirst ($wok/${#WIFINAMES[@]} applied)."
    log "           This does not block the kernel build/install; a brcmfmac rebuild"
    log "           (build-om-wifi.sh) requires porting this patch — a separate step."
  else
    log "    OK: all ${#WIFINAMES[@]} om-wifi patches apply on $TARGET (copy: $WIFICHK)"
    log "        After the kernel rebuild, rebuild brcmfmac: ./scripts/build-om-wifi.sh"
    log "        (vermagic will change — the old module will not load on the new kernel)"
  fi
fi
log

# ----------------------------------------------------------------------------
# Summary + next steps
# ----------------------------------------------------------------------------
newrel="${KVER}${LOCALVERSION}-dirty"   # tag = exactly HEAD → no -g<hash>;
                                        # patches in the tree → -dirty (LOCALVERSION_AUTO=y)
log "=== SUMMARY ==="
log "tree: $(git -C "$SRC" describe --tags 2>/dev/null || git -C "$SRC" rev-parse --short HEAD), patches: $([ "$CHECK_ONLY" = true ] && echo 'not applied (--check-only)' || echo 'applied + stamp')"
log "expected kernelrelease/vermagic after build: $newrel"
log
log "Next steps:"
jobs_hint=""; [ -n "$JOBS" ] && jobs_hint=" --jobs=$JOBS"
log "  1. build + install:      ./scripts/build-om-kernel.sh --kver=$KVER --full-tree$jobs_hint"
log "     (without --clean — keeps .config with CONFIG_MOUSE_BCM5974=m; with --clean"
log "      config comes back from /proc/config.gz of the running 7.1.9-omnkp kernel, which"
log "      ALSO has MOUSE_BCM5974=m — verified 2026-09-11, the README note is outdated)"
log "  2. UKI + limine entry:   ./scripts/build-uki-omnkp.sh   (KREL auto — VERIFY the printed one)"
log "  3. after build:          ./scripts/build-om-wifi.sh    (brcmfmac under $newrel)"
log "  4. reboot → //omnkp → uname -r = $newrel"
log
log "Semantic risks (to observe after boot, do not block the build):"
log "  - 0005 (intel_dp.c) — the file changed heavily 7.1.9→7.2.3 (+484/-… lines),"
log "    apply-check passes, but verify edp dpcd retry behavior live."
log "  - tests after reboot: suspend/resume (bcm5974 0016–0018, nouveau 0009–0011),"
log "    switcheroo power-on (0019/0020) — as in PLAN.md."
log

# ----------------------------------------------------------------------------
# Chained (--build / --uki) — the scripts do their own sudo -n; do NOT run
# through sudo ./ (the build must run as user — no-builds rule + reclocked [compiler] thermals)
# ----------------------------------------------------------------------------
build_args=(--kver="$KVER" --full-tree)
[ -n "$JOBS" ] && build_args+=(--jobs="$JOBS")

if [ "$DO_BUILD" = true ] && [ "$CHECK_ONLY" = false ]; then
  log ">>> --build: build-om-kernel.sh ${build_args[*]}"
  bash "$PROJ/scripts/build-om-kernel.sh" "${build_args[@]}"
  krel_now="$(cat "$SRC/include/config/kernel.release" 2>/dev/null || true)"
  log "    kernel.release in the tree after build: ${krel_now:-?}"
fi

if [ "$DO_UKI" = true ] && [ "$CHECK_ONLY" = false ]; then
  log ">>> --uki: build-uki-omnkp.sh (KREL auto = newest *-omnkp* in /usr/lib/modules)"
  bash "$PROJ/scripts/build-uki-omnkp.sh"
fi