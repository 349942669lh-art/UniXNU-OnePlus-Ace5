/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_runtime.h"

/* Conservative exclusion envelope containing the captured GPU/GMU/GPUCC/
 * GPU-SMMU windows, including holes. It does NOT permit accesses to holes.
 * Using one envelope avoids self-overlapping GMU/GPUCC ledger claims.
 * Secure SID 2 has its own assignment path and is not activated here. */
static const struct gpr_key runtime_keys[GGRT_LEASES]={
    {GPR_MMIO,0,0,0,0x03d00000,0xe0000},
    {GPR_DMA_STREAM,0,0,0,0x03da0000,0},
    {GPR_DMA_STREAM,0,1,0,0x03da0000,0},
    {GPR_DMA_STREAM,0,5,0,0x03da0000,0}
};
static int runtime_authority(const struct ggrt_state *s)
{
    return s && s->epoch_open && s->ops.authority &&
        s->ops.authority(s->ops.cookie,s->epoch)==1;
}
/* Acquisition has not set ready yet, and release clears individual power
 * tokens. Neither can use runtime_live() as its access guard. The entire
 * exclusion bundle and the current platform authority must instead survive
 * EVERY provider call, not just the outer transaction. This is a backstop,
 * not a substitute for serializing revocation with the provider executor. */
