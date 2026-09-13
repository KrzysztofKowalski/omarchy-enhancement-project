#!/usr/bin/env bash
# mesa-manage.sh — surgical install/rollback of the patched Mesa (nouveau/nvc0, Kepler GK107)
#
# Strategy (Mesa 26.x, the "dril" architecture):
#   /usr/lib/dri/*_dri.so  ->  libdril_dri.so   (shared dispatcher ~96 KB)
#   /usr/lib/dri/libdril_dri.so  dlopens  /usr/lib/libgallium-26.1.8-arch1.1.so  (~54 MB)
#   /usr/lib/dri/nouveau_dri.so  = SYMLINK  -> libdril_dri.so
#
# The patched Mesa (nouveau-only) builds a self-contained libdril_dri.so (~2.8 MB) with
# nouveau statically linked. We copy it as a REAL file
# /usr/lib/dri/nouveau_dri_patched.so and switch the symlink
# nouveau_dri.so -> nouveau_dri_patched.so. The other drivers (iris, swrast, ...)
# stay on the system libdril_dri.so — untouched.
#
# Rollback = restoring the symlink nouveau_dri.so -> libdril_dri.so + removing
# nouveau_dri_patched.so. Ultimately: `sudo pacman -S mesa` (reinstall).
#
# Commands: status | backup | install | restore | diff
# Flags:   --yes (no confirmations), --force (overwrite backup), --dry-run
#
# Does NOT use the system /tmp — scratch in tmp/ in the project directory.
set -euo pipefail

# PROJ = ROOT of the repo (the script lives in scripts/ — dirname $0 = scripts, hence /..)
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
MESA="$PROJ/tmp/mesa"
BUILD="$MESA/build-nouveau"
DRIL_DIR="$BUILD/src/gallium/targets/dril"      # the built nouveau_dri.so -> libdril_dri.so lives here
BUILT_DRIL="$DRIL_DIR/libdril_dri.so"           # self-contained nouveau driver (real file)
PATCH="$PROJ/patches/0002-mesa-nvc0-sched-data.patch"
SCRATCH="$PROJ/tmp/mesa-manage-scratch"

DRI_DIR="/usr/lib/dri"
SYS_NOUVEAU="$DRI_DIR/nouveau_dri.so"           # symlink (system)
SYS_LIBDRIL="$DRI_DIR/libdril_dri.so"           # dispatcher (system)
# Auto-detect system libgallium version (e.g. libgallium-26.1.8-arch1.1.so).
# Override with: SYS_LIBGALLIUM=/path/to/libgallium-*.so mesa-manage.sh ...
if [ -z "${SYS_LIBGALLIUM:-}" ]; then
    SYS_LIBGALLIUM="$(ls /usr/lib/libgallium-*.so 2>/dev/null | head -1)"
    [ -z "$SYS_LIBGALLIUM" ] && SYS_LIBGALLIUM="/usr/lib/libgallium.so"
fi
PATCHED_NAME="nouveau_dri_patched.so"           # name of the patched driver file in /usr/lib/dri/
PATCHED_PATH="$DRI_DIR/$PATCHED_NAME"

# --- colors ----------------------------------------------------------------
if [ -t 1 ]; then
    C_B='\033[1m'; C_R='\033[31m'; C_G='\033[32m'; C_Y='\033[33m'; C_C='\033[36m'; C_D='\033[2m'; C_0='\033[0m'
else
    C_B=''; C_R=''; C_G=''; C_Y=''; C_C=''; C_D=''; C_0=''
fi

# --- flags -------------------------------------------------------------------
YES=0; FORCE=0; DRY=0
for arg in "$@"; do
    case "$arg" in
        --yes|-y) YES=1 ;;
        --force)  FORCE=1 ;;
        --dry-run) DRY=1 ;;
    esac
done
# remove the flags from $@ to leave the command
ARGS=()
for a in "$@"; do case "$a" in --yes|-y|--force|--dry-run) ;; *) ARGS+=("$a") ;; esac; done
set -- "${ARGS[@]}"
CMD="${1:-}"

# --- helpers -----------------------------------------------------------------
say()  { printf "%b\n" "$*"; }
ok()   { say "${C_G}✔ $*${C_0}"; }
err()  { say "${C_R}✖ $*${C_0}" >&2; }
warn() { say "${C_Y}⚠ $*${C_0}" >&2; }
hdr()  { say "${C_C}${C_B}=== $* ===${C_0}"; }
die()  { err "$*"; exit 1; }

