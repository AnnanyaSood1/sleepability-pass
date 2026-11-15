# Results: the Rust helpers of Linux 6.18

The first run of the pass on real kernel IR (15 November 2026). The input is
`rust/helpers/helpers.ll` from Linux 6.18 (tag `v6.18`), x86-64 `defconfig`,
built with clang 18 at the kernel's normal optimisation level (`-O2`). The
helpers are the C side of the Rust-to-C boundary: Rust code reaches most
non-trivial kernel C through them. Reproduce with `scripts/kernel_helpers.sh`.

| Run | Primitive table | May sleep | Conditional | Unknown | Atomic-safe |
| --- | --- | ---: | ---: | ---: | ---: |
| `helpers.builtin-only.verdicts.txt` | built-in five names only | 2 | 0 | 3 | 342 |
| `helpers.verdicts.txt` | built-in + `config/linux-6.18.primitives` | 20 | 4 | 2 | 321 |

347 functions are defined in the module; 341 of them are `rust_helper_*`.

## What the numbers say

**The built-in name list is not enough on real IR.** With only the five
built-in names, the pass finds 2 sleeping helpers. The helpers call 53 kernel
functions that the built-in table does not know, among them the real
allocators (`krealloc_node_align_noprof`, not `kmalloc`), `down_read`,
`usleep_range_state` and `_copy_from_user`. The primitive table must be
written for the kernel version analysed; `config/linux-6.18.primitives` is that
table, with each entry checked against the 6.18 source.

**Allocation is conditional, as designed.** The four allocator helpers
(`krealloc_node_align`, `kvrealloc_node_align`, `alloc_pages`,
`dma_alloc_attrs`) are IF-FLAGS: they may sleep only if their flags argument
has `__GFP_DIRECT_RECLAIM` (bit 10, 0x400, in 6.18's `gfp_types.h`). A
one-bit summary would have marked all of them as always sleeping.

**Indirect calls are visible, not hidden.** `rust_helper_poll_wait` and
`rust_helper_fwnode_handle_put` call through function pointers and are
UNKNOWN. `rust_helper_devm_add_action_or_reset` also has an indirect call, but
it is MAY SLEEP because it also reaches `__devm_add_action`, which allocates
with `GFP_KERNEL`.

**The spinlock helpers are atomic-safe.** `rust_helper_spin_lock` and
`rust_helper_spin_unlock` call `_raw_spin_lock` and `_raw_spin_unlock`, which
are not in the sleep set. This is the intended rule: involuntary preemption
when preemption is re-enabled is not a voluntary sleep (see `docs/DESIGN.md`).

## What this run does not show

- **Completeness of the table.** 37 external callees remain unmodelled (they are
  listed at the end of `helpers.clsc_summary`). Most do not sleep. Some may in
  some paths (`put_device`, `dma_free_attrs`, `__mmdrop`, `drm_gem_object_free`
  can run release callbacks), and the pass treats them as non-sleeping. That is
  a soundness gap of this table, stated here rather than hidden.
- **Inline assembly.** The module contains 837 inline-asm call sites. The pass
  assumes none of them calls the scheduler.
- **Other configurations.** One configuration only. With
  `CONFIG_DEBUG_ATOMIC_SLEEP` or lockdep, other primitives (`__might_sleep`,
  `mutex_lock_nested`) appear and the table must include them.
- **Precision.** I have not yet checked every MAY SLEEP verdict against the
  source by hand; the 20 are listed in `helpers.verdicts.txt` for that review.
- **Bugs.** This run summarises the C side only. Finding violations needs the
  Rust side and the join (the `guardstate` repository).
