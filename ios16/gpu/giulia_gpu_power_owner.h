/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Resident, power-only handoff coordinator. This API is kernel-only. A native
 * board source must supply the actual firmware/VM handoff and refcounted clock,
 * voltage and bandwidth operations. No source is installed by a DT property,
 * UEFI READY report, USB command or this coordinator itself.
 */
#ifndef GIULIA_GPU_POWER_OWNER_H
#define GIULIA_GPU_POWER_OWNER_H
#include "giulia_gpu_runtime.h"

#define GGPO_ABI 1u
enum { GGPO_WINDOWS=6, GGPO_IRQS=3, GGPO_PREREQUISITES=0x3fu };
/* Prerequisites: retained CFG access, ACD policy, parent clocks, voltage,
 * bandwidth and reset/global-quiescence policy. These bits describe the
 * native verifier's obligations; setting them is NOT an authorization. */
struct ggpo_range { uint64_t base,bytes; };
struct ggpo_scope {
    struct ggpo_range windows[GGPO_WINDOWS];
    uint32_t sid_mask,intids[GGPO_IRQS],prerequisites,reserved;
};
struct ggpo_grant {
    uint64_t epoch,serial;
    uint32_t self_vmid,reserved;
    struct ggpo_scope scope;
};
enum { GGPO_TAKEN=0, GGPO_UNCHANGED=1, GGPO_UNCERTAIN=2 };
struct ggpo_source {
    uint32_t abi,bytes;
    void *cookie;
    /* take must perform the authoritative transfer, not synthesize a grant
     * from static resource descriptions. UNCHANGED promises no mutation and
     * a completely zero grant. Anything uncertain is retained until reset. */
    int (*take)(void *,uint64_t,const struct ggpo_scope *,struct ggpo_grant *);
    /* Exactly 1 proves this grant, its complete scope and all prerequisites
     * are still held for this boot. No MMIO or mutation in this check. */
    int (*live)(void *,const struct ggpo_grant *);
    /* Native source releases the transfer and prerequisite leases only after
     * power was safely withdrawn. Nonzero means uncertain, not a retry. */
    int (*drop)(void *,const struct ggpo_grant *);
    struct gg_power_ops power;
};
enum { GGPO_OK=0, GGPO_ARGUMENT, GGPO_UNAVAILABLE, GGPO_USED, GGPO_BUSY,
    GGPO_HANDOFF, GGPO_POWER, GGPO_LOST, GGPO_QUARANTINED };
struct ggpo_status {
    uint64_t epoch,serial;
    uint32_t registered,attempted,grant_held,ready,revoked,quarantined;
    uint32_t power_held,mutation_attempted,last_code;
};
#ifdef __cplusplus
extern "C" {
#endif
/* Initialization allocates one permanent process-context lock. All remaining
 * entries are process-context, non-reentrant, and never called by an IRQ.
 * source/consumer must be real IOService objects in the native kernel. */
int giulia_gpu_power_owner_initialize(void);
int giulia_gpu_power_owner_register(void *source,const struct ggpo_source *);
int giulia_gpu_power_owner_acquire(void *consumer,uint64_t epoch);
int giulia_gpu_power_owner_release(void *consumer);
/* Latch denial before waiting for the executor. Return acknowledges no local
 * power callback is executing. A source MUST coordinate upstream revocation
 * with this call; this cannot stop a hypervisor revoking hardware mid-access.
 * Active/uncertain votes stay pinned, and the source may not destroy backing. */
int giulia_gpu_power_owner_invalidate(void *source);
int giulia_gpu_power_owner_unregister(void *source);
/* Cached CPU snapshot only; never queries live hardware or calls a provider. */
int giulia_gpu_power_owner_status(struct ggpo_status *);
#ifdef __cplusplus
}
#endif
#endif
