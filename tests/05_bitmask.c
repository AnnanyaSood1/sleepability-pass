/* 05 - Known-bits rule over gfp flags.
 * `extra | GFP_KERNEL` provably sets the reclaim bit whatever `extra` is, so
 * the wrapper MUST be caught (MAY SLEEP). `extra | GFP_ATOMIC` has the bit iff
 * `extra` has it, so the wrapper is IF-FLAGS on its argument 1: each caller is
 * decided by what it passes (see test 07). */
#include "gfp.h"
extern void *kmalloc(size_t size, gfp_t flags);

void *wrap_alloc(size_t n, gfp_t extra) {        /* extra | GFP_KERNEL -> MAY SLEEP */
    return kmalloc(n, extra | GFP_KERNEL);
}

void *wrap_alloc_unknown(size_t n, gfp_t extra) {/* extra | GFP_ATOMIC -> IF-FLAGS (arg 1) */
    return kmalloc(n, extra | GFP_ATOMIC);
}
