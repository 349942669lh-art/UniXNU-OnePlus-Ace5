/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pineapple APPS DRV2 native executor. NOT a resource ownership grant.
 * No auto-registration, terminal input or boot-argument activation.
 */
#ifndef GIULIA_GPU_RSC_NATIVE_H
#define GIULIA_GPU_RSC_NATIVE_H
#include "giulia_gpu_rsc.h"
#define GGRN_ABI 1u
#define GGRN_IDENTITY "GIULIA GPU RSC NATIVE: guarded mapping unbound"
enum { GGRN_OK=0, GGRN_ARGUMENT, GGRN_UNAVAILABLE, GGRN_USED, GGRN_BUSY,
       GGRN_PENDING, GGRN_QUARANTINED, GGRN_PROTOCOL };
struct ggrn_scope {
    uint64_t drv_base;
    uint32_t drv_bytes,drv_id,tcs_offset,intid,active_mask,reserved;
    struct ggrsc_config transport;
};
struct ggrn_authority {
    uint32_t abi,bytes;
    void *cookie;
    /* 1 + nonzero grant: a durable native grant for the ENTIRE session.
     * 0 + zero grant: unchanged denial. Anything else is uncertain: retain.
     * The source must establish the exact current-boot DRV/legacy ACTIVE
     * allocation, exclusive mapping delegation, target IRQ routing/enable,
     * stable access/clocks, no solver/HW-channel use, real Command DB address
     * and selector mask, and serialized ARC aggregation. NO grant by DT,
     * register value, software ledger, retained IOService or UEFI READY.
     * No foreign TCS writer/IRQ handler or upstream backing removal until a
     * clean drop. This is a durable ownership reservation, NOT a thread-bound
     * mutex held across returns. No callback may reenter this executor. */
    int (*take)(void *,const struct ggrn_scope *,uint64_t *grant);
    int (*live)(void *,uint64_t epoch,uint64_t grant); /* CPU-only, exactly 1 */
    /* CPU-only approval that THIS selector is the current serialized ARC
     * aggregate for this grant. 1=approve; 0=unchanged busy; else uncertain.
     * The aggregate must remain reserved through actual command completion;
     * timeout does not free that reservation. No stale snapshot submission. */
    int (*approve)(void *,const struct ggrn_scope *,uint64_t grant,uint32_t selector);
    /* CPU-only proof: no dependent uses remain and the last completed ARC
     * request has been reconciled with the persistent aggregation owner.
     * Completion alone never withdraws an ARC vote. No backing/vote mutation
     * here. 1=safe to close; 0=busy; else uncertain. Called also for serial=0. */
    int (*settled)(void *,const struct ggrn_scope *,uint64_t grant,
                   uint64_t completed,uint32_t selector);
    /* Called only after clean transport close and alias unmapping, while
     * software exclusion/source remain retained. 0 confirms grant removal.
     * Any other result retains source/exclusion in quarantine, without retry.
     * take/live/drop backing must be kernel-resident after ExitBootServices. */
    int (*drop)(void *,const struct ggrn_scope *,uint64_t grant);
};
struct ggrn_status {
    uint64_t epoch,grant,serial,completed;
    uint32_t bound,ready_cached,revoked,quarantined,mapped,leased;
    uint32_t selector,inflight,mutation_attempted,trigger_attempted;
    uint32_t last_code,last_transport_code;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *giulia_gpu_rsc_native_identity(void);
int giulia_gpu_rsc_native_initialize(void); /* Allocates only its IOLock. */
/* Process context, one open attempt per boot. source is an actual IOService.
 * Copied callback table; retained source does not make stop() safe. The source
 * MUST keep all backing alive on BUSY/quarantine, including after revoke.
 * config must describe a source-assigned TCS 0..2, NOT a guessed free slot;
 * mapped_bytes must be 0xf300, relative to tcs_base, not the DRV base.
 * No production source/caller is provided by this unit. */
int giulia_gpu_rsc_native_open(void *source,const struct ggrsc_config *,const struct ggrn_authority *);
int giulia_gpu_rsc_native_submit(void *source,uint32_t selector,uint64_t *serial);
int giulia_gpu_rsc_native_poll(void *source,uint64_t serial);
int giulia_gpu_rsc_native_close(void *source);
/* Latches loss then joins the executor. NEVER removes backing or performs
 * hardware cleanup. BUSY during detach is not permission to release backing. */
int giulia_gpu_rsc_native_invalidate(void *source);
int giulia_gpu_rsc_native_status(struct ggrn_status *); /* Cached, CPU-only. */
#ifdef __cplusplus
}
#endif
#endif
