/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pineapple GPUCC branch transactions. Not a PLL, regulator or power owner.
 * Caller serializes this state for its entire lifetime and retains the actual
 * current-boot GPUCC mapping, branch exclusion, parent rates and supplies.
 * Callbacks must not reenter this executor. Invalidate also requires the
 * caller's serialization; an atomic source-side live flag may revoke access.
 * No platform source is created by these functions or by register readback.
 */
#ifndef GIULIA_GPU_GPUCC_H
#define GIULIA_GPU_GPUCC_H
#include <stdint.h>
#define GGCB_IDENTITY "GIULIA GPU GPUCC: fixed branch transactions no-power-authority"
#define GGCB_BASE 0x03d90000u
#define GGCB_BYTES 0x9800u
#define GGCB_POLLS 200u
enum { GGCB_AHB, GGCB_GMU, GGCB_CXO, GGCB_SMMU, GGCB_HUB_AON,
       GGCB_HUB_CX_INT, GGCB_CLOCKS };
enum { GGCB_HALT, GGCB_VOTED, GGCB_DELAY };
enum { GGCB_OK, GGCB_ARGUMENT, GGCB_USED, GGCB_UNAVAILABLE, GGCB_BUSY,
       GGCB_INHERITED, GGCB_PINNED, GGCB_NOT_OWNED, GGCB_LOST,
       GGCB_IO, GGCB_READBACK, GGCB_TIMEOUT, GGCB_QUARANTINED };
enum { GGCB_PHASE_IDLE, GGCB_PHASE_READY, GGCB_PHASE_QUIET, GGCB_PHASE_READ,
       GGCB_PHASE_WRITE, GGCB_PHASE_DELAY, GGCB_PHASE_POLL, GGCB_PHASE_DONE };
struct ggcb_branch { uint32_t offset,policy,aon; };
struct ggcb_io {
    void *cookie;
    /* CPU-only. Backing/exclusion MUST remain reserved across all calls and
     * after uncertainty; 1 is live, anything else latches loss. */
    int (*live)(void *);
    /* CPU-only, no mutation. 1 proves THIS branch's parent configuration,
     * effective clock >=1 MHz, CX/supplies, safe access and native ownership
     * are already established. 0 is unchanged unavailable; else uncertain.
     * This executor does not program rates, PLLs, GDSC, RPMh, ACD or CFG. */
    int (*ready)(void *,unsigned);
    int (*quiescent)(void *,unsigned); /* 1=dependents quiet; 0=busy. */
    /* Only the six fixed offsets are ever passed. Native write includes DSB.
     * CPU mapping must be device/uncached. Return observed I/O errors; this
     * API does NOT recover an unrecoverable synchronous external abort. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*wait_us)(void *,unsigned); /* Only 1 or 10 us, no implicit retries. */
};
struct ggcb_state {
    struct ggcb_io io;
    uint32_t opened,quarantined,branch,phase,last_code;
    uint32_t requested_mask,changed_mask,inherited_mask,touched_mask,pinned_mask;
    uint32_t reads,writes,waits,polls,last_offset,before,observed;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggcb_identity(void);
int ggcb_describe(unsigned,struct ggcb_branch *);
/* Zero-initialize once; no MMIO, allocation or source registration. */
int ggcb_open(struct ggcb_state *,const struct ggcb_io *);
/* Idempotent per-branch request, NOT a refcount/aggregate client API. The real
 * source retains its client votes externally. Never clear inherited votes.
 * AON disable returns PINNED, not a fake successful power-off. After any
 * uncertain callback/write/poll, no further I/O and no optimistic rollback.
 * AHB DELAY does not require a latched vote/halt bit, per branch policy.
 * VOTED disable confirms our vote cleared but does NOT claim clock OFF. */
int ggcb_apply(struct ggcb_state *,unsigned,unsigned);
void ggcb_invalidate(struct ggcb_state *); /* CPU-only, no hardware cleanup. */
#ifdef __cplusplus
}
#endif
#endif