confirm() {
    [ "$YES" = 1 ] && return 0
    local q="$1"
    printf "%b" "${C_Y}${q} [y/N] ${C_0}" >&2
    read -r ans
    [ "$ans" = "y" ] || [ "$ans" = "Y" ]
}

sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }

# Newest backup directory (if any)
latest_backup() {
    # returns the newest backup directory or an empty string; NEVER exits
    local d
    d="$(ls -1d "$PROJ/tmp/mesa-backup-"* 2>/dev/null | sort | tail -1 || true)"
    echo "$d"
}

# State of the system nouveau_dri.so symlink
# prints: "symlink -> <target>" or "file <size>" or "missing"
describe_nouveau_dri() {
    if [ -L "$SYS_NOUVEAU" ]; then
        printf "symlink -> %s" "$(readlink "$SYS_NOUVEAU")"
    elif [ -e "$SYS_NOUVEAU" ]; then
        printf "file %s bytes" "$(stat -c '%s' "$SYS_NOUVEAU")"
    else
        printf "missing"
    fi
}

# Is the patched driver currently active?
is_patched_active() {
    [ -L "$SYS_NOUVEAU" ] && [ "$(readlink "$SYS_NOUVEAU")" = "$PATCHED_NAME" ] && [ -f "$PATCHED_PATH" ]
}

# Version of the mesa package
mesa_ver() { pacman -Qi mesa 2>/dev/null | awk -F': ' '/^Version/{print $2; exit}'; }

# Check the prerequisites
check_built() {
    [ -f "$BUILT_DRIL" ] || die "Built driver not found: $BUILT_DRIL
   Run first: ./build-mesa.sh"
}

# ============================================================================
# status
# ============================================================================
do_status() {
    hdr "mesa package (pacman)"
    echo "  Version:          $(mesa_ver)"
    echo "  Package file:     $(pacman -Qo "$SYS_LIBDRIL" 2>/dev/null | sed 's/^ *//')"
    echo
    hdr "System nouveau DRI"
    echo "  Path:             $SYS_NOUVEAU"
    echo "  Type:             $(describe_nouveau_dri)"
    if [ -L "$SYS_NOUVEAU" ]; then
        local tgt; tgt="$(readlink "$SYS_NOUVEAU")"
        local abs="$DRI_DIR/$tgt"
        [ -f "$abs" ] || abs="$tgt"
        echo "  Target (real):    $abs"
        echo "  sha256 of target: $(sha "$abs" 2>/dev/null || echo '(none)')"
    fi
    echo "  libgallium:       $SYS_LIBGALLIUM"
    echo "  sha256 libgall.:  $(sha "$SYS_LIBGALLIUM" 2>/dev/null || echo '(none)')"
    echo
    hdr "Built driver (patched)"
    if [ -f "$BUILT_DRIL" ]; then
        echo "  Path:             $BUILT_DRIL"
        echo "  Size:             $(stat -c '%s' "$BUILT_DRIL") bytes"
        echo "  sha256:           $(sha "$BUILT_DRIL")"
        echo "  SONAME:           $(readelf -d "$BUILT_DRIL" 2>/dev/null | awk '/SONAME/{print $NF}' | tr -d '[]')"
    else
        warn "  No built driver ($BUILT_DRIL)"
        echo "  → run: ./build-mesa.sh"
    fi
    echo
    hdr "Currently active"
    if is_patched_active; then
        ok "PATCHED (nouveau_dri.so -> $PATCHED_NAME)"
        echo "  sha256 active:   $(sha "$PATCHED_PATH")"
    else
        say "${C_G}STOCK (original system)${C_0}"
    fi
    echo
    hdr "Backup"
    local bk; bk="$(latest_backup)"
    if [ -n "$bk" ]; then
        echo "  Newest:           $bk"
        echo "  Created:          $(stat -c '%y' "$bk" 2>/dev/null | cut -d'.' -f1)"
        if [ -f "$bk/manifest.txt" ]; then
            echo "  Manifest:         $bk/manifest.txt"
        fi
    else
        warn "  No backup. Run: $0 backup"
    fi
    echo
    hdr "Patch (git diff --stat in tmp/mesa)"
    if git -C "$MESA" diff --quiet 2>/dev/null; then
        warn "  Tree clean — patch NOT applied?"
    else
        git -C "$MESA" diff --stat 2>/dev/null | sed 's/^/  /'
    fi
}

