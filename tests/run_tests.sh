#!/bin/bash
# Run all tests locally: C ring buffer unit tests + Python pipeline & integrity tests.
# Creates a venv and installs PyYAML on first run (~3s), <2s after.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# ── C ring buffer unit tests ──────────────────────────────────────────────
echo "=== Compiling C ring buffer tests ==="
cc -std=c11 -Wall -Wextra -Werror \
   -I "$PROJECT_ROOT/MacAudioDriver" \
   -DkSHM_Name='"/macaudio_test_rb"' \
   -o "$SCRIPT_DIR/test_ring_buffer" \
   "$SCRIPT_DIR/test_ring_buffer.c" \
   "$PROJECT_ROOT/MacAudioDriver/SharedRingBuffer.c"

echo "=== Running C ring buffer tests ==="
"$SCRIPT_DIR/test_ring_buffer"
echo ""

# ── Python tests ──────────────────────────────────────────────────────────
# Create venv + install deps if needed
if [ ! -d "$VENV_DIR" ]; then
    echo "Creating test venv..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --quiet pyyaml
fi

# Run tests from project root so relative paths resolve
cd "$PROJECT_ROOT"
echo "=== Running Python pipeline tests ==="
"$VENV_DIR/bin/python" -m unittest tests.test_release_pipeline -v
echo ""
echo "=== Running Python source integrity tests ==="
"$VENV_DIR/bin/python" -m unittest tests.test_source_integrity -v
