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

echo "----"
[[ "$MODE" == "--update" ]] && { echo "expected outputs regenerated"; exit 0; }
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
