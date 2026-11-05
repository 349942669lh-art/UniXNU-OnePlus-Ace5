/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Runtime exclusion + power transaction, not a hardware ownership grant.
 * One serial executor owns this state. State/callbacks/backing stay pinned
 * until confirmed drain, or until reset after any uncertain mutation.
 */
#ifndef GIULIA_GPU_RUNTIME_H
#define GIULIA_GPU_RUNTIME_H
#include "giulia_gpu_power_control.h"
#include "../platform/giulia_platform_resources.h"
enum { GGRT_LEASES=4, GGRT_ALL=(1u<<GGRT_LEASES)-1 };
enum { GGRT_OK, GGRT_ARGUMENT, GGRT_USED, GGRT_AUTHORITY, GGRT_CONFLICT,
    GGRT_POWER, GGRT_BUSY, GGRT_LOST, GGRT_QUARANTINED };
struct ggrt_ops {
    void *cookie;
    /* Read-only platform proof for this boot/role/IRQ epoch and all named
     * windows/streams. Neither UEFI READY nor this software ledger suffices.
     * Provider must serialize revocation with this executor before mutation.
     * Runtime rechecks this and the broker bundle around every power callback;
     * checks cannot prevent a concurrent revocation DURING a hardware access. */
    int (*authority)(void *,uint64_t);
    /* All callbacks are synchronous, non-reentrant and use this nested cookie.
     * trace is CPU-only logging, including after quarantine; never hardware I/O. */
    struct gg_power_ops power;
};
struct ggrt_state {
    struct ggrt_ops ops;
    struct gg_power_state power;
    struct gpr_token leases[GGRT_LEASES];
    uint64_t epoch;
    unsigned attempted,epoch_open,held,ready,dma,quarantined;
};
#ifdef __cplusplus
extern "C" {
#endif
int giulia_gpu_runtime_acquire(struct ggrt_state *,const struct ggrt_ops *,uint64_t);
int giulia_gpu_runtime_live(const struct ggrt_state *);
int giulia_gpu_runtime_dma(struct ggrt_state *);
int giulia_gpu_runtime_drain(struct ggrt_state *);
int giulia_gpu_runtime_release(struct ggrt_state *);
void giulia_gpu_runtime_quarantine(struct ggrt_state *);
#ifdef __cplusplus
}
#endif
#endif
