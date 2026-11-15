#!/usr/bin/env bash
# Compile each C test to LLVM IR, run the pass, and diff against expected output.
#   ./scripts/run_tests.sh           run and check
#   ./scripts/run_tests.sh -v        show diffs on failure
#   ./scripts/run_tests.sh --update  regenerate expected/*.txt
set -uo pipefail
cd "$(dirname "$0")/.."

CLANG="${CLANG:-clang-18}"; command -v "$CLANG" >/dev/null || CLANG=clang
OPT="${OPT:-opt-18}";       command -v "$OPT"   >/dev/null || OPT=opt
LINK="${LINK:-llvm-link-18}"; command -v "$LINK" >/dev/null || LINK=llvm-link

PLUGIN=$(ls build/libSleepability.* 2>/dev/null | head -1 || true)
[[ -z "$PLUGIN" ]] && { echo "error: plugin not built. run ./scripts/build.sh first"; exit 1; }

MODE="${1:-check}"
mkdir -p build/ll tests/expected
run() { "$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability -disable-output "$1" 2>&1; }

pass=0; fail=0
check() { # name  ll-path
  local name="$1" ll="$2" exp="tests/expected/$1.txt"
  local got; got="$(run "$ll")"
  if [[ "$MODE" == "--update" ]]; then printf '%s\n' "$got" > "$exp"; echo "updated  $name"; return; fi
  if [[ -f "$exp" ]] && diff <(printf '%s\n' "$got") "$exp" >/dev/null; then
    echo "PASS  $name"; ((pass++))
  else
    echo "FAIL  $name"; ((fail++))
    [[ "$MODE" == "-v" ]] && { diff <(printf '%s\n' "$got") "$exp" 2>/dev/null || printf '%s\n' "$got"; }
  fi
}

# Single-file tests.
for c in tests/*.c; do
  base="$(basename "$c" .c)"; ll="build/ll/$base.ll"
  "$CLANG" -S -emit-llvm -O0 -o "$ll" "$c" -Itests 2>/dev/null
  check "$base" "$ll"
done

# Cross-TU test: two files linked into one module.
"$CLANG" -S -emit-llvm -O0 -o build/ll/probe.ll  tests/link/probe.c  2>/dev/null
"$CLANG" -S -emit-llvm -O0 -o build/ll/helper.ll tests/link/helper.c 2>/dev/null
"$LINK" -S -o build/ll/linked.ll build/ll/probe.ll build/ll/helper.ll
check "linked" build/ll/linked.ll

# Primitive table extended from a file.
"$CLANG" -S -emit-llvm -O0 -o build/ll/09_primitives_file.ll tests/prim/09_primitives_file.c 2>/dev/null
run() { "$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability \
          -sleepability-primitives=tests/prim/extra.primitives -disable-output "$1" 2>&1; }
check "09_primitives_file" build/ll/09_primitives_file.ll

# Summary file: records, witness chains, and the IR-hash cache.
# Header lines that depend on the toolchain or the path are left out of the diff.
SUM=build/ll/07.clsc_summary; rm -f "$SUM"
run() { "$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability \
          -sleepability-summary="$SUM" -disable-output "$1" >/dev/null 2>&1
        grep -v -E '^# (producer|module|ir-sha256|config-sha256):' "$SUM"; }
check "summary_file" build/ll/07_flag_forwarding.ll
if [[ "$MODE" != "--update" ]]; then
  again="$("$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability \
            -sleepability-summary="$SUM" -disable-output build/ll/07_flag_forwarding.ll 2>&1)"
  if [[ "$again" == "summary up to date"* ]]; then echo "PASS  summary_cache"; ((pass++))
  else echo "FAIL  summary_cache"; ((fail++)); [[ "$MODE" == "-v" ]] && echo "$again"; fi
  # A different configuration (here: an extended primitive table) must not
  # reuse the cached summary, even though the IR is the same.
  other="$("$OPT" -load-pass-plugin "$PLUGIN" -passes=sleepability \
            -sleepability-primitives=tests/prim/extra.primitives \
            -sleepability-summary="$SUM" -disable-output build/ll/07_flag_forwarding.ll 2>&1)"
  if [[ "$other" != "summary up to date"* ]]; then echo "PASS  summary_cache_config"; ((pass++))
  else echo "FAIL  summary_cache_config"; ((fail++)); fi
fi

echo "----"
[[ "$MODE" == "--update" ]] && { echo "expected outputs regenerated"; exit 0; }
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