static int runtime_access(const struct ggrt_state *s)
{
    if(!s || s->quarantined || !s->epoch_open || s->held!=GGRT_ALL)return 0;
    for(unsigned i=0;i<GGRT_LEASES;i++)
        if(!gpr_global_owns(&s->leases[i],s,s->epoch))return 0;
    return runtime_authority(s);
}
__attribute__((used)) void giulia_gpu_runtime_quarantine(struct ggrt_state *s)
{
    if(!s)return;
    s->quarantined=1;s->ready=0;s->power.ready=0;s->power.quarantined=1;
    for(unsigned i=0;i<GGRT_LEASES;i++)if(s->held&(1u<<i))
        (void)gpr_global_quarantine(&s->leases[i],s,s->epoch);
    /* No optimistic reset, power withdrawal, unmap or epoch retirement. */
}
static int runtime_guard(struct ggrt_state *s)
{
    if(runtime_access(s))return 1;
    giulia_gpu_runtime_quarantine(s);return 0;
}
static int runtime_resolve(void *cookie)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return -1;
    int code=s->ops.power.resolve(s->ops.power.cookie);
    return runtime_guard(s)?code:-1;
}
static int runtime_quiescent(void *cookie)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return -1;
    int quiet=s->ops.power.quiescent(s->ops.power.cookie);
    return runtime_guard(s)?quiet:-1;
}
static enum gg_resource_result runtime_power_acquire(void *cookie,unsigned id,
    uint32_t value,uint64_t *token)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return GGRC_UNCERTAIN;
    enum gg_resource_result code=s->ops.power.acquire(s->ops.power.cookie,id,value,token);
    /* Keep any returned token even if ownership disappeared during the call.
     * No rollback is authorized after such a loss. */
    return runtime_guard(s)?code:GGRC_UNCERTAIN;
}
static int runtime_power_release(void *cookie,unsigned id,uint64_t token)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return -1;
    int code=s->ops.power.release(s->ops.power.cookie,id,token);
    return runtime_guard(s)?code:-1;
}
static int runtime_observe(void *cookie)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return -1;
    int ready=s->ops.power.observe(s->ops.power.cookie);
    return runtime_guard(s)?ready:-1;
}
static int runtime_wait(void *cookie,unsigned us)
{
    struct ggrt_state *s=cookie;
    if(!runtime_guard(s))return -1;
    int code=s->ops.power.wait_us(s->ops.power.cookie,us);
    return runtime_guard(s)?code:-1;
}
static void runtime_trace(void *cookie,const struct gg_event *event)
{
    struct ggrt_state *s=cookie;
    /* Trace is CPU-only logging and may run after quarantine. It must not
     * access hardware, mutate authority or reenter this transaction. */
    if(s->ops.power.trace)s->ops.power.trace(s->ops.power.cookie,event);
}
static struct gg_power_ops runtime_power_ops(struct ggrt_state *s)
{
    const struct gg_power_ops guarded={s,runtime_resolve,runtime_quiescent,
        runtime_power_acquire,runtime_power_release,runtime_observe,runtime_wait,
        runtime_trace};
    return guarded;
}
static int runtime_unclaim(struct ggrt_state *s)
{
    for(unsigned i=GGRT_LEASES;i>0;i--)if(s->held&(1u<<(i-1))) {
        if(gpr_global_release(&s->leases[i-1],s,s->epoch)) {
            giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
        }
        s->held&=~(1u<<(i-1));
    }
    if(s->epoch_open) {
        if(gpr_global_retire_epoch(s,s->epoch)) {
            giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
        }
        s->epoch_open=0;
    }
    s->staged=0;return GGRT_OK;
}
static int runtime_claim(struct ggrt_state *s,uint64_t epoch)
{
    s->epoch=epoch;
    if(gpr_global_initialize() || gpr_global_open_epoch(s,epoch))return GGRT_CONFLICT;
    s->epoch_open=1;
    for(unsigned i=0;i<GGRT_LEASES;i++) {
        if(gpr_global_claim(&runtime_keys[i],s,epoch,0,0,&s->leases[i])) {
            int clean=runtime_unclaim(s);return clean?clean:GGRT_CONFLICT;
        }
        s->held|=1u<<i;
    }
    return GGRT_OK;
}
__attribute__((used)) int giulia_gpu_runtime_reserve(struct ggrt_state *s,uint64_t epoch)
{
    if(!s || !epoch)return GGRT_ARGUMENT;
    if(s->attempted || s->held || s->epoch_open || s->staged || s->quarantined)return GGRT_USED;
    int code=runtime_claim(s,epoch);
    if(!code)s->staged=1;
    return code;
}
__attribute__((used)) int giulia_gpu_runtime_cancel(struct ggrt_state *s)
{
    if(!s)return GGRT_ARGUMENT;
    if(s->quarantined || s->power.quarantined)return GGRT_QUARANTINED;
    if(!s->staged || !s->epoch_open || s->held!=GGRT_ALL || s->ready || s->dma ||
       s->power.ready || s->power.held)return GGRT_BUSY;
    return runtime_unclaim(s);
}
__attribute__((used)) int giulia_gpu_runtime_live(const struct ggrt_state *s)
{
    if(!s || !s->ready || s->quarantined || s->held!=GGRT_ALL ||
       !s->power.ready || s->power.quarantined ||
       s->power.held!=((1u<<GGR_COUNT)-1))return 0;
    return runtime_access(s);
}
__attribute__((used)) int giulia_gpu_runtime_acquire(struct ggrt_state *s,
    const struct ggrt_ops *ops,uint64_t epoch)
{
    if(!s || !ops || !epoch || !ops->authority || !gg_power_ops_valid(&ops->power))return GGRT_ARGUMENT;
    if(s->attempted || s->quarantined ||
       (s->staged && (s->held!=GGRT_ALL || !s->epoch_open || s->epoch!=epoch)) ||
       (!s->staged && (s->held || s->epoch_open)))return GGRT_USED;
    s->attempted=1;s->ops=*ops;s->epoch=epoch;
    /* Refuse before opening leases or calling any hardware mutator. */
    if(ops->authority(ops->cookie,epoch)!=1)return GGRT_AUTHORITY;
    if(!s->staged) {
        int code=runtime_claim(s,epoch);
        if(code)return code;
    }
    if(!runtime_authority(s)) {
        if(s->staged)return GGRT_AUTHORITY;
        int clean=runtime_unclaim(s);return clean?clean:GGRT_AUTHORITY;
    }
    struct gg_power_ops guarded=runtime_power_ops(s);
    int code=gg_power_acquire(&guarded,&s->power);
    if(code || !runtime_authority(s)) {
        if(s->power.held || s->power.quarantined || !runtime_authority(s)) {
            giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
        }
        if(s->staged)return GGRT_POWER;
        int clean=runtime_unclaim(s);return clean?clean:GGRT_POWER;
    }
    s->ready=1;return GGRT_OK;
}
__attribute__((used)) int giulia_gpu_runtime_dma(struct ggrt_state *s)
{
    if(!giulia_gpu_runtime_live(s))return GGRT_LOST;
    if(s->dma)return GGRT_OK;
    /* Mark BEFORE publishing any firmware-visible queue index. A partial
     * failure keeps all leases isolated; no producer is allowed to continue. */
    for(unsigned i=1;i<GGRT_LEASES;i++)
        if(gpr_global_mark_dma(&s->leases[i],s,s->epoch)) {
            giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
        }
    s->dma=1;return GGRT_OK;
}
__attribute__((used)) int giulia_gpu_runtime_drain(struct ggrt_state *s)
{
    if(!giulia_gpu_runtime_live(s))return GGRT_LOST;
    /* A command fence alone is NOT a global GPU/GMU DMA drain. */
    int quiet=runtime_quiescent(s);
    if(s->quarantined)return GGRT_LOST;
    if(quiet!=1)return GGRT_BUSY;
    if(!giulia_gpu_runtime_live(s)) {giulia_gpu_runtime_quarantine(s);return GGRT_LOST;}
    for(unsigned i=1;i<GGRT_LEASES;i++)
        if(gpr_global_quiesce(&s->leases[i],s,s->epoch)) {
            giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
        }
    s->dma=0;return GGRT_OK;
}
__attribute__((used)) int giulia_gpu_runtime_release(struct ggrt_state *s)
{
    if(!s)return GGRT_ARGUMENT;
    if(s->quarantined)return GGRT_QUARANTINED;
    if(!s->held && !s->power.held)return GGRT_OK;
    if(s->dma)return GGRT_BUSY;
    if(!giulia_gpu_runtime_live(s)) {giulia_gpu_runtime_quarantine(s);return GGRT_LOST;}
    struct gg_power_ops guarded=runtime_power_ops(s);
    if(gg_power_release(&guarded,&s->power)) {
        giulia_gpu_runtime_quarantine(s);return GGRT_QUARANTINED;
    }
    s->ready=0;return s->staged?GGRT_OK:runtime_unclaim(s);
}
#endif
