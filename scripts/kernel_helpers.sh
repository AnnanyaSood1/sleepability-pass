#!/usr/bin/env bash
# Reproduce results/linux-6.18: run the pass on the Rust helpers of a kernel tree.
#   ./scripts/kernel_helpers.sh /path/to/linux [out-dir]
# The tree must be configured (e.g. `make LLVM=-18 defconfig`). CONFIG_RUST is
# not needed: the helpers are plain C and kbuild can emit their LLVM IR directly.
set -euo pipefail
KDIR="${1:?usage: $0 /path/to/linux [out-dir]}"
OUT="${2:-results/local}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OPT="${OPT:-opt-18}"; command -v "$OPT" >/dev/null || OPT=opt
PLUGIN=$(ls "$HERE"/build/libSleepability.* 2>/dev/null | head -1)
[[ -n "$PLUGIN" ]] || { echo "error: run ./scripts/build.sh first"; exit 1; }
mkdir -p "$OUT"
# rust/Makefile silences -Wmissing-prototypes only for helpers.o, so pass it here.
make -C "$KDIR" LLVM=-18 KCFLAGS="-Wno-missing-prototypes -Wno-missing-declarations" \
     rust/helpers/helpers.ll
"$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability -disable-output \
       -sleepability-primitives="$HERE/config/linux-6.18.primitives" \
       -sleepability-summary="$OUT/helpers.clsc_summary" \
       "$KDIR/rust/helpers/helpers.ll" | tee "$OUT/helpers.verdicts.txt"
