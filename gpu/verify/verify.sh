#!/usr/bin/env bash
# verify.sh — builds and runs all CUDA verification programs for the GT 750M.
# Requires: the 'driver' variant installed (nvidia module) + 'cuda' (nvcc 10.2).
#
#   ./verify.sh          # build + run + summary
#   CUDA_HOME=/opt/cuda ./verify.sh

set -uo pipefail
cd "$(dirname "$0")"

export CUDA_HOME="${CUDA_HOME:-/opt/cuda}"
NVCC="$CUDA_HOME/bin/nvcc"
ARCH="-gencode arch=compute_30,code=sm_30"
PASS=0; FAIL=0

b() { printf '\033[1;36m..\033[0m %s\n' "$*"; }
ok(){ printf '\033[1;32mOK\033[0m %s\n' "$*"; PASS=$((PASS+1)); }
no(){ printf '\033[1;31mNO\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }

echo "== Step 0: environment =="
command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader 2>/dev/null \
  && ok "nvidia-smi sees the card" || no "nvidia-smi unavailable — is the nvidia module loaded?"
if command -v nvcc >/dev/null; then
  ok "nvcc: $(nvcc --version | tail -1 | sed 's/^ *//')"
else
  if [[ -x "$NVCC" ]]; then ok "nvcc: $($NVCC --version | tail -1 | sed 's/^ *//')"
  else no "nvcc missing (CUDA_HOME=$CUDA_HOME) — install the 'cuda' variant"; fi
fi
echo

NVCC_BIN="nvcc"; command -v nvcc >/dev/null || NVCC_BIN="$NVCC"

for prog in check_cuda vecadd bandwidth; do
  b "build: $prog.cu"
  if $NVCC_BIN $ARCH -O2 -std=c++14 $prog.cu -o $prog 2>build.err; then
    ok "built: $prog"
  else
    no "compilation of $prog failed:"; sed 's/^/    /' build.err
    FAIL=$((FAIL+1)); continue
  fi
  b "run: ./$prog"
  if ./$prog; then ok "$prog: PASS"; else no "$prog: FAIL"; fi
  echo
done

echo "=========================================="
printf "Summary:       \033[1;32m%d PASS\033[0m   \033[1;31m%d NO/FAIL\033[0m\n" "$PASS" "$FAIL"
echo "=========================================="
exit $(( FAIL > 0 ? 1 : 0 ))