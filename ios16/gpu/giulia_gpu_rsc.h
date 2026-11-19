/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Restricted RPMh ACTIVE TCS transport, NOT a board power/ownership source.
 * No automatic registration, physical address, mapping or terminal entry.
 */
#ifndef GIULIA_GPU_RSC_H
#define GIULIA_GPU_RSC_H
#include <stdint.h>
#define GGRSC_ABI 2u
#define GGRSC_LEGACY_ABI 1u
#define GGRSC_IDENTITY "GIULIA GPU RSC: owned-active-tcs transport unbound"
enum { GGRSC_V27=27, GGRSC_V30=30 };
enum { GGRSC_OK=0, GGRSC_ARGUMENT, GGRSC_USED, GGRSC_BUSY, GGRSC_PENDING,
    GGRSC_LOST, GGRSC_IO, GGRSC_TIMEOUT, GGRSC_PROTOCOL, GGRSC_QUARANTINED };
struct ggrsc_config {
    uint32_t abi,version,mapped_bytes,tcs_id;
    uint32_t arc_address,selector_mask,timeout_us,reserved;
    uint64_t epoch;
    /* ABI 2 optional second ARC binding. Both zero preserves a single target.
     * ABI 1 callers end at epoch and these members MUST NOT be read. */
    uint32_t second_arc_address,second_selector_mask;
};
struct ggrsc_io {
    void *cookie;
    /* Caller already holds a genuine native guard for the WHOLE session:
     * source-owned dedicated ACTIVE TCS (not borrowed sleep/wake/fast path),
     * target IRQ bit/routing, serialized shared-register access, exact ARC
     * binding and current-boot aggregation, retained clocks/access/backing.
     * Guard excludes foreign handlers/writers and upstream revocation during
     * EVERY callback. live is CPU-only and exactly 1; it creates no authority.
     * Callbacks/state/module must remain fixed even after loss/timeout.
     * Process context only, caller-serialized, no reentry. */
    int (*live)(void *,uint64_t);
    /* Offsets relative to provider's authorized tcs_base, not DRV base.
     * 0 success, all other results uncertain. Native 32-bit device accesses
     * must include required ordering/barriers; no emulated successful writes.
     * No IRQ_ENABLE write: source must have enabled OUR bit, retained routing
     * ownership and excluded delivery to another handler for this session. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*now_us)(void *,uint64_t *); /* monotonic, cannot wrap */
    int (*delay_us)(void *,unsigned); /* bounded 1 us, no indefinite sleep */
};
struct ggrsc_state {
    struct ggrsc_config config;
    struct ggrsc_io io;
    uint64_t serial,completed,last_us,started_us;
    uint32_t control,status,wait,enable,msg,addr,data,cmd_status;
    uint32_t initialized,ready,inflight,closed,quarantined;
    uint32_t mutation_attempted,trigger_attempted,selector,last_code,polls;
    uint32_t arc_address; /* target of the retained serial, including uncertainty */
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggrsc_identity(void);
/* State must start zero and never move. No live IO before config validation.
 * Explicit v2.7 / v3.0 legacy layouts ONLY, no HW-channel mode or autodetect.
 * Source verifies actual version, assigned TCS, accessible extent and selector
 * mask against THIS BOOT's authorized Command DB. Config isn't a grant.
 * Open requires an idle, disabled, clean TCS and no pending target IRQ.
 * It will NEVER erase or adopt inherited commands, IRQs or active votes. */
int ggrsc_open(struct ggrsc_state *,const struct ggrsc_config *,const struct ggrsc_io *);
/* One ARC write, RESP_REQ and WAIT, cmd slot 0; no general RPMh/IB input.
 * Selector must be explicitly permitted. serial is allocated BEFORE the first
 * possible mutation and retained on error. A failed write may have happened.
 * Any uncertainty quarantines; no automatic retry, cancel or rollback.
 * Completion DOES NOT withdraw the voltage request or release native leases.
 * Resource-level aggregation/withdrawal belongs to the future native source. */
int ggrsc_submit(struct ggrsc_state *,uint32_t,uint64_t *);
/* Uses only either binding copied by open. One shared TCS and serial stream,
 * one in-flight request across both resources. No new mapping or reopening.
 * Each target has its own selector mask. The owner must retain/reconcile BOTH
 * resources after any partial sequence, including a completed first request. */
int ggrsc_submit_arc(struct ggrsc_state *,uint32_t address,uint32_t selector,uint64_t *);
/* Real target IRQ transition + issued/completed command status + idle TCS,
 * then ordered trigger disable/command disable/masked W1C + readback.
 * Neither ACK nor a write readback alone is command completion. Poll bounded
 * by timeout AND 4096 calls; repeated/stale serial cannot consume another job. */
int ggrsc_poll(struct ggrsc_state *,uint64_t);
/* Clean close only detaches the transport. Does NOT remove an ARC vote.
 * Busy/quarantined means retain source, guard, mapping and aggregation leases.
 * No reset/reopen API. Invalidation does no IO or unauthorized cleanup. */
int ggrsc_close(struct ggrsc_state *);
void ggrsc_invalidate(struct ggrsc_state *);
#ifdef __cplusplus
}
#endif
#endif
