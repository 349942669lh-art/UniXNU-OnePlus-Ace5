/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed Pineapple GPUCC RCG configuration transactions; NOT a PLL/power owner.
 * Caller serializes state and holds a genuine current-boot native guard across
 * the ENTIRE call. Callbacks cannot reenter. Invalidate also requires that
 * serialization. This library cannot catch synchronous external aborts.
 */
#ifndef GIULIA_GPU_RCG_H
#define GIULIA_GPU_RCG_H
#include <stdint.h>
#define GGRCLK_IDENTITY "GIULIA GPU RCG: fixed configuration transactions no-power-authority"
#define GGRCLK_POLLS 500u
enum { GGRCLK_GMU, GGRCLK_HUB, GGRCLK_FF, GGRCLK_ROOTS };
enum { GGRCLK_XO, GGRCLK_GPLL0, GGRCLK_PLL1 };
enum { GGRCLK_OK, GGRCLK_ARGUMENT, GGRCLK_USED, GGRCLK_UNAVAILABLE, GGRCLK_BUSY,
       GGRCLK_LOST, GGRCLK_IO, GGRCLK_READBACK, GGRCLK_TIMEOUT, GGRCLK_QUARANTINED };
enum { GGRCLK_IDLE, GGRCLK_READY, GGRCLK_QUIET, GGRCLK_FORCE, GGRCLK_CONFIG,
       GGRCLK_UPDATE, GGRCLK_RESTORE, GGRCLK_DONE };
struct ggrc_plan {
    /* hz/parent_hz are nominal requests, NOT measured rates. A real PLL source
     * must validate its L/alpha rounding, lock and bounded rate tolerance. */
    uint32_t root,hz,cmd,cfg_mask,cfg_value,parent,parent_hz;
};
struct ggrc_io {
    void *cookie;
    int (*live)(void *); /* CPU-only, 1=guard/backing still live. */
    /* CPU-only, no mutation. Exactly 1 requires stable mapping and ownership
     * of this root, CX/MX and access supplies, old AND new parents pinned,
     * actual parent rate/config established, old/new effective rates >=1 MHz,
     * and no external writers/revocation. This module does not establish
     * those facts, program a PLL, or return a hardware authority grant.
     * A PLL shared by old/new configurations cannot be retuned while feeding
     * a dependent root. The source must first park that root on XO, ensure all
     * other PLL consumers quiet/parked, then retune and retain the PLL.
     * 0=unchanged unavailable; any other result is uncertain. */
    int (*ready)(void *,const struct ggrc_plan *);
    int (*quiescent)(void *,unsigned); /* ALL root dependents quiet: 1. */
    /* Fixed CMD/CFG offsets only, device/uncached mapping. Native writes
     * include DSB. Nonzero means uncertain even if the write took effect. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*wait_us)(void *,unsigned); /* Exactly 1 us, no automatic retry. */
};
struct ggrc_state {
    struct ggrc_io io;
    uint32_t opened,quarantined,root,phase,last_code;
    uint32_t touched_mask,force_added_mask,configured_mask,hz[GGRCLK_ROOTS];
    uint32_t reads,writes,waits,root_polls,update_polls,last_offset,observed;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggrc_identity(void);
/* RAM-only exact plans, no rounding and no arbitrary register/rate interface.
 * XO is a parking configuration for all three roots, not a power-off claim. */
int ggrc_describe(unsigned,uint32_t,struct ggrc_plan *);
int ggrc_open(struct ggrc_state *,const struct ggrc_io *);
/* Bounded force-on -> CFG -> self-clearing UPDATE -> restore own force vote.
 * Preserves an inherited ROOT_EN. No disable, refcounting or source binding.
 * Success means configuration acknowledged, NOT rate measured/branch enabled.
 * Any uncertain operation quarantines, retaining all backing and votes; never
 * rollback/retry/free after uncertainty. Applies also to failed restore. */
int ggrc_apply(struct ggrc_state *,unsigned,uint32_t);
void ggrc_invalidate(struct ggrc_state *);
#ifdef __cplusplus
}
#endif
#endif
