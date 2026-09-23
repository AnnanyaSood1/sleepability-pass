/* 04 - Argument sensitivity (the precision control from the proposal).
 * kmalloc's sleepability depends on its gfp flags. The GFP_KERNEL call MUST be
 * flagged; the GFP_ATOMIC call MUST NOT. */
#include "gfp.h"
extern void *kmalloc(size_t size, gfp_t flags);

void *alloc_sleepy(size_t n) {           /* GFP_KERNEL  -> MAY SLEEP    */
    return kmalloc(n, GFP_KERNEL);
}

void *alloc_atomic(size_t n) {           /* GFP_ATOMIC  -> ATOMIC-SAFE  */
    return kmalloc(n, GFP_ATOMIC);
}
