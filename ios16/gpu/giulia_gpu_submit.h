/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Bounded, nonsecure Gen7 legacy dispatch submission. NOT ISSUE_CMD_RAW,
 * CP_INIT, PM4 validation, a scheduler, or a user-client/Metal interface.
 * All calls/revocation are serialized with the runtime owner. No terminal
 * can install callbacks. Never bind this to untrusted mutable IB backing.
 */
#ifndef GIULIA_GPU_SUBMIT_H
#define GIULIA_GPU_SUBMIT_H
#include "giulia_gpu_runtime.h"
#include "giulia_gpu_hfi.h"
enum { GGSUB_MAX_IBS=4, GGSUB_WORDS=8+3*GGSUB_MAX_IBS };
enum { GGSUB_OK, GGSUB_ARGUMENT, GGSUB_BUSY, GGSUB_NOT_READY, GGSUB_FULL,
    GGSUB_PIN, GGSUB_PENDING, GGSUB_COMPLETE, GGSUB_LOST, GGSUB_FAULT,
    GGSUB_TIMEOUT, GGSUB_EXHAUSTED, GGSUB_QUARANTINED };
struct ggsub_ib { uint64_t address; uint32_t bytes; };
struct ggsub_request {
    uint32_t context,queue,timestamp,count;
    struct ggsub_ib ibs[GGSUB_MAX_IBS];
};
struct ggsub_ops {
    void *cookie;
    /* Current boot: runtime power/IRQ ownership, GPU SMMU, GMU/HFI/CP
     * initialization, registered nonsecure context bound to this legacy
     * dispatch queue, no context-queue mode, and a real fence backend.
     * Provider serializes lifecycle loss with submit/poll, not just a bitmask. */
    int (*proof)(void *,uint64_t,uint32_t,uint32_t);
    /* 0=success with nonzero token, 1=unchanged failure with zero token,
     * everything else=uncertain. Atomically seal/pin all trusted IBs and
     * referenced writable buffers and ARM a fresh unsignaled fence matching
     * epoch/context/timestamp. Validate the complete IB ranges + translations,
     * maintain caches, deny all CPU mutation/unmap until cancel/retire/reset.
     * An old signaled fence must NEVER satisfy this reservation. */
    int (*pin)(void *,uint64_t,const struct ggsub_request *,uint64_t *);
    int (*cancel)(void *,uint64_t); /* Only before queue-index publication. */
    int (*ring)(void *,uint32_t); /* Own GMU HOST2GMU_INTR_SET; no retries. */
    /* 0=pending, 1=matching real hardware fence success, other=fault.
     * A queue ACK/read-index change or TS_RETIRE alone is insufficient. */
    int (*fence)(void *,uint64_t,uint64_t,uint32_t,uint32_t);
    int (*retire)(void *,uint64_t); /* Observe output caches, release pin. */
    uint64_t (*now_us)(void *);
};
struct ggsub_state {
    struct ggsub_ops ops;
    struct ggrt_state *runtime;
    struct ggh_io io;
    struct ggsub_request request;
    uint64_t token,start_us,last_us,timeout_us;
    unsigned initialized,busy,published,completed,quarantined,sequence,last_timestamp;
};
#ifdef __cplusplus
extern "C" {
#endif
int giulia_gpu_submit_encode(const struct ggsub_request *,unsigned,uint32_t *,unsigned);
int giulia_gpu_submit_init(struct ggsub_state *,struct ggrt_state *,const struct ggh_io *,const struct ggsub_ops *);
int giulia_gpu_submit(struct ggsub_state *,const struct ggsub_request *,unsigned);
int giulia_gpu_submit_poll(struct ggsub_state *);
void giulia_gpu_submit_invalidate(struct ggsub_state *);
#ifdef __cplusplus
}
#endif
#endif
