#!/usr/bin/env bash
# patch-check.sh — check whether the om-kernel patches (0001–0018) apply on a given tag
#
# Universal counterpart of tmp/patch-check-719.sh from nv-kepler: scratch worktree
# ($PROJ/tmp/linux-patch-check), materializing ONLY the files touched by the
# patches (git show <TAG>:<path> — no index/sparse; lesson: sparse-checkout
# in a scratch worktree is broken) and SEQUENTIAL `git apply --check` + `git apply`
# one after another — each patch's context sees the changes of the previous ones
# (lesson: applying per-patch on a clean tree gives false OK/FAIL, e.g. because
# 0006/0007/0008/0010/0011 touch the same file nouveau_drm.c).
#
# Usage:
#   ./scripts/patch-check.sh                                — patches from $PROJ/patches/kernel
#                                                            on $PROJ/tmp/linux-omkp (auto tag)
#   ./scripts/patch-check.sh --series $PROJ/patches/kernel --tree $PROJ/tmp/linux-omkp
#   ./scripts/patch-check.sh --tag v7.2.0                   — verify against another tag
#   ./scripts/patch-check.sh --check-only                   — only git apply --check
#   ./scripts/patch-check.sh --log PATH                     — write the full log to a file
#   ./scripts/patch-check.sh --worktree PATH                — a different scratch directory
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SERIES="${SERIES:-$PROJ/patches/kernel}"
TREE="${TREE:-$PROJ/tmp/linux-omkp}"
WT="${WT:-$PROJ/tmp/linux-patch-check}"
LOG="${LOG:-$PROJ/tmp/patch-check.log}"
TAG=""
CHECK_ONLY=false

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --series=*)  SERIES="${1#*=}" ;;
    --series)    SERIES="$2"; shift ;;
    --tree=*)    TREE="${1#*=}" ;;
    --tree)      TREE="$2"; shift ;;
    --tag=*)     TAG="${1#*=}" ;;
    --tag)       TAG="$2"; shift ;;
    --check-only) CHECK_ONLY=true ;;
    --log=*)     LOG="${1#*=}" ;;
    --log)       LOG="$2"; shift ;;
    --worktree=*) WT="${1#*=}" ;;
    --worktree)  WT="$2"; shift ;;
    -h|--help)   usage; exit 0 ;;
    *) echo "ERROR: unknown argument: $1" >&2; usage; exit 1 ;;
  esac
  shift
done

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

# --- log: everything to stdout + a copy to $LOG (tee through a substituted process) ---
mkdir -p "$(dirname "$LOG")" 2>/dev/null || true
exec > >(tee "$LOG") 2>&1

echo "== patch-check.sh start ($(date '+%F %T')) =="
echo "   series:   $SERIES"
echo "   tree:     $TREE"
echo "   worktree: $WT"
echo "   tag:      ${TAG:-auto}"
if [ "$CHECK_ONLY" = true ]; then
  echo "   mode:     --check-only (only git apply --check, NO application — each patch"
  echo "             against a CLEAN tree; the full sequential test = without --check-only)"
fi
echo

[ -d "$SERIES" ] || die "no patch directory: $SERIES"
[ -d "$TREE/.git" ] || die "$TREE is not a git clone (no .git) — prepare the tree: ./scripts/setup-tree.sh"

# target tag: --tag or auto (describe --tags of the current tree)
if [ -n "$TAG" ]; then
  TARGET="v${TAG#v}"
else
  TARGET="$(git -C "$TREE" describe --tags 2>/dev/null || echo v7.1.9)"
  TARGET="v${TARGET#v}"
fi
echo "== [0/4] target: $TARGET =="
if ! git -C "$TREE" rev-parse --verify "$TARGET" >/dev/null 2>&1; then
  echo "    tag $TARGET unknown — fetch from stable (fallback: origin)"
  git -C "$TREE" fetch --depth 1 stable tag "$TARGET" 2>/dev/null \
    || git -C "$TREE" fetch --depth 1 origin tag "$TARGET" \
    || die "fetch of $TARGET failed — check the network (git.kernel.org) and retry"