# ============================================================================
# backup
# ============================================================================
do_backup() {
    check_built
    local ts; ts="$(date +%Y%m%d-%H%M%S)"
    local bk="$PROJ/tmp/mesa-backup-$ts"

    # idempotency: if any backup exists and there is no --force
    local existing; existing="$(latest_backup)"
    if [ -n "$existing" ] && [ "$FORCE" != 1 ]; then
        warn "Backup already exists: $existing"
        warn "Use --force to create a new one (the old one stays)."
        if confirm "Create a new backup alongside the existing one?"; then
            : # we continue
        else
            say "Skipped."
            return 0
        fi
    fi

    mkdir -p "$bk" "$SCRATCH"

    hdr "Backing up system state → $bk"

    # 1. Metadata of the nouveau_dri.so symlink (target + type)
    local nv_type="missing" nv_target=""
    if [ -L "$SYS_NOUVEAU" ]; then
        nv_type="symlink"
        nv_target="$(readlink "$SYS_NOUVEAU")"
    elif [ -e "$SYS_NOUVEAU" ]; then
        nv_type="file"
    fi
    printf '%s\n' "$nv_target" > "$bk/nouveau_dri.linktarget"

    # 2. If it is a symlink → also save where to copy the original target (libdril_dri.so) from
    if [ "$nv_type" = "symlink" ] && [ -n "$nv_target" ]; then
        local abs_tgt="$DRI_DIR/$nv_target"
        [ -f "$abs_tgt" ] || abs_tgt="$nv_target"
        if [ -f "$abs_tgt" ]; then
            cp -a "$abs_tgt" "$bk/$(basename "$abs_tgt")"
            echo "  Backed up symlink target:   $(basename "$abs_tgt")"
        fi
    elif [ "$nv_type" = "file" ]; then
        cp -a "$SYS_NOUVEAU" "$bk/nouveau_dri.so"
        echo "  Backed up file: nouveau_dri.so"
    fi

    # 3. sha256 of libgallium (for the full picture — we do not restore it, but verify it)
    if [ -f "$SYS_LIBGALLIUM" ]; then
        sha "$SYS_LIBGALLIUM" > "$bk/libgallium.sha256"
    fi

    # 4. Full file list of the mesa package
    pacman -Ql mesa > "$bk/mesa-files.txt" 2>/dev/null || true

    # 5. Manifest (human-readable)
    {
        echo "# mesa-manage.sh backup — $ts"
        echo "backup_created=$ts"
        echo "mesa_version=$(mesa_ver)"
        echo "nouveau_dri_path=$SYS_NOUVEAU"
        echo "nouveau_dri_type=$nv_type"
        echo "nouveau_dri_target=$nv_target"
        echo "sys_libdril=$SYS_LIBDRIL"
        echo "sys_libdril_sha256=$(sha "$SYS_LIBDRIL" 2>/dev/null)"
        echo "sys_libgallium=$SYS_LIBGALLIUM"
        echo "sys_libgallium_sha256=$(sha "$SYS_LIBGALLIUM" 2>/dev/null)"
        echo "built_dril=$BUILT_DRIL"
        echo "built_dril_sha256=$(sha "$BUILT_DRIL" 2>/dev/null)"
        echo "patched_install_path=$PATCHED_PATH"
        echo "patched_install_name=$PATCHED_NAME"
        echo
        echo "# Rollback:"
        echo "#   $0 restore"
        echo "# Ultimately (package reinstall):"
        echo "#   sudo pacman -S mesa"
    } > "$bk/manifest.txt"

    ok "Backup created: $bk"
    echo "  Manifest: $bk/manifest.txt"
    say "  To install the patch:    ${C_C}$0 install${C_0}"
}

