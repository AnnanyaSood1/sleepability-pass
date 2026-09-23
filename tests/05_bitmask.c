/* 05 - Stretch: intraprocedural known-bits over gfp flags.
 * `extra | GFP_KERNEL` provably sets the reclaim bit whatever `extra` is, so
 * the wrapper MUST be caught (MAY SLEEP). `extra | GFP_ATOMIC` is NOT provably
 * reclaim-free (the caller could pass a reclaim flag in `extra`), so soundness
 * requires the conservative MAY SLEEP verdict -- see README on the limitation. */
#include "gfp.h"
extern void *kmalloc(size_t size, gfp_t flags);

void *wrap_alloc(size_t n, gfp_t extra) {        /* extra | GFP_KERNEL -> MAY SLEEP */
    return kmalloc(n, extra | GFP_KERNEL);
}

void *wrap_alloc_unknown(size_t n, gfp_t extra) {/* extra | GFP_ATOMIC -> conservatively MAY SLEEP */
    return kmalloc(n, extra | GFP_ATOMIC);
}
