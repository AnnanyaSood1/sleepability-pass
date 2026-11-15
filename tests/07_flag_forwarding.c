/* 07 - Parametric summaries: flags forwarded through wrappers.
 * Shaped like rust/helpers/slab.c in Linux 6.18, where the helper simply
 * forwards the caller's gfp flags to the allocator. The helper itself is
 * IF-FLAGS (may sleep iff its flags argument has the reclaim bit); each caller
 * is then decided by the flags it passes.
 *
 *   rust_helper_krealloc_like  IF-FLAGS (arg 2)
 *   caller_atomic              ATOMIC-SAFE   passes GFP_ATOMIC
 *   caller_kernel              MAY SLEEP     passes GFP_KERNEL
 *   two_level                  IF-FLAGS (arg 1), condition carried up a level
 *   caller_masked              ATOMIC-SAFE   clears the reclaim bit with &
 */
#include "gfp.h"
extern void *kmalloc(size_t size, gfp_t flags);

void *rust_helper_krealloc_like(const void *p, size_t n, gfp_t flags) {
    (void)p;
    return kmalloc(n, flags);
}

void *caller_atomic(void) { return rust_helper_krealloc_like(0, 8, GFP_ATOMIC); }
void *caller_kernel(void) { return rust_helper_krealloc_like(0, 8, GFP_KERNEL); }

void *two_level(size_t n, gfp_t f) { return rust_helper_krealloc_like(0, n, f); }

void *caller_masked(gfp_t f) { return two_level(8, f & ~__GFP_DIRECT_RECLAIM); }
