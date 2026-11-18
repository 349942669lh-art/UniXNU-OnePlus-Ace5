/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed SM8650 GCC enable transactions. Caller supplies current-boot native
 * access, exclusion, parent/supply backing and revocation synchronization.
 * No mapping, authority grant, regulator vote or automatic startup is made.
 */
#ifndef GIULIA_GPU_GCC_H
#define GIULIA_GPU_GCC_H
#include <stdint.h>
#define GGGC_IDENTITY "GIULIA GPU GCC: fixed retained config/axi/memnoc/dvm"
#define GGGC_BASE 0x00171000u
#define GGGC_BYTES 0x1000u
#define GGGC_POLLS 200u
enum { GGGC_CFG, GGGC_AXI, GGGC_MEMNOC, GGGC_DVM, GGGC_CLOCKS };
enum { GGGC_DIRECT, GGGC_SKIP, GGGC_VOTED, GGGC_DELAY };
enum { GGGC_OK, GGGC_ARGUMENT, GGGC_USED, GGGC_UNAVAILABLE, GGGC_LOST,
       GGGC_IO, GGGC_READBACK, GGGC_TIMEOUT, GGGC_QUARANTINED };
struct gggc_branch { uint32_t offset,policy,hwcg_mask; };
struct gggc_io {
    void *cookie;
    int (*live)(void *); /* CPU-only; anything but 1 permanently latches loss. */
    /* CPU-only, stable across all I/O: 1=THIS branch's access, supply, parents
     * and exclusion are held, with effective rate >=1 MHz for the fixed poll
     * bound; 0=unchanged unavailable; other=uncertain. CFG is
     * separately prepared before board-source resolve, never inside resolve.
     * These callbacks cannot reenter the executor or access the hardware. */
    int (*ready)(void *,unsigned);
    /* Page-relative whitelist only: 0x4, 0x154, 0x10, 0x18. Device/uncached
     * mapping; ordered 32-bit access; write includes DSB before returning.
     * Synchronous external aborts are not recoverable by this interface. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*wait_us)(void *,unsigned); /* 1 or 10 us; finite, no hidden retries. */
};
struct gggc_state {
    struct gggc_io io;
    uint32_t opened,quarantined,last_code,branch;
    uint32_t requested_mask,inherited_mask,touched_mask;
    uint32_t reads,writes,waits,polls,last_offset,before,observed;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *gggc_identity(void);
int gggc_describe(unsigned,struct gggc_branch *);
int gggc_open(struct gggc_state *,const struct gggc_io *); /* Zero once. No MMIO. */
/* Enable-only and retained until reset; this is not an aggregate/refcount
 * provider or proof of actual clock rate. Inherited bits are never withdrawn.
 * AXI uses SKIP and does not require a latched enable/halt observation. DVM
 * waits 10 us without polling halt. MEMNOC checks HWCG before bounded halt
 * polling. CFG is an always-enabled direct write in the source's GCC probe.
 * Uncertain callback/write/timeout seals this state, with no rollback/retry.
 * Caller serializes APIs AND all callbacks; backing survives uncertainty. */
int gggc_enable(struct gggc_state *,unsigned);
void gggc_invalidate(struct gggc_state *); /* Serialized CPU-only loss latch. */
#ifdef __cplusplus
}
#endif
#endif