# ============================================================================
# install
# ============================================================================
do_install() {
    check_built
    local bk; bk="$(latest_backup)"
    [ -n "$bk" ] || die "No backup. First: $0 backup"

    if is_patched_active; then
        warn "The patched driver is already active."
        if ! confirm "Continue (copy the file again)?"; then
            say "Skipped."; return 0
        fi
    fi

    local built_sha; built_sha="$(sha "$BUILT_DRIL")"

    hdr "Install plan (dry-run)"
    echo "  1. Copy:     $BUILT_DRIL"
    echo "              → $PATCHED_PATH   (as a real file: $PATCHED_NAME)"
    echo "  2. Switch the symlink:"
    echo "              $SYS_NOUVEAU  →  $PATCHED_NAME"
    echo "              (currently: $(describe_nouveau_dri))"
    echo "  3. Other *_dri.so untouched (still → system libdril_dri.so)."
    echo "  Backup:     $bk"
    echo "  sha256 of the built one: $built_sha"
    echo

    if [ "$DRY" = 1 ]; then say "${C_Y}--dry-run: nothing changed.${C_0}"; return 0; fi

    confirm "Perform the install?" || die "Cancelled."

    hdr "Installing"
    echo "  [1/3] Copying the built driver..."
    sudo install -m 0755 "$BUILT_DRIL" "$PATCHED_PATH"
    local inst_sha; inst_sha="$(sha "$PATCHED_PATH")"
    [ "$inst_sha" = "$built_sha" ] || die "sha256 after copy ≠ built. DO NOT continue."
    ok "  Copied. sha256 OK."

    echo "  [2/3] Switching the nouveau_dri.so symlink..."
    sudo ln -sfn "$PATCHED_NAME" "$SYS_NOUVEAU"
    ok "  Symlink: $(readlink "$SYS_NOUVEAU")"

    echo "  [3/3] Verification..."
    is_patched_active && ok "PATCHED active" || die "Symlink does not point at the patched file — check manually."

    echo
    hdr "Renderer test (optional)"
    if command -v glxinfo >/dev/null 2>&1; then
        say "  Check the renderer (new session/app, so it loads the new .so):"
        say "  ${C_C}LIBGL_DRIVERS_PATH=$DRI_DIR DRI_PRIME=0 glxinfo | grep -i renderer${C_0}"
        say "  ${C_C}LIBGL_DRIVERS_PATH=$DRI_DIR DRI_PRIME=1 glxinfo | grep -i renderer${C_0}"
        say "  Renderer NVE7 / nouveau = OK. (DRI_PRIME=0 = the dGPU nouveau on this machine.)"
    else
        warn "  glxinfo unavailable — install: sudo pacman -S mesa-utils"
    fi

    echo
    hdr "Rollback"
    say "  ${C_C}$0 restore${C_0}   — restores the stock symlink + removes the patched file"
    say "  ${C_C}sudo pacman -S mesa${C_0}  — ultimate reinstall (guaranteed stock)"
}

# ============================================================================
# restore
# ============================================================================
do_restore() {
    local bk; bk="$(latest_backup)"
    [ -n "$bk" ] || die "No backup. Alternative: sudo pacman -S mesa (package reinstall)."

    [ -f "$bk/manifest.txt" ] || die "Manifest missing in $bk — corrupted backup."

    # Load the metadata from the manifest
    local orig_target; orig_target="$(awk -F= '/^nouveau_dri_target=/{print $2}' "$bk/manifest.txt")"
    local orig_type;   orig_type="$(awk -F= '/^nouveau_dri_type=/{print $2}'   "$bk/manifest.txt")"
    local sys_libdril_sha; sys_libdril_sha="$(awk -F= '/^sys_libdril_sha256=/{print $2}' "$bk/manifest.txt")"

    hdr "Restore plan (dry-run)"
    echo "  Backup:         $bk"
    echo "  Original type:  $orig_type"
    if [ "$orig_type" = "symlink" ]; then
        echo "  Target to restore:   nouveau_dri.so -> $orig_target"
    else
        echo "  File to restore:     nouveau_dri.so (from the backup)"
    fi
    echo "  To remove:     $PATCHED_PATH   (if present)"
    echo "  Currently:     $(describe_nouveau_dri)"
    echo

    if [ "$DRY" = 1 ]; then say "${C_Y}--dry-run: nothing changed.${C_0}"; return 0; fi

    confirm "Perform the restore?" || die "Cancelled."

    hdr "Restoring"

    # 1. First restore the nouveau_dri.so symlink/file
    if [ "$orig_type" = "symlink" ] && [ -n "$orig_target" ]; then
        sudo ln -sfn "$orig_target" "$SYS_NOUVEAU"
        ok "  Symlink restored: nouveau_dri.so -> $(readlink "$SYS_NOUVEAU")"
    elif [ "$orig_type" = "file" ] && [ -f "$bk/nouveau_dri.so" ]; then
        sudo cp -a "$bk/nouveau_dri.so" "$SYS_NOUVEAU"
        ok "  File restored from the backup."
    else
        die "Don't know how to restore (type=$orig_type). Check $bk/manifest.txt"
    fi

    # 2. Remove the patched driver file
    if [ -f "$PATCHED_PATH" ] || [ -L "$PATCHED_PATH" ]; then
        sudo rm -f "$PATCHED_PATH"
        ok "  Removed: $PATCHED_PATH"
    else
        say "  (patched file did not exist — removal skipped)"
    fi

    # 3. Verification: sha256 of system libdril_dri.so == backup
    echo "  [verification] sha256 of system libdril_dri.so vs backup..."
    local cur_sha; cur_sha="$(sha "$SYS_LIBDRIL" 2>/dev/null || echo MISSING)"
    if [ -n "$sys_libdril_sha" ] && [ "$cur_sha" = "$sys_libdril_sha" ]; then
        ok "  sha256 of libdril_dri.so matches the backup."
    else
        warn "  sha256 of libdril_dri.so DIFFERS from the backup ($cur_sha vs $sys_libdril_sha)."
        warn "  The system mesa may have been updated in the meantime."
        warn "  If it is a new package version — that is OK (stock). If not — consider a reinstall."
    fi

    if is_patched_active; then
        err "  Still patched active! Manual intervention required."
        die "  Ultimately: sudo pacman -S mesa"
    fi
    ok "  State: STOCK."

    echo
    hdr "Ultimate restore (optional)"
    say "  If anything looks wrong — a package reinstall guarantees stock:"
    say "  ${C_C}sudo pacman -S mesa${C_0}"
}

