/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_CX_H
#define GIULIA_GPU_CX_H
#include <stdint.h>
#define GGCX_IDENTITY "GIULIA GPU CX: fixed retained GDSC transactions"
#define GGCX_CONTROL 0x9108u
#define GGCX_STATUS 0x9168u
#define GGCX_POLLS 1500u
#define GGCX_WINDOWS 2u
enum { GGCX_OK, GGCX_ARGUMENT, GGCX_USED, GGCX_UNAVAILABLE, GGCX_BUSY,
       GGCX_LOST, GGCX_IO, GGCX_READBACK, GGCX_TIMEOUT, GGCX_QUARANTINED };
enum { GGCX_IDLE, GGCX_PREPARE, GGCX_ENABLE, GGCX_POLL, GGCX_RETAIN, GGCX_DONE };
struct ggcx_policy {
    uint32_t control_offset, status_offset;
    uint32_t mode_mask, wait_mask, wait_value, collapse_mask, retain_mask, power_mask;
    uint32_t polls_per_window, poll_windows;
};
struct ggcx_io {
    void *cookie;
    int (*live)(void *);
    /* CPU-only, valid across every operation. Requires assigned software
     * control of CX (including inherited HW_CONTROL takeover), exclusive
     * register updates, retained CFG/XO/ARC/ACD and access supplies, and a
     * synchronized revocation guard. The caller establishes these first. */
    int (*ready)(void *);
    /* CPU-only current-boot proof of global quiescence and required resets.
     * 1=held, 0=unchanged busy, any other value=uncertain. */
    int (*quiescent)(void *);
    /* GPUCC-relative device/uncached access. Only CONTROL is writable;
     * write includes DSB. External aborts cannot be recovered here. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*wait_us)(void *,unsigned);
};
struct ggcx_state {
    struct ggcx_io io;
    uint32_t opened, attempted, enabled, quarantined, last_code, phase;
    uint32_t mutation_attempted, inherited, before, observed, last_offset;
    uint32_t reads, writes, waits, polls, poll_window;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggcx_identity(void);
int ggcx_describe(struct ggcx_policy *);
int ggcx_open(struct ggcx_state *,const struct ggcx_io *);
/* One attempt once ready/quiet. Configures fixed software mode, enables CX,
 * polls the separate status register, then retains FF state. No mapping,
 * voltage vote, ownership transfer, registration, disable or retry of writes.
 * All backing survives failure and stop until the platform's verified reset.
 * Caller serializes APIs/callbacks; invalidate follows executor synchronization. */
int ggcx_enable(struct ggcx_state *);
void ggcx_invalidate(struct ggcx_state *);
#ifdef __cplusplus
}
#endif
#endif
