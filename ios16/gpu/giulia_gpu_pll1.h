/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed SM8650 PLL1 standby-to-run executor, NOT a native power provider.
 * Caller serializes state/invalidate, pins all backing and a genuine native
 * guard for the ENTIRE call; callbacks cannot reenter or revoke that guard.
 */
#ifndef GIULIA_GPU_PLL1_H
#define GIULIA_GPU_PLL1_H
#include <stdint.h>
#define GGP1_IDENTITY "GIULIA GPU PLL1: calibrated-standby transactions no-power-authority"
#define GGP1_POLLS 200u
enum { GGP1_OK, GGP1_ARGUMENT, GGP1_USED, GGP1_UNAVAILABLE, GGP1_BUSY,
       GGP1_LOST, GGP1_IO, GGP1_READBACK, GGP1_TIMEOUT, GGP1_QUARANTINED };
enum { GGP1_IDLE, GGP1_READY, GGP1_INSPECT, GGP1_RATE, GGP1_LATCH,
       GGP1_RUN, GGP1_LOCK, GGP1_OUTPUT, GGP1_DONE };
struct ggp1_plan {
    /* floor_hz is an integer floor of the encoded rational rate, NOT a
     * measurement. reported_hz follows the source's 1 kHz ceiling convention.
     * Neither a rounded report nor LOCK proves physical output frequency. */
    uint32_t requested_hz,xo_hz,l,alpha,floor_hz,reported_hz,shortfall_hz;
};
struct ggp1_io {
    void *cookie;
    int (*live)(void *); /* CPU-only: exactly 1 while stable guard is held. */
    /* CPU-only, no I/O/mutation. 1 requires current-boot exclusive ownership
     * of PLL1 and all writers, device mapping, stable 19.2 MHz XO, supplies
     * and voltage votes sufficient for requested AND inherited rate, plus
     * completed initialization/calibration. This executor NEVER performs
     * first-power calibration, regulator votes or handoff. 0 denies unchanged;
     * other values are uncertain. All backing must survive quarantine. */
    int (*ready)(void *,const struct ggp1_plan *);
    /* 1 only if ALL PLL1 consumers are parked away and remain so until the
     * caller separately validates/switches RCGs. Quiet jobs alone is NOT enough.
     * 0 denies unchanged; other values are uncertain. CPU-only. */
    int (*parked)(void *);
    /* Fixed GPUCC-relative offsets only. Native writes include device DSB.
     * Nonzero can mean mutation happened; never retry or roll back on error. */
    int (*read32)(void *,uint32_t,uint32_t *);
    int (*write32)(void *,uint32_t,uint32_t);
    int (*wait_us)(void *,unsigned); /* Exactly 1 us. */
};
struct ggp1_state {
    struct ggp1_io io;
    uint32_t opened,quarantined,started,touched,phase,last_code;
    uint32_t reads,writes,waits,ack_polls,lock_polls,last_offset,observed;
    struct ggp1_plan applied;
};
#ifdef __cplusplus
extern "C" {
#endif
const char *ggp1_identity(void);
/* RAM-only fixed parent plans: 520 / 1250 MHz, NOT GPU core OPPs. */
int ggp1_describe(uint32_t,struct ggp1_plan *);
/* State must be zero-initialized once. Callback table/cookie/backing outlive
 * state; open checks liveness only and does NOT acquire any native resources. */
int ggp1_open(struct ggp1_state *,const struct ggp1_io *);
/* One successful start per state. Accepts only a calibrated, initialized,
 * reset-deasserted STANDBY PLL with output disabled, no FSM/vote-run or latch.
 * Preserves calibration L bits and unrelated MODE bits. No cold init,
 * calibration, active retune, disable, source binding or automatic invocation.
 * Uncertain I/O/liveness/readback/timeout permanently quarantines and retains
 * backing. No cleanup writes, release or retries even if output was enabled.
 * Cannot catch synchronous external aborts from an invalid native mapping. */
int ggp1_start(struct ggp1_state *,uint32_t);
void ggp1_invalidate(struct ggp1_state *);
#ifdef __cplusplus
}
#endif
#endif
