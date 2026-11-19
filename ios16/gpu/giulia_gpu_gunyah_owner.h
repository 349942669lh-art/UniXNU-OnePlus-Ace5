/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_GUNYAH_OWNER_H
#define GIULIA_GPU_GUNYAH_OWNER_H
#include "giulia_gpu_gunyah_boot.h"
#include "giulia_gpu_gunyah_irq.h"
#include "giulia_gpu_gunyah_session.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { GGOWN_OK=0,GGOWN_ARGUMENT=1,GGOWN_BUSY=2,GGOWN_USED=3,
    GGOWN_LOST=4,GGOWN_IRQ=5,GGOWN_HYPERVISOR=6,GGOWN_SESSION=7,
    GGOWN_IDLE=8,GGOWN_BUDGET=9 };
enum { GGOWN_CONFIGURED=1,GGOWN_CLAIM=2,GGOWN_COMPATIBILITY=3,
    GGOWN_CHANNEL=4,GGOWN_DISCOVERY=5,GGOWN_READY=6,GGOWN_FAILED=7 };
struct ggown_policy {
    void *cookie;
    /* Actual current-boot owning-worker authorization, pinned lifetime and
     * stopping state. Return exactly 1. This is kernel policy, not a report,
     * token, caller-supplied capability, or permission to share memory.
     * live and now_us must be non-sleeping: HVC validation calls them with
     * local interrupts disabled. No callback may replace the descriptor. */
    int (*live)(void *,uint64_t);
    uint64_t (*now_us)(void *);
    int (*delay_us)(void *,unsigned); /* Always outside the IRQ guard. */
    int (*notification)(void *,uint32_t,const void *,size_t); /* Likewise. */
};
struct ggown_status {
    unsigned initialized,busy,bootstrapped,compatible,irq_claimed,invalid;
    unsigned stage,last_result,probe_result,session_result,query_step;
    unsigned next_sequence,rm_quarantined,hvc_calls;
    uint64_t epoch,native_code,api_info;
    unsigned self_attempted,self_observed,self_vmid,self_rpc_result,self_remote_error;
};
/* Allocate/zero this large object on the heap. Pin the object, its callback
 * code/cookies, and the IOService owner until reboot, even after failure.
 * All APIs except invalidate run on one serialized owning worker. The GIC
 * implementation additionally requires the actual boot CPU and one-CPU
 * topology, rejecting generic interrupt-handler coexistence.
 * No report grants the runtime policy or IRQ reservation: bootstrap executes
 * the actual native IRQ claim and canonical hypervisor probe separately.
 * Fields are private to these APIs; diagnostics are copied with get_status.
 * Never reset/reinitialize a consumed or invalid state. */
struct ggown_state {
    unsigned initialized,busy,loss,invalid,attempted,bootstrapped;
    unsigned compatible,irq_claimed,guard_active,guard_open,stage,last_result;
    unsigned probe_result,session_result,hvc_calls,timing,time_failed,timeout_us;
    uint64_t epoch,start_us,last_us;
    unsigned self_attempted,self_observed,self_vmid,self_rpc_result,self_remote_error;
    uint32_t tx_intid,rx_intid;
    struct ggr_report report;
    struct ggown_policy policy;
    struct ggh_hvc_probe compatibility_probe;
    struct ggh_hvc_compatibility compatibility;
    struct ggh_hvc hvc;
    struct ggrm_io io;
    struct ggrs_session session;
};
/* Init only validates/copies; no HVC, IRQ claim, queue receive or assignment. */
int ggown_init(struct ggown_state *,const struct ggr_report *,const struct ggown_policy *);
/* Bootstrap claims IRQs, guards the canonical UID/API probe and initializes
 * one RM sequence space. It performs no RM query, memory lookup or share. */
int ggown_bootstrap(struct ggown_state *,unsigned timeout_us);
/* One fixed VM_GET_VMID request on the same serialized RM stream. May
 * bootstrap first. Preserves sequence numbering for later discovery.
 * The historical identity is reported through get_status, independently
 * of peer discovery, parcel lookup and every GPU resource assignment.
 * Failure retains the channel and invalidates the owner until reset. */
int ggown_query_self(struct ggown_state *,unsigned timeout_us);
/* If needed, bootstrap and all seven discovery queries share one deadline.
 * An explicit later probe after bootstrap uses the existing RM sequence.
 * Query failure, including a missing parcel, quarantines this attempt. */
int ggown_probe(struct ggown_state *,unsigned timeout_us);
int ggown_poll(struct ggown_state *,unsigned frame_budget,unsigned timeout_us);
int ggown_copy_snapshot(struct ggown_state *,struct gg_rq_snapshot *);
/* Historical diagnostics only: no IRQ/HVC access or lease refresh. */
int ggown_get_status(struct ggown_state *,struct ggown_status *);
/* Kernel-only borrowing for a future primary-sharing transaction BEFORE
 * discovery. The same worker must serialize all use and retain this exact
 * RM state; never duplicate/reset its sequence space. Separate real primary
 * source-ownership authorization is still mandatory for any assignment.
 * A missing read-only MEM_LOOKUP must not trigger an implicit assignment. */
int ggown_get_channel(struct ggown_state *,struct ggrm_io *,struct ggrm_state **);
/* Stop must first set its own atomic stopping latch, then call this before
 * joining the worker. Loss is latched first, then actual IRQ/session state
 * is invalidated. A busy worker only consumes the latch; memory stays pinned.
 * This API never waits for the worker or frees resources. */
int ggown_invalidate(struct ggown_state *);

#ifdef __cplusplus
}
#endif
#endif
