#ifndef TEST_GFP_H
#define TEST_GFP_H
/* Self-contained gfp flag model that mirrors the sleep-relevant kernel bit.
 * The pass keys off __GFP_DIRECT_RECLAIM (0x400): GFP_KERNEL sets it (the
 * allocator may enter direct reclaim and sleep); GFP_ATOMIC does not (the
 * allocation is atomic-safe). Bit values are illustrative but the KERNEL/ATOMIC
 * distinction on the reclaim bit matches real kernel semantics. */
#define __GFP_HIGH            0x001u
#define __GFP_IO              0x040u
#define __GFP_FS              0x080u
#define __GFP_DIRECT_RECLAIM  0x400u   /* the sleep-relevant bit */
#define __GFP_KSWAPD_RECLAIM  0x800u

#define GFP_ATOMIC   (__GFP_HIGH | __GFP_KSWAPD_RECLAIM)                                   /* 0x801 */
#define GFP_KERNEL   (__GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM | __GFP_IO | __GFP_FS)   /* 0xCC0 */

typedef unsigned int  gfp_t;
typedef unsigned long size_t;
#endif
