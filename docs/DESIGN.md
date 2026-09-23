# Design notes

This document records the analysis design, why it is correct, and the decisions
behind the scope. It is intentionally precise about what is and is not claimed.

The one-page figure below is a visual index of the same reasoning; the sections
after it are the detail.

![Design rationale, from first principles](img/design-rationale.png)


## Problem

In the Linux kernel some functions **may sleep** (they can call `schedule()` and
yield the CPU). Calling a may-sleep function from **atomic context** — an
interrupt handler, a region holding a spinlock, an RCU read-side section — is a
bug that can deadlock or corrupt the system. Whether a call site is safe depends
on two facts: the *context* it runs in, and whether the *callee* may sleep.

This pass computes the second fact for C code: a per-function `CanSleep`
predicate. It is the C-side summary that the larger Cross-Language Sleepability
Checker (CLSC) consumes; the Rust-side context tracking and the FFI join are out
of scope here.

## The lattice and the transfer

The property is a two-point lattice, `AtomicSafe ⊑ MaySleep`, joined by "or":
a function may sleep if **any** path through it reaches a blocking primitive.

Ground truth ("atoms") comes from leaf primitives:

- Unconditional blockers, recognised by name: `mutex_lock`, `schedule`,
  `msleep`, `might_sleep`. These are external declarations; a direct call to one
  is an immediate `MaySleep`.
- The allocator `kmalloc`, which is *argument sensitive* (below).

The transitive rule: `CanSleep(f) = directBlocks(f) ∨ ∃ g ∈ callees(f). CanSleep(g)`.

## Why SCCs, and why one bottom-up pass suffices

Reachability over a call graph is a least fixpoint. The clean way to compute it
is to condense the graph into its strongly connected components — the condensation
is a DAG — and evaluate the DAG in reverse-topological order.

`llvm::scc_iterator` over `llvm::CallGraph` yields SCCs exactly in that order:
**every callee's SCC is finalised before its callers' SCC**. (This is the same
bottom-up traversal the CGSCC pass manager relies on.) So a single sweep is
enough; no outer iteration to a fixpoint is needed *across* SCCs.

Within one SCC the members are mutually reachable, so they must share a verdict.
The pass computes that shared verdict as: *does any member of the SCC either
(a) directly call a blocking primitive, or (b) call a function in an
already-finalised lower SCC that is `MaySleep`?* If so, the whole SCC is
`MaySleep`.

**Soundness sketch.** Every source of sleeping is a leaf primitive, which is an
external declaration and therefore never a defined member of an SCC. So any
sleeping path out of an SCC either (a) ends at a primitive called directly by
some member — caught by the direct scan over all members — or (b) leaves through
an edge to a lower, already-finalised SCC — caught by the callee check. A path
that "leaves and returns" cannot exist, because a returning path would place the
target in the same SCC by definition. Hence checking (a) and (b) over all
members captures every sleeping path, and the single bottom-up sweep is exact for
this lattice. Recursive cycles are handled for free: they are just non-trivial
SCCs.

There is a completeness safety net after the SCC sweep: `scc_iterator` starts
from the call graph's external node and so may not visit an *uncalled internal*
function. A short worklist pass assigns verdicts to any function the sweep left
untouched (and iterates for cycles among them). In the test suite every function
is reachable, so this net is a no-op there; it exists for robustness on arbitrary
modules.

## Argument sensitivity for `kmalloc`

Name-only recognition would be wrong for `kmalloc`: the same callee may or may
not sleep depending on its gfp flags. The real distinction is the
`__GFP_DIRECT_RECLAIM` bit — `GFP_KERNEL` sets it (the allocator may enter direct
reclaim and sleep), `GFP_ATOMIC` does not. The pass reads the flags operand and
classifies:

- **Constant** flags → decided exactly by testing the reclaim bit. `GFP_KERNEL`
  ⇒ may sleep; `GFP_ATOMIC` ⇒ atomic-safe.
- **`X | C`** where the constant `C` sets the reclaim bit → may sleep, whatever
  `X` is. An `or` can only set bits, so the result provably carries the bit. This
  is a one-rule instance of known-bits analysis and catches the common wrapper
  pattern `kmalloc(n, extra | GFP_KERNEL)`.
- **Otherwise unknown** flags → **conservatively** may sleep. Reporting
  `AtomicSafe` here would be unsound (the caller could supply reclaim flags), so
  soundness forces the pessimistic verdict. `05_bitmask`'s `wrap_alloc_unknown`
  is exactly this case, and is intentionally recorded as a conservative
  `MAY SLEEP` rather than a false negative.

The reclaim bit value lives in a single constant so it can be re-pointed at the
kernel's real `gfp_types.h`; the test header `tests/gfp.h` models the same
KERNEL-vs-ATOMIC distinction self-containedly so no kernel tree is needed.

## Precision boundaries (deliberate)

- **Indirect calls.** Calls with no statically known callee are skipped. A sound
  extension would either treat them as top (`MaySleep`) or use points-to
  information; both are future work.
- **Interprocedural flag threading.** A pure forwarder
  `wrap(n, f){ return kmalloc(n, f); }` called elsewhere with `GFP_KERNEL` is not
  resolved: that needs argument-value propagation across the call boundary
  (or per-call-site specialisation). This is precisely the constant/bitmask
  propagation earmarked in the proposal, and the current known-bits rule is the
  intraprocedural down payment on it.
- **Context sensitivity.** One verdict per function, independent of call site.

## Why this shape of artifact

The build is a standard out-of-tree new-PM plugin loaded into `opt`, so it needs
no LLVM source tree and no kernel build. Inputs are ordinary `.c` files lowered
with `clang -S -emit-llvm`; whole-program cases use `llvm-link`. Output is
deterministic (sorted by function name) so it can be diffed against frozen
expected files, which is what `scripts/run_tests.sh` does.

## Map to the research proposal

| Proposal element | Status in this artifact |
|------------------|-------------------------|
| Interprocedural C `CanSleep` over the call graph | **implemented** |
| Transitivity / reachability to a fixpoint | **implemented** (bottom-up over SCCs) |
| Recursion / cycles | **implemented** (SCC collapse) |
| Argument-sensitive allocator handling | **implemented** (literal + known-bits) |
| Fault-injection-style test controls (esp. `GFP_ATOMIC`) | **implemented** (`04_gfp`, `05_bitmask`) |
| Full interprocedural constant/bitmask propagation | future work |
| Rust typestate context tracking | future work |
| FFI join across `rust_helper_*` | future work |
