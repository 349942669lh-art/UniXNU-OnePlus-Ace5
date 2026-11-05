/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef GIULIA_GPU_POWER_CONTROL_H
#define GIULIA_GPU_POWER_CONTROL_H
#include <stdint.h>
#include "giulia_gpu_diag.h"
/* Resource transactions, NOT raw regulator/PLL programming. A platform adapter
 * must resolve every name/parent/supply first and implement refcounted leases.
 * HWSCHED first boot uses freqs[1]=625 MHz (not the legacy 260 MHz path).
 * Board voltage corners and parent clocks remain the provider's responsibility.
 */
enum gg_resource { GGR_CX, GGR_GMU_RATE, GGR_HUB_RATE, GGR_GMU_CLK, GGR_CXO_CLK,
    GGR_AXI_CLK, GGR_MEMNOC_CLK, GGR_AHB_CLK, GGR_HUB_CLK, GGR_SMMU_CLK, GGR_COUNT };
enum gg_resource_result { GGRC_OK, GGRC_UNCHANGED_ERROR, GGRC_UNCERTAIN };
struct gg_power_ops {
    void *cookie;
    int (*resolve)(void *); /* All provider ABIs, resources and board policy; no mutation. */
    int (*quiescent)(void *); /* Exclusive lease, all GPU/GMU DMA stopped. */
    enum gg_resource_result (*acquire)(void *,unsigned,uint32_t,uint64_t *);
    int (*release)(void *,unsigned,uint64_t); /* Failure may have changed HW; quarantine. */
    int (*observe)(void *); /* 1=ready, 0=not yet, -1=I/O/fault. */
    int (*wait_us)(void *,unsigned);
    gg_trace_fn trace;
};
struct gg_power_state {
    uint64_t tokens[GGR_COUNT];
    unsigned attempted, held, ready, quarantined, stage, error;
    struct gg_diagnostics diag;
};
enum { GGC_OK, GGC_ARGUMENT, GGC_BUSY, GGC_PROVIDER, GGC_TIMEOUT, GGC_RELEASE };
static inline int gg_power_ops_valid(const struct gg_power_ops *o)
{
    return o && o->resolve && o->quiescent && o->acquire && o->release && o->observe && o->wait_us;
}
static inline int gg_power_release(const struct gg_power_ops *o,struct gg_power_state *s)
{
    unsigned i;int quiet;
    if (!gg_power_ops_valid(o) || !s) return GGC_ARGUMENT;
    if (s->quarantined) return GGC_BUSY;
    if (!s->held) { s->ready=0; return GGC_OK; }
    quiet=o->quiescent(o->cookie);
    if (quiet!=1) { s->quarantined=1;
        gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,12,GGD_VALIDATE,0,1,(uint64_t)quiet,6|GGD_NATIVE,~(uint64_t)0,GGC_BUSY,quiet,1);
        return GGC_BUSY; }
    for (i=GGR_COUNT;i>0;--i) if (s->held&(1u<<(i-1))) {
        int e=o->release(o->cookie,i-1,s->tokens[i-1]);
        gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,i-1,GGD_RELEASE,0,0,0,GGD_NATIVE,0,e?GGC_RELEASE:0,e,1);
        if (e) {
            s->quarantined=1; s->ready=0; return GGC_RELEASE;
        }
        s->held&=~(1u<<(i-1)); s->tokens[i-1]=0;
    }
    s->ready=0; return GGC_OK;
}
static inline int gg_power_acquire(const struct gg_power_ops *o,struct gg_power_state *s)
{
    unsigned i;int observed=0,e;
    if (!gg_power_ops_valid(o) || !s) return GGC_ARGUMENT;
    if (s->attempted || s->held || s->quarantined) return GGC_BUSY;
    s->attempted=1;
    s->stage=11;e=o->resolve(o->cookie);
    gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,11,GGD_CONFIGURE,0,0,0,GGD_NATIVE,0,e?GGC_PROVIDER:0,e,0);
    if(e)return s->error=GGC_PROVIDER;
    s->stage=12;observed=o->quiescent(o->cookie);
    gg_record(&s->diag,o->trace,o->cookie,GGD_POWER,12,GGD_VALIDATE,0,1,(uint64_t)observed,6,observed==1?0:GGC_BUSY,0);
    if(observed!=1)return s->error=GGC_BUSY;
    for (i=0;i<GGR_COUNT;++i) {
        enum gg_resource_result e; uint64_t token=0;
        uint32_t value=i==GGR_GMU_RATE?625000000u:i==GGR_HUB_RATE?200000000u:1;
        s->stage=i;
        e=o->acquire(o->cookie,i,value,&token);
        gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,i,GGD_ACQUIRE,0,value,0,GGD_EXPECTED|GGD_NATIVE,0,e?GGC_PROVIDER:0,e,0);
        if (token) { s->tokens[i]=token; s->held|=1u<<i; }
        /* Ambiguous mutations must never trigger optimistic power removal. */
        if ((e!=GGRC_OK && e!=GGRC_UNCHANGED_ERROR) || (e==GGRC_OK && !token) ||
            (e==GGRC_UNCHANGED_ERROR && token)) {
            gg_record(&s->diag,o->trace,o->cookie,GGD_POWER,i,GGD_VALIDATE,0,0,0,0,GGC_PROVIDER,0);
            s->quarantined=1; return s->error=GGC_PROVIDER;
        }
        if (e!=GGRC_OK) { s->error=GGC_PROVIDER; (void)gg_power_release(o,s); return s->error; }
    }
    s->stage=GGR_COUNT;
    for (i=0;i<1000;++i) {
        observed=o->observe(o->cookie);
        gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,GGR_COUNT,GGD_POLL,0,1,(uint64_t)observed,6|GGD_NATIVE,~(uint64_t)0,
            observed<0 || observed>1?GGC_PROVIDER:0,observed,0);
        if (observed==1) { s->ready=1; return GGC_OK; }
        if (observed<0 || observed>1) {s->error=GGC_PROVIDER;break;}
        e=o->wait_us(o->cookie,10);
        if(e) {
            gg_record_full(&s->diag,o->trace,o->cookie,GGD_POWER,GGR_COUNT,GGD_WAIT,0,10,0,GGD_EXPECTED|GGD_NATIVE,0,GGC_PROVIDER,e,0);
            s->error=GGC_PROVIDER;break;
        }
    }
    if (!s->error) s->error=GGC_TIMEOUT;
    gg_record(&s->diag,o->trace,o->cookie,GGD_POWER,GGR_COUNT,GGD_POLL,0,1,(uint64_t)observed,6,s->error,0);
    (void)gg_power_release(o,s); return s->error;
}
#endif