# ============================================================================
# diff
# ============================================================================
do_diff() {
    check_built
    hdr "sha256 — comparison"
    printf "  %-22s %s\n" "built libdril:"      "$(sha "$BUILT_DRIL")"
    printf "  %-22s %s\n" "system libdril:"     "$(sha "$SYS_LIBDRIL" 2>/dev/null || echo MISSING)"
    if [ -f "$PATCHED_PATH" ]; then
        printf "  %-22s %s\n" "installed patched:"  "$(sha "$PATCHED_PATH")"
    else
        printf "  %-22s %s\n" "installed patched:"  "(not installed)"
    fi
    local bk; bk="$(latest_backup)"
    if [ -n "$bk" ] && [ -f "$bk/libgallium.sha256" ]; then
        printf "  %-22s %s\n" "backup libgallium:"  "$(cat "$bk/libgallium.sha256")"
    fi
    echo
    hdr "Patch — modified source files (git diff --stat in tmp/mesa)"
    if git -C "$MESA" diff --quiet 2>/dev/null; then
        warn "  Tree clean (patch not applied to the sources?)."
    else
        git -C "$MESA" diff --stat 2>/dev/null | sed 's/^/  /'
        echo
        say "  Full diff:  ${C_C}git -C $MESA diff${C_0}"
        say "  Patch:      $PATCH"
    fi
}

# ============================================================================
# usage
# ============================================================================
usage() {
    cat <<EOF
${C_B}mesa-manage.sh${C_0} — install/rollback of the patched Mesa (nouveau/nvc0, Kepler GK107)

${C_B}Commands:${C_0}
  status    Show state: pacman, the system nouveau driver, backup, patched/stock, source diff
  backup    Back up the system state to tmp/mesa-backup-<date>/ (required before install)
  install   Install the patched driver (requires an existing backup)
  restore   Restore the state from the backup (symlink + remove the patched file)
  diff      sha256 of built vs system vs backup + git diff --stat of the sources

${C_B}Flags:${C_0}
  --yes       No confirmations (don't ask before install/restore)
  --force     New backup alongside the existing one (backup normally warns)
  --dry-run   Show the plan without modifying anything (install/restore)

${C_B}Flow (user gates):${C_0}
  1. ./build-mesa.sh                 # build the patched driver (already done)
  2. ./mesa-manage.sh status         # verify the paths and sha
  3. ./mesa-manage.sh backup         # back up the system (safe, does not modify the system)
  4. ./mesa-manage.sh install        # install (asks for confirmation)
  5. test: DRI_PRIME=0 glxinfo | grep -i renderer
  6. ./mesa-manage.sh restore        # roll back if something is wrong
  7. (ultimately) sudo pacman -S mesa

${C_B}Key paths:${C_0}
  System:  $SYS_NOUVEAU  ->  $(readlink "$SYS_NOUVEAU" 2>/dev/null || echo '?')
           $SYS_LIBDRIL  (dispatcher, dlopens libgallium)
  Built:   $BUILT_DRIL
  Patched installed as: $PATCHED_PATH
EOF
}

# ============================================================================
# dispatch
# ============================================================================
case "$CMD" in
    status)  do_status ;;
    backup)  do_backup ;;
    install) do_install ;;
    restore) do_restore ;;
    diff)    do_diff ;;
    ""|-h|--help|help) usage ;;
    *) die "Unknown command: $CMD
$(usage)" ;;
esac