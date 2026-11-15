# Sleepability Pass: a C-side sleepability summary for the Linux kernel

[![build-and-test](https://github.com/AnnanyaSood1/sleepability-pass/actions/workflows/ci.yml/badge.svg)](https://github.com/AnnanyaSood1/sleepability-pass/actions/workflows/ci.yml)

An out-of-tree **LLVM module analysis pass** (new pass manager) that decides,
for every function in a module, whether it **may sleep**: whether it can,
directly or through its callees, reach a blocking primitive. A caller in
atomic context (holding a spinlock, in an interrupt handler) must not make
such a call.

This is the C side of **CLSC, the Context-Lattice Sleepability Checker**. The
Rust side, which tracks the atomic context, and the join at the foreign call
site are in the companion repository
[`guardstate`](https://github.com/AnnanyaSood1/guardstate), which reads the
summary file this pass writes.

```
$ opt -load-pass-plugin build/libSleepability.so -passes=sleepability \
      -disable-output build/ll/07_flag_forwarding.ll

  [ATOMIC-SAFE]  caller_atomic              -- no path to a blocking primitive
  [MAY SLEEP  ]  caller_kernel              -- calls rust_helper_krealloc_like() with direct-reclaim gfp flags (may sleep)
  [ATOMIC-SAFE]  caller_masked              -- no path to a blocking primitive
  [IF-FLAGS   ]  rust_helper_krealloc_like  -- may sleep iff arg 2 has the reclaim bit (passed to kmalloc())
  [IF-FLAGS   ]  two_level                  -- may sleep iff arg 1 has the reclaim bit (passed to rust_helper_krealloc_like())

summary: 1 may-sleep, 2 conditional, 0 unknown, 2 atomic-safe (5 functions analysed)
```

## What it computes

Each function gets one of four verdicts:

| Verdict | Meaning |
| --- | --- |
| `ATOMIC-SAFE` | no path reaches a blocking primitive |
| `IF-FLAGS` | may sleep **iff** a given argument has the gfp direct-reclaim bit |
| `UNKNOWN` | reaches a call through a function pointer, which the pass cannot resolve |
| `MAY SLEEP` | some path reaches a blocking primitive, whatever the arguments |

`IF-FLAGS` is what makes allocation decidable. In Linux 6.18 the Rust helper
for allocation forwards the caller's flags to the allocator; the helper is
IF-FLAGS, and each call is decided by the flags passed at that call.

## How it works

1. **Primitives.** Blocking primitives and allocators are matched by name. The
   built-in table is `mutex_lock`, `schedule`, `msleep`, `might_sleep` and
   `kmalloc`; `-sleepability-primitives=FILE` extends it for a real kernel
   (`config/linux-6.18.primitives`).
2. **Flags.** For an allocator call the pass asks whether the flags carry
   `__GFP_DIRECT_RECLAIM` (0x400 in Linux 6.18). Constants are decided exactly;
   `x | GFP_KERNEL` always has the bit; `x & ~__GFP_DIRECT_RECLAIM` never does;
   a parameter makes the function IF-FLAGS on that parameter; anything else is
   conservatively MAY SLEEP. These are a small set of known-bits rules, not
   LLVM's general known-bits analysis.
3. **Propagation.** Summaries are propagated bottom-up over the call graph's
   strongly connected components (`llvm::scc_iterator`), iterating inside each
   SCC until nothing changes, so recursion and mutual recursion are handled and
   conditions are carried through any number of wrappers.
4. **Output.** A deterministic per-function report, and optionally a
   `.clsc_summary` file with a witness chain for each function.

`docs/DESIGN.md` gives the lattice, the transfer, the termination and
soundness arguments, and the boundaries.

## Build

Requires LLVM/Clang 18 (developed on Ubuntu 24.04) and CMake 3.20 or newer.

```sh
# Ubuntu: sudo apt-get install llvm-18-dev clang-18 cmake ninja-build
./scripts/build.sh
```

`build.sh` finds `llvm-config`; override with `LLVM_CONFIG=llvm-config-17
./scripts/build.sh`. The result is `build/libSleepability.so`.

## Run the tests

```sh
./scripts/run_tests.sh            # compile each test to IR, run, diff against expected
./scripts/run_tests.sh -v         # show diffs on failure
./scripts/run_tests.sh --update   # regenerate tests/expected/
```

```
PASS  01_direct_mutex
PASS  02_chain
PASS  03_nonsleeping
PASS  04_gfp
PASS  05_bitmask
PASS  06_recursion
PASS  07_flag_forwarding
PASS  08_indirect
PASS  linked
PASS  09_primitives_file
PASS  summary_file
PASS  summary_cache
PASS  summary_cache_config
----
13 passed, 0 failed
```

| Test | What it checks | Expected |
| --- | --- | --- |
| `01_direct_mutex` | a `rust_helper_*`-style shim forwarding to a primitive | MAY SLEEP |
| `02_chain` | transitivity, three levels deep | all MAY SLEEP |
| `03_nonsleeping` | genuinely atomic-safe code, including `memcpy` | all ATOMIC-SAFE |
| `04_gfp` | `GFP_KERNEL` against `GFP_ATOMIC`, the precision control | MAY SLEEP / ATOMIC-SAFE |
| `05_bitmask` | `extra \| GFP_KERNEL` always has the bit; `extra \| GFP_ATOMIC` has it iff `extra` does | MAY SLEEP / IF-FLAGS |
| `06_recursion` | SCC collapse: a sleeping cycle and a clean cycle | MAY SLEEP / ATOMIC-SAFE |
| `07_flag_forwarding` | flags forwarded through two wrappers, shaped like `rust/helpers/slab.c` | per caller: safe, sleeps, masked |
| `08_indirect` | a call through a function pointer | UNKNOWN, never silently safe |
| `linked` | cross-translation-unit propagation after `llvm-link` | MAY SLEEP |
| `09_primitives_file` | the primitive table extended from a file | MAY SLEEP / ATOMIC-SAFE |
| `summary_file` | the `.clsc_summary` records and witness chains | frozen file |
| `summary_cache` | an unchanged module is not recomputed | "summary up to date" |
| `summary_cache_config` | the same module with a different primitive table is recomputed | not reused |

## The summary file

```sh
opt -load-pass-plugin build/libSleepability.so -passes=sleepability \
    -disable-output -sleepability-summary=helpers.clsc_summary helpers.ll
```

```
# clsc-summary 1
# ir-sha256: 2fd44762...
# reclaim-mask: 0x400
if_arg    rust_helper_krealloc_node_align arg=3 mask=0x400 via=rust_helper_krealloc_node_align>krealloc_node_align_noprof
may_sleep rust_helper_mutex_lock via=rust_helper_mutex_lock>mutex_lock
unknown   rust_helper_poll_wait via=rust_helper_poll_wait>(indirect)
atomic_safe rust_helper_spin_unlock
```

If the file already exists for the same IR and the same configuration (the
SHA-256 of each is in the header), the pass does not recompute it. `guardstate` reads this file.

| Option | Default | Meaning |
| --- | --- | --- |
| `-sleepability-summary=FILE` | none | write the summary file |
| `-sleepability-primitives=FILE` | none | extend the primitive table (`sleep NAME`, `gfp NAME ARGINDEX`) |
| `-sleepability-reclaim-mask=N` | `0x400` | the direct-reclaim bit of the analysed kernel |
| `-sleepability-indirect=unknown\|ignore` | `unknown` | `ignore` treats function pointers as safe (unsound; for comparison only) |

## Results on Linux 6.18

`results/linux-6.18/` is the first run on real kernel IR: the Rust helpers
(`rust/helpers/helpers.ll`, x86-64 `defconfig`, clang 18, `-O2`). Reproduce with
`./scripts/kernel_helpers.sh /path/to/linux`.

| Primitive table | May sleep | Conditional | Unknown | Atomic-safe |
| --- | ---: | ---: | ---: | ---: |
| built-in only | 2 | 0 | 3 | 342 |
| with `config/linux-6.18.primitives` | 20 | 4 | 2 | 321 |

The first row shows that a name list written for test inputs is not enough on
real IR; the second shows the four allocator helpers coming out conditional on
their flags argument, as designed. The results README states what the run does
not show, including the 37 external functions left unmodelled.

## Limitations

- **The primitive table is configuration.** Sleeping externals that are not in
  the table are assumed not to sleep. The summary file lists every unmodelled
  external so the gap is visible.
- **Function pointers are UNKNOWN**, not resolved, on the C side.
- **Inline assembly** is assumed not to enter the scheduler.
- **One verdict per function**, beyond the argument conditions; one kernel
  configuration per summary.

## Repository layout

```
src/SleepabilityPass.cpp      the pass
config/linux-6.18.primitives  primitive table for Linux 6.18, checked against the source
results/linux-6.18/           first run on the kernel's Rust helpers
scripts/build.sh              configure and build
scripts/run_tests.sh          compile tests to IR, run, diff against expected
scripts/kernel_helpers.sh     reproduce the kernel results
tests/*.c, tests/link, tests/prim   test inputs
tests/expected/               frozen expected outputs
docs/DESIGN.md                design: lattice, transfer, soundness, boundaries
```

## Authorship

**Author:** Annanya Sood — <annanyas0142@gmail.com>

I scoped this project and own its design. The decisions are mine: to build the
C side as a standalone artifact that answers one well-posed question rather
than a partial version of the whole checker; to make argument sensitivity the
target, since `kmalloc(size, flags)` is where a name-based analysis stops being
sufficient and the property becomes a question about a *value*; to propagate
bottom-up over SCCs so that recursion and mutual cycles collapse to one verdict;
to over-approximate to MAY SLEEP wherever atomic-safety cannot be proven, so the
pass fails toward false positives rather than silent misses; and to pair each
test with a near-miss control, so the suite demonstrates precision and not only
recall. The limitations section is mine too, and states what the artifact does
not do.

The implementation — the LLVM pass, the CMake build, the test harness, CI, and
this documentation — was written with AI assistance (Claude, by Anthropic)
working to that direction. The analysis design, its soundness argument, and its
stated boundaries are set out in [`docs/DESIGN.md`](docs/DESIGN.md). I can
account for each component and the reasoning behind it, and I take
responsibility for the artifact as published.

The figures in `docs/img/` are AI-rendered from `docs/DESIGN.md`.

## License

MIT — see `LICENSE`.
