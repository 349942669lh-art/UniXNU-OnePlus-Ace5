/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pineapple GCC GPU parent branch votes. NOT a PLL/regulator or GPU owner.
 * A native platform guard must retain stable 600 MHz GPLL0, 300 MHz OUT_EVEN,
 * the post-divider configuration and supplies,
 * own the register, serialize ALL RMW users, and exclude upstream revocation
 * throughout each transaction. Neither DT nor register bits provide this.
 */
#ifndef GIULIA_GPU_PARENTS_H
#define GIULIA_GPU_PARENTS_H
#include <stdint.h>
#ifndef UINT64_C
#define UINT64_C(x) x##ULL
#endif
#ifndef UINT64_MAX
#define UINT64_MAX (~(uint64_t)0)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX (~(uint32_t)0)
#endif
enum { GGCP_MAIN=0, GGCP_DIV=1, GGCP_CLOCKS=2, GGCP_SLOTS=16 };
#define GGCP_PAGE UINT64_C(0x152000)
#define GGCP_BYTES 0x1000u
#define GGCP_MASK ((1u<<15)|(1u<<16))
#define GGCP_SETTLE_US 10u
enum { GGCP_OK=0, GGCP_ARGUMENT, GGCP_USED, GGCP_INHERITED, GGCP_FULL,
       GGCP_LOST, GGCP_IO, GGCP_BUSY, GGCP_QUARANTINED, GGCP_UNAVAILABLE };
struct ggcp_io {
    void *cookie;
    int (*live)(void *); /* Already inside the native platform guard. */
    int (*read32)(void *,uint32_t *); /* Fixed GCC +0x52000 only. */
    int (*write32)(void *,uint32_t); /* Barrier is part of the native write. */
    int (*settle)(void *,unsigned); /* Exactly 10 us; no halt-bit polling. */
    int (*quiescent)(void *); /* 1=all dependents quiet, 0=busy, else uncertain. */
};
struct ggcp_ticket { uint64_t serial; unsigned clock; };
struct ggcp_state {
    struct ggcp_io io;
    struct ggcp_ticket tickets[GGCP_SLOTS];
    uint64_t serial;
    unsigned refs[GGCP_CLOCKS],enabled,attempted,ready,closed,quarantined;
    unsigned write_attempted,last_code;
};
struct ggcp_scope {
    uint64_t page,parent_hz,div_hz;
    uint32_t bytes,offset,mask,settle_us;
};
#define GGCP_ABI 1u
struct ggcp_authority {
    uint32_t abi,bytes;
    void *cookie;
    /* 1 holds a native guard for this exact scope and epoch; 0 is an
     * unchanged denial. Any other result is uncertain and retained. No
     * Boot Services pointers, USB registration or self-authenticating blobs. */
    int (*enter)(void *,uint64_t,const struct ggcp_scope *);
    int (*live)(void *,uint64_t); /* CPU-only, no I/O, exactly 1. */
    int (*quiescent)(void *,uint64_t);
    /* Releases only the CPU guard, NEVER clock/voltage/access backing. It
     * must also work after a latched loss. 0 confirms guard release. */
    int (*leave)(void *,uint64_t);
};
struct ggcp_status {
    uint64_t epoch;
    uint32_t bound,ready_cached,quarantined,revoked,enabled_cached;
    uint32_t main_refs,div_refs,write_attempted,last_code;
};
#ifdef __cplusplus
extern "C" {
#endif
/* State must be zero-initialized once. Serialized core; the I/O table/backing must outlive the state. Outputs may
 * retain a ticket after an uncertain write. Never discard it or roll back. */
int ggcp_init(struct ggcp_state *,const struct ggcp_io *);
int ggcp_acquire(struct ggcp_state *,unsigned,uint64_t *);
int ggcp_release(struct ggcp_state *,uint64_t);
int ggcp_close(struct ggcp_state *);
void ggcp_quarantine(struct ggcp_state *);
/* Native, process-context only. One registration per boot, one permanent
 * executor. source is a real IOService retained until clean close; retain
 * does not stop its stop() freeing backing. Source MUST honor BUSY/quarantine.
 * Invalidate synchronizes with operations; a clean close atomically detaches
 * before unmapping. Source MUST keep backing until close returns and must
 * honor a denied/uncertain close. No production caller/source is supplied. */
int giulia_gpu_parents_initialize(void);
int giulia_gpu_parents_open(void *source,uint64_t,const struct ggcp_authority *);
int giulia_gpu_parents_acquire(void *source,unsigned,uint64_t *);
int giulia_gpu_parents_release(void *source,uint64_t);
int giulia_gpu_parents_close(void *source);
int giulia_gpu_parents_invalidate(void *source);
int giulia_gpu_parents_status(struct ggcp_status *); /* Cached CPU-only. */
#ifdef __cplusplus
}
#endif
#endif
