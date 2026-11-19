/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed Ace5 CX/GDSC and clock composition. Requires genuine current-boot
 * authority, voltage/parent/bandwidth backing and native activation.
 */
#ifndef GIULIA_GPU_BOARD_SOURCE_H
#define GIULIA_GPU_BOARD_SOURCE_H
#include "giulia_gpu_power_owner.h"
#include "giulia_gpu_rcg.h"
#include "giulia_gpu_pll1.h"
#include "giulia_gpu_gpucc.h"
#include "giulia_gpu_gcc.h"
#include "giulia_gpu_cx.h"
#define GGBS_IDENTITY "GIULIA GPU BOARD SOURCE: fixed clocks upstream-required retained-test-only"
enum { GGBS_OK, GGBS_ARGUMENT, GGBS_USED, GGBS_UNAVAILABLE, GGBS_LOST,
       GGBS_CLOCK, GGBS_PROTOCOL, GGBS_RETAINED, GGBS_QUARANTINED };
struct ggbs_io {
    void *cookie;
    /* Real native CPU-only facts under the upstream session guard. Same
     * semantics as the corresponding executor callbacks, NOT constant-true
     * stubs. Includes stable GPUCC mapping, CX/access supplies, old/new
     * parents, PLL calibration and ALL PLL1 consumers parked when required.
     * AHB additionally needs its existing HUB divider configuration retained.
     * These callbacks do not map, prepare, vote, reset, or mutate hardware. */
    int (*rcg_ready)(void *,const struct ggrc_plan *);
    int (*root_quiet)(void *,unsigned);
    int (*pll_ready)(void *,const struct ggp1_plan *);
    int (*pll_parked)(void *);
    int (*branch_ready)(void *,unsigned);
    int (*branch_quiet)(void *,unsigned);
    /* GPUCC-relative, device/uncached native access, write includes DSB.
     * This composition enforces an additional fixed register whitelist.
     * I/O cannot recover from a synchronous external abort. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    /* Separately mapped existing GCC scope 0x171000/0x1000. Ready is CPU-only
     * proof of this branch's stable access, parents and supply backing under
     * the same upstream guard. These are NOT GPUCC-relative callbacks. */
    int (*gcc_ready)(void *,unsigned);
    int (*gcc_read32)(void *,uint32_t,uint32_t *);
    int (*gcc_write32)(void *,uint32_t,uint32_t);
    /* CPU-only CX software-control assignment and retained supply/CFG/ACD
     * proof. Global quiescence is checked through upstream.power.quiescent. */
    int (*cx_ready)(void *);
    /* Explicit upstream consumer binding. Zero preserves the existing
     * HUB_AON route. Only GGCB_HUB_AON and GGCB_HUB_CX_INT are accepted.
     * The two branches have distinct registers and are never aliases. */
    uint32_t hub_branch;
};
struct ggbs_ticket { uint64_t serial,upstream; uint32_t held,local; };
struct ggbs_state {
    struct ggpo_source upstream;
    struct ggbs_io io;
    struct ggpo_grant grant;
    struct ggrc_state rcg;
    struct ggp1_state pll;
    struct ggcb_state branches;
    struct gggc_state gcc;
    struct ggcx_state cx;
    struct ggbs_ticket tickets[GGR_COUNT];
    uint64_t serial;
    uint32_t initialized,take_attempted,active,quarantined,lost,retained;
    uint32_t bind_attempted,bound,resource,last_code,clock_code;
    uint32_t attempted_mask,held_mask,reads,writes;
};
struct ggbs_status {
    uint64_t epoch;
    uint32_t bound,active,quarantined,lost,retained,held_mask;
    uint32_t attempted_mask,last_code,clock_code,reads,writes;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggbs_identity(void);
/* Zero once. Copies tables only, no registration, mapping or I/O. The real
 * upstream must own current-boot handoff + durable CFG/ACD/parent/voltage/
 * bandwidth/reset/IRQ/SID backing. CX/GDSC and AXI/MEMNOC/DVM execute
 * locally in the assigned GPUCC/GCC scopes. CFG remains
 * an upstream preparation prerequisite, not a hidden resolve mutation.
 * Its release/drop must honor retention/quarantine. No Boot Services pointers,
 * grants synthesized from FDT, or Android state snapshots qualify.
 * Caller serializes all APIs/callbacks except the atomic invalidate latch.
 * State, callbacks/cookies AND their native backing survive until reset after
 * retention/uncertainty; IOService retain alone does not establish this. */
int ggbs_init(struct ggbs_state *,const struct ggpo_source *,const struct ggbs_io *);
int ggbs_export(struct ggbs_state *,struct ggpo_source *);
/* Called only by a real upstream native IOService, never auto-started or
 * invoked from USB/ADT/boot-arg matching. Allocates the existing owner lock
 * and registers the composed source. Registration is NOT a hardware grant. */
int giulia_gpu_board_source_register(void *,struct ggbs_state *);
void ggbs_invalidate(struct ggbs_state *); /* Atomic loss latch only. */
int ggbs_status(const struct ggbs_state *,struct ggbs_status *); /* Serialized CPU snapshot. */
/* This is a one-shot RETAINED test composition. Once any local CX/clock
 * transaction is attempted, release/drop refuse BEFORE any hardware I/O or
 * upstream withdrawal. It cannot claim clean shutdown of PLL/AON clocks.
 * Error/stop pins the entire grant/backing until reset. This limitation must
 * be independently accepted and supported by a genuine source before a
 * power-only image could be admitted; this library does not admit an image. */
#ifdef __cplusplus
}
#endif
#endif
