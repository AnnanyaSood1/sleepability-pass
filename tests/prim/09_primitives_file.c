/* 09 - The primitive table can be extended from a file (tests/prim/extra.primitives).
 * wait_for_completion and __kmalloc_noprof are not built in. Without the file
 * both callers would be ATOMIC-SAFE; with it:
 *
 *   waiter          MAY SLEEP   (wait_for_completion is declared 'sleep')
 *   alloc_atomic2   ATOMIC-SAFE (__kmalloc_noprof is 'gfp ... 1', GFP_ATOMIC)
 *   alloc_kernel2   MAY SLEEP   (GFP_KERNEL)
 */
#include "../gfp.h"
struct completion;
extern void wait_for_completion(struct completion *c);
extern void *__kmalloc_noprof(size_t size, gfp_t flags);

void waiter(struct completion *c) { wait_for_completion(c); }
void *alloc_atomic2(void) { return __kmalloc_noprof(8, GFP_ATOMIC); }
void *alloc_kernel2(void) { return __kmalloc_noprof(8, GFP_KERNEL); }
