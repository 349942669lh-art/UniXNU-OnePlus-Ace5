/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Serialized ARC vote planner, normal kernel RAM only. NOT an RPMh provider.
 * A native owner must serialize all entries, include every upstream client in
 * the base interval, retain the current-boot snapshot/backing, and match an
 * actual transport completion before calling finish(APPLIED). No value in
 * this structure establishes access, ownership, voltage or hardware readiness.
 */
#ifndef GIULIA_GPU_ARC_VOTES_H
#define GIULIA_GPU_ARC_VOTES_H
#include "giulia_gpu_arc.h"
#define GGAV_SLOTS 16u
#define GGAV_IDENTITY "GIULIA GPU ARC VOTES: transactional CPU-only aggregate unbound"
enum { GGAV_OK=0, GGAV_ARGUMENT, GGAV_USED, GGAV_FULL, GGAV_RANGE,
       GGAV_BUSY, GGAV_STALE, GGAV_QUARANTINED, GGAV_UNCERTAIN };
enum { GGAV_ADD=1, GGAV_REMOVE, GGAV_BASE };
enum { GGAV_APPLIED=0, GGAV_UNCHANGED=1, GGAV_UNKNOWN=2 };
struct ggav_vote { uint64_t ticket; uint32_t min,max; };
struct ggav_transaction {
    uint64_t epoch,serial,ticket;
    struct ggarc_plan plan;
    uint32_t operation,slot,min,max;
};
struct ggav_state {
    struct ggarc_table table;
    struct ggav_vote votes[GGAV_SLOTS];
    struct ggav_transaction pending;
    struct ggarc_plan applied;
    uint64_t epoch,serial;
    uint32_t base_min,base_max,initialized,applied_valid,quarantined,last_code;
};
struct ggav_status {
    uint64_t epoch,serial,pending_serial;
    uint32_t count,base_min,base_max,applied_valid,quarantined,last_code;
    struct ggarc_plan applied;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggav_identity(void);
/* State must be initially zero. All input tables are private stable RAM, not
 * firmware memory or MMIO. Table is copied; no pointers borrowed. One init.
 * base is the authoritative owner's other-client constraint, NOT assumed OFF.
 * Initialization is not evidence that even the base plan reached hardware. */
int ggav_init(struct ggav_state *,uint64_t,const struct ggarc_table *,uint32_t,uint32_t);
/* One pending transaction, no merging or cancellation after issue. The source
 * must pin the returned transaction/ticket BEFORE any potentially partial I/O.
 * Failure leaves out zero. Output and input/state must not alias. */
int ggav_add(struct ggav_state *,uint32_t,uint32_t,struct ggav_transaction *);
int ggav_remove(struct ggav_state *,uint64_t,struct ggav_transaction *);
int ggav_base(struct ggav_state *,uint32_t,uint32_t,struct ggav_transaction *);
/* UNCHANGED only if transport was never triggered or independently proved no
 * change. An error/timeout after trigger is UNKNOWN, not UNCHANGED. APPLIED
 * requires an exact epoch/serial association AND the actual completed plan.
 * Echoing the requested plan is not completion evidence. Failed reconciliation
 * retains pending + old votes until reset. Zero-vote base may be NONZERO. */
int ggav_finish(struct ggav_state *,uint64_t,uint64_t,unsigned,const struct ggarc_plan *);
void ggav_quarantine(struct ggav_state *);
int ggav_status(const struct ggav_state *,struct ggav_status *);
#ifdef __cplusplus
}
#endif
#endif
