/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * CPU-only interval aggregation and explicit pending/committed transactions.
 * Native caller owns the executor; this unit performs no I/O or allocation.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_arc_votes.h"
__attribute__((used)) const char *ggav_identity(void) { return GGAV_IDENTITY; }
static int av_code(struct ggav_state *s,unsigned code)
{ if(s)s->last_code=code;return (int)code; }
__attribute__((used)) void ggav_quarantine(struct ggav_state *s)
{ if(s){s->quarantined=1;s->last_code=GGAV_QUARANTINED;} }
static int av_ready(struct ggav_state *s)
{
    if(!s)return GGAV_ARGUMENT;
    if(s->quarantined)return GGAV_QUARANTINED;
    if(!s->initialized)return GGAV_USED;
    if(s->pending.serial)return GGAV_BUSY;
    if(s->serial==~(uint64_t)0)return GGAV_FULL;
    return GGAV_OK;
}
static int av_plan(struct ggav_state *s,unsigned skip,uint32_t min,uint32_t max,
    struct ggarc_plan *plan)
{
    for(unsigned i=0;i<GGAV_SLOTS;i++)if(i!=skip && s->votes[i].ticket) {
        if(min<s->votes[i].min)min=s->votes[i].min;
        if(max>s->votes[i].max)max=s->votes[i].max;
    }
    if(min>max || max>65535u)return GGAV_RANGE;
    return ggarc_select(&s->table,min,max,plan)==GGARC_OK?GGAV_OK:GGAV_RANGE;
}
__attribute__((used)) int ggav_init(struct ggav_state *s,uint64_t epoch,
    const struct ggarc_table *table,uint32_t min,uint32_t max)
{
    if(!s || !table || !epoch || min>max || max>65535u)return GGAV_ARGUMENT;
    if(s->initialized || s->quarantined || s->serial || s->pending.serial)return GGAV_USED;
    /* Reject nonzero caller state rather than adopting phantom tickets. */
    for(unsigned i=0;i<GGAV_SLOTS;i++)if(s->votes[i].ticket)return GGAV_USED;
    struct ggarc_plan plan={0,0,0};
    int code=ggarc_select(table,min,max,&plan);
    if(code!=GGARC_OK)return GGAV_ARGUMENT;
    s->table=*table;s->epoch=epoch;s->base_min=min;s->base_max=max;
    s->initialized=1;s->applied_valid=0;
    return av_code(s,GGAV_OK);
}
static int av_prepare(struct ggav_state *s,unsigned op,unsigned slot,uint64_t ticket,
    uint32_t min,uint32_t max,struct ggav_transaction *out)
{
    uint32_t lower=s->base_min,upper=s->base_max;
    if(op==GGAV_BASE) { lower=min;upper=max; }
    else if(op==GGAV_ADD) {
        if(lower<min)lower=min;
        if(upper>max)upper=max;
    }
    struct ggarc_plan plan={0,0,0};
    int code=av_plan(s,op==GGAV_REMOVE?slot:GGAV_SLOTS,lower,upper,&plan);
    if(code)return av_code(s,(unsigned)code);
    struct ggav_transaction tx={s->epoch,++s->serial,ticket,plan,op,slot,min,max};
    if(op==GGAV_ADD)tx.ticket=tx.serial;
    s->pending=tx;*out=tx;return av_code(s,GGAV_OK);
}
__attribute__((used)) int ggav_add(struct ggav_state *s,uint32_t min,uint32_t max,
    struct ggav_transaction *out)
{
    if(!out)return GGAV_ARGUMENT;
    const struct ggav_transaction zero={0};*out=zero;
    if(min>max || max>65535u)return GGAV_ARGUMENT;
    int code=av_ready(s);if(code)return av_code(s,(unsigned)code);
    unsigned slot=0;while(slot<GGAV_SLOTS && s->votes[slot].ticket)++slot;
    if(slot==GGAV_SLOTS)return av_code(s,GGAV_FULL);
    return av_prepare(s,GGAV_ADD,slot,0,min,max,out);
}
__attribute__((used)) int ggav_remove(struct ggav_state *s,uint64_t ticket,
    struct ggav_transaction *out)
{
    if(!out)return GGAV_ARGUMENT;
    const struct ggav_transaction zero={0};*out=zero;
    if(!ticket)return GGAV_ARGUMENT;
    int code=av_ready(s);if(code)return av_code(s,(unsigned)code);
    unsigned slot=0;while(slot<GGAV_SLOTS && s->votes[slot].ticket!=ticket)++slot;
    if(slot==GGAV_SLOTS)return av_code(s,GGAV_STALE);
    return av_prepare(s,GGAV_REMOVE,slot,ticket,0,0,out);
}
__attribute__((used)) int ggav_base(struct ggav_state *s,uint32_t min,uint32_t max,
    struct ggav_transaction *out)
{
    if(!out)return GGAV_ARGUMENT;
    const struct ggav_transaction zero={0};*out=zero;
    if(min>max || max>65535u)return GGAV_ARGUMENT;
    int code=av_ready(s);if(code)return av_code(s,(unsigned)code);
    return av_prepare(s,GGAV_BASE,GGAV_SLOTS,0,min,max,out);
}
__attribute__((used)) int ggav_finish(struct ggav_state *s,uint64_t epoch,uint64_t serial,
    unsigned result,const struct ggarc_plan *observed)
{
    if(!s || !epoch || !serial)return GGAV_ARGUMENT;
    if(s->quarantined)return GGAV_QUARANTINED;
    if(!s->initialized)return GGAV_USED;
    if(epoch!=s->epoch || serial!=s->pending.serial)return av_code(s,GGAV_STALE);
    const struct ggav_transaction tx=s->pending;
    if(result==GGAV_APPLIED && observed && observed->address==tx.plan.address &&
       observed->selector==tx.plan.selector && observed->logical_level==tx.plan.logical_level) {
        if(tx.operation==GGAV_ADD) {
            s->votes[tx.slot].ticket=tx.ticket;
            s->votes[tx.slot].min=tx.min;s->votes[tx.slot].max=tx.max;
        } else if(tx.operation==GGAV_REMOVE) {
            const struct ggav_vote zero={0};s->votes[tx.slot]=zero;
        } else {s->base_min=tx.min;s->base_max=tx.max;}
        s->applied=tx.plan;s->applied_valid=1;
    } else if(result!=GGAV_UNCHANGED || observed) {
        ggav_quarantine(s);return av_code(s,GGAV_UNCERTAIN);
    }
    const struct ggav_transaction zero={0};s->pending=zero;
    return av_code(s,GGAV_OK);
}
__attribute__((used)) int ggav_status(const struct ggav_state *s,struct ggav_status *out)
{
    if(!s || !out)return GGAV_ARGUMENT;
    const struct ggav_status zero={0};*out=zero;
    out->epoch=s->epoch;out->serial=s->serial;out->pending_serial=s->pending.serial;
    out->base_min=s->base_min;out->base_max=s->base_max;
    out->applied_valid=s->applied_valid;out->applied=s->applied;
    out->quarantined=s->quarantined;out->last_code=s->last_code;
    for(unsigned i=0;i<GGAV_SLOTS;i++)if(s->votes[i].ticket)++out->count;
    return GGAV_OK;
}
#endif
