#!/usr/bin/env bash
# Build the Sleepability pass plugin (out-of-tree, new pass manager).
set -euo pipefail
cd "$(dirname "$0")/.."

# Find an LLVM install. Override with: LLVM_CONFIG=llvm-config-XX ./scripts/build.sh
LLVM_CONFIG="${LLVM_CONFIG:-}"
if [[ -z "$LLVM_CONFIG" ]]; then
  for c in llvm-config-18 llvm-config-17 llvm-config-16 llvm-config; do
    if command -v "$c" >/dev/null 2>&1; then LLVM_CONFIG="$c"; break; fi
  done
fi
[[ -z "$LLVM_CONFIG" ]] && { echo "error: no llvm-config found (install llvm-*-dev)"; exit 1; }

echo "using $($LLVM_CONFIG --version) from $LLVM_CONFIG"
CMAKE_DIR="$($LLVM_CONFIG --cmakedir)"

GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

mkdir -p build
cd build
cmake $GEN -DLLVM_DIR="$CMAKE_DIR" ..
cmake --build .
echo
echo "built: $(pwd)/libSleepability.*"