fi

# --- patch list: all 0*-*.patch from $SERIES, sort -V (0001→0018) -------------
mapfile -t NAMES < <(cd "$SERIES" && shopt -s nullglob && printf '%s\n' 0*-*.patch | LC_ALL=C sort -V)
[ "${#NAMES[@]}" -ge 1 ] || die "no patches (0*-*.patch) in $SERIES"
echo "== [1/4] ${#NAMES[@]} patches in apply order:"
printf '         %s\n' "${NAMES[@]}"
echo

# --- files touched by the patches (from each patch, from the `--- a/<path>` lines) ----
# The patch format is mixed (0001–0005: diff --git; 0006–0018: --- a/+++ b/), but
# EVERY one has a `--- a/<path>` line — hence the universal path extraction.
files=()
for name in "${NAMES[@]}"; do
  while IFS= read -r f; do
    [ -n "$f" ] && files+=("$f")
  done < <(grep -E '^--- (a|i)/' "$SERIES/$name" 2>/dev/null | sed -E 's#^--- (a|i)/##; s/[[:space:]].*$//')
done
mapfile -t FILES < <(printf '%s\n' "${files[@]}" | LC_ALL=C sort -u)
echo "== [2/4] ${#FILES[@]} touched files (materialized via git show $TARGET:<path>):"
printf '         %s\n' "${FILES[@]}"
echo

# --- scratch worktree (if needed) ---------------------------------------------
if [ ! -e "$WT/.git" ]; then
  echo "== [3/4] git worktree add --detach --no-checkout $WT $TARGET"
  git -C "$TREE" worktree add --detach --no-checkout "$WT" "$TARGET" \
    || die "worktree add failed (check whether $TARGET exists and $WT is not in use)"
else
  echo "== [3/4] worktree $WT already exists — using it (materializing the files anew)"
fi

# materialize the touched files (git show reads from objects — no index)
for f in "${FILES[@]}"; do
  mkdir -p "$WT/$(dirname "$f")"
  git -C "$TREE" show "$TARGET:$f" > "$WT/$f" 2>/dev/null \
    || die "git show $TARGET:$f failed — does the file exist on this tag?"
done
echo "    ${#FILES[@]} files materialized OK"
echo

# --- SEQUENTIAL apply (+check) per patch --------------------------------------
if [ "$CHECK_ONLY" = true ]; then
  echo "== [4/4] SEQUENTIAL git apply --check per patch (check-only, no application) =="
else
  echo "== [4/4] SEQUENTIAL git apply --check + apply per patch =="
fi
cd "$WT" || exit 1
ok=0; fail=0
for name in "${NAMES[@]}"; do
  p="$SERIES/$name"
  if git apply --check "$p" >"$WT/check.out" 2>&1; then
    if [ "$CHECK_ONLY" = true ]; then
      echo "OK    $name (check-only)"; ok=$((ok+1))
    elif git apply "$p" 2>/dev/null; then
      echo "OK    $name"; ok=$((ok+1))
    else
      echo "FAIL  $name (--check OK, but apply failed anyway)"; fail=$((fail+1))
    fi
  else
    echo "FAIL  $name"
    # Note: `sed | head` in this script runs under set -o pipefail — head closes
    # the pipe after 12 lines → sed gets SIGPIPE (141) → false negative. Hence
    # `|| true`: show the error, but do not kill the loop (lesson from nv-kepler).
    sed 's/^/      /' "$WT/check.out" | head -12 || true
    fail=$((fail+1))
  fi
done
rm -f "$WT/check.out"
mode=$([ "$CHECK_ONLY" = true ] && echo check-only || echo full)
echo "== RESULT: ok=$ok fail=$fail (tag $TARGET, mode $mode) =="
[ "$fail" -eq 0 ] && echo "== ALL patches apply SEQUENTIALLY on $TARGET. ==" \
                  || echo "== THERE ARE ERRORS — see above (if when changing the tag: two-level rebase). =="
# also echo the result to the terminal (stdout went to the log via tee)
echo "patch-check: ok=$ok fail=$fail — details in $LOG" >&2