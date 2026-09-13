#!/usr/bin/env bash
set -euo pipefail

LOG="${LOG:-$HOME/.local/state/ol-has/build-ollama.log}"

# The whole script output goes to the console AND the log file.
mkdir -p "$(dirname "$LOG")"
exec > >(tee -a "$LOG") 2>&1

echo "=== Building Ollama v0.33.0-rc2 (CMake) ==="
echo "Log: $LOG"
echo

# --- Preflight: check dependencies ---
for tool in go cmake ninja; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: tool '$tool' not found."
        echo "Install the missing deps with:"
        echo "  sudo pacman -S --needed go cmake ninja"
        exit 1
    fi
done
echo "OK: go, cmake, ninja present."
echo "  go:      $(go version)"
echo "  cmake:   $(cmake --version | head -1)"
echo "  ninja:   $(ninja --version)"

# --- Preflight: repo presence ---
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/ollama" && pwd)"
if [ ! -d "$REPO" ]; then
    echo "ERROR: repo not present at: $REPO"
    exit 1
fi
cd "$REPO"
echo "Repo: $REPO"
echo

# --- Configuration ---
echo "Configuring CMake..."
cmake -B build . \
    -DOLLAMA_VERSION=v0.33.0-rc2 \
    -DGGML_CPU_ALL_VARIANTS=OFF \
    -DGGML_AVX2=OFF \
    -DGGML_FMA=OFF \
    -DGGML_F16C=OFF \
    -DGGML_BMI2=OFF
echo

# --- Build ---
echo "Building (cores: $(nproc))..."
cmake --build build --parallel "$(nproc)"
echo

# --- Version check ---
echo "Verifying version..."
./ollama --version
echo

# --- Smoke test on port 11435 (NEVER 11434!) ---
SERVER_PID=""
cleanup() {
    if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        echo "Test server stopped."
    fi
}
trap cleanup EXIT

echo "Starting the server on 127.0.0.1:11435..."
OLLAMA_HOST=127.0.0.1:11435 ./ollama serve &
SERVER_PID=$!

echo "Polling http://127.0.0.1:11435/api/version (max 30 s)..."
ok=0
for i in $(seq 1 30); do
    if curl -sf http://127.0.0.1:11435/api/version >/dev/null 2>&1; then
        ok=1
        break
    fi
    sleep 1
done

if [ "$ok" -eq 1 ]; then
    echo "SUCCESS: server answers at http://127.0.0.1:11435/api/version"
else
    echo "ERROR: server did not answer within 30 seconds."
    exit 1
fi

echo
echo "=== Build finished successfully ==="
echo "Binary: $REPO/ollama"
echo "Full log: $LOG"