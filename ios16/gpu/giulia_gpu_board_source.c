/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Actual PLL1/RCG/GPUCC execution composed into ggpo_source. Upstream grant
 * and shared supply/parent/bandwidth backing remain native obligations.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_board_source.h"
__attribute__((used)) const char *ggbs_identity(void) { return GGBS_IDENTITY; }
static int bs_poison(struct ggbs_state *s,unsigned code)
{ if(!s->quarantined)s->last_code=code;s->quarantined=1;return (int)code; }
static int bs_live0(struct ggbs_state *s)
{
    if(!s->active || s->quarantined || __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE))return 0;
    int live=s->upstream.live(s->upstream.cookie,&s->grant);
    return live==1 && !__atomic_load_n(&s->lost,__ATOMIC_ACQUIRE);
}
static int bs_live(void *p)
{
    struct ggbs_state *s=p;
    if(bs_live0(s))return 1;
    (void)bs_poison(s,GGBS_LOST);return 0;
}
static int bs_rcg_ready(void *p,const struct ggrc_plan *plan)
{
    struct ggbs_state *s=p;
    if(plan->root!=GGRCLK_GMU && plan->root!=GGRCLK_HUB)return 0;
    return s->io.rcg_ready(s->io.cookie,plan);
}
static int bs_root_quiet(void *p,unsigned root)
{ struct ggbs_state *s=p;return s->io.root_quiet(s->io.cookie,root); }
static int bs_pll_ready(void *p,const struct ggp1_plan *plan)
{ struct ggbs_state *s=p;return s->io.pll_ready(s->io.cookie,plan); }
static int bs_pll_parked(void *p)
{ struct ggbs_state *s=p;return s->io.pll_parked(s->io.cookie); }
static int bs_branch_ready(void *p,unsigned id)
{ struct ggbs_state *s=p;return s->io.branch_ready(s->io.cookie,id); }
static int bs_branch_quiet(void *p,unsigned id)
{ struct ggbs_state *s=p;return s->io.branch_quiet(s->io.cookie,id); }
static int bs_cx_ready(void *p)
{ struct ggbs_state *s=p;return s->io.cx_ready(s->io.cookie); }
static int bs_cx_quiet(void *p)
{ struct ggbs_state *s=p;return s->upstream.power.quiescent(s->upstream.power.cookie); }
static int bs_gcc_ready(void *p,unsigned id)
{ struct ggbs_state *s=p;return s->io.gcc_ready(s->io.cookie,id); }
static int bs_gcc_offset(uint32_t off)
{ return off==4 || off==0x154 || off==0x10 || off==0x18; }
static int bs_gcc_read(void *p,uint32_t off,uint32_t *v)
{
    struct ggbs_state *s=p;
    if(!bs_gcc_offset(off) || !bs_live(s))return -1;
    ++s->reads;int result=s->io.gcc_read32(s->io.cookie,off,v);
    if(!bs_live(s))return -1;
    if(result) { (void)bs_poison(s,GGBS_CLOCK);return -1; }
    return 0;
}
static int bs_gcc_write(void *p,uint32_t off,uint32_t v)
{
    struct ggbs_state *s=p;
    if(!bs_gcc_offset(off) || !bs_live(s))return -1;
    ++s->writes;int result=s->io.gcc_write32(s->io.cookie,off,v);
    if(!bs_live(s))return -1;
    if(result) { (void)bs_poison(s,GGBS_CLOCK);return -1; }
    return 0;
}
static int bs_offset(uint32_t off,unsigned write)
{
    switch(off) {
    case 0x1000:case 0x1004:case 0x1010:case 0x1014:
    case 0x9318:case 0x931c:case 0x93ec:case 0x93f0:
    case 0x911c:case 0x913c:case 0x9144:case 0x93e8:case 0x7000:return 1;
    case 0x9108:return 1;
    case 0x9168:return !write;
    case 0x1018:case 0x101c:case 0x1020:case 0x1024:case 0x1028:
    case 0x102c:case 0x1030:case 0x1034:case 0x1038:return !write;
    default:return 0;
    }
}
static int bs_read(void *p,uint32_t off,uint32_t *v)
{
    struct ggbs_state *s=p;
    if(!bs_offset(off,0) || !bs_live(s))return -1;
    ++s->reads;int result=s->io.read32(s->io.cookie,off,v);
    if(!bs_live(s))return -1;
    if(result) { (void)bs_poison(s,GGBS_CLOCK);return -1; }
    return 0;
}
static int bs_write(void *p,uint32_t off,uint32_t v)
{
    struct ggbs_state *s=p;
    if(!bs_offset(off,1) || !bs_live(s))return -1;
    ++s->writes;int result=s->io.write32(s->io.cookie,off,v);
    if(!bs_live(s))return -1;
    if(result) { (void)bs_poison(s,GGBS_CLOCK);return -1; }
    return 0;
}
static int bs_wait(void *p,unsigned us)
{
    struct ggbs_state *s=p;
    if((us!=1 && us!=10) || !bs_live(s))return -1;
    int result=s->upstream.power.wait_us(s->upstream.power.cookie,us);
    if(!bs_live(s))return -1;
    if(result) { (void)bs_poison(s,GGBS_CLOCK);return -1; }
    return 0;
}
static int bs_scope(const struct ggpo_scope *a,const struct ggpo_scope *b)
{
    if(a->sid_mask!=b->sid_mask || a->prerequisites!=b->prerequisites || a->reserved!=b->reserved)return 0;
    for(unsigned i=0;i<GGPO_WINDOWS;i++)
        if(a->windows[i].base!=b->windows[i].base || a->windows[i].bytes!=b->windows[i].bytes)return 0;
    for(unsigned i=0;i<GGPO_IRQS;i++)if(a->intids[i]!=b->intids[i])return 0;
    return 1;
}
static int bs_take(void *p,uint64_t epoch,const struct ggpo_scope *scope,struct ggpo_grant *out)
{
    struct ggbs_state *s=p;
    if(!out)return GGPO_UNCHANGED;
    const struct ggpo_grant zero={0};*out=zero;
    if(!epoch || !scope || s->take_attempted || s->quarantined ||
       __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE))return GGPO_UNCHANGED;
    s->take_attempted=1;
    int result=s->upstream.take(s->upstream.cookie,epoch,scope,&s->grant);
    *out=s->grant; /* Preserve partial/malformed grants, never manufacture one. */
    unsigned dirty=0;const unsigned char *raw=(const unsigned char *)&s->grant;
    for(unsigned i=0;i<sizeof(s->grant);i++)dirty|=raw[i];
    if(result==GGPO_UNCHANGED && !dirty)return GGPO_UNCHANGED;
    s->active=1; /* Includes uncertainty: backing stays pinned. */
    if(result!=GGPO_TAKEN || s->grant.epoch!=epoch || !s->grant.serial ||
       !s->grant.self_vmid || s->grant.self_vmid>=65535 || s->grant.reserved ||
       !bs_scope(scope,&s->grant.scope) || !bs_live(s)) {
        (void)bs_poison(s,GGBS_PROTOCOL);return GGPO_UNCERTAIN;
    }
    const struct ggrc_io rcg={s,bs_live,bs_rcg_ready,bs_root_quiet,bs_read,bs_write,bs_wait};
    const struct ggp1_io pll={s,bs_live,bs_pll_ready,bs_pll_parked,bs_read,bs_write,bs_wait};
    const struct ggcb_io branches={s,bs_live,bs_branch_ready,bs_branch_quiet,bs_read,bs_write,bs_wait};
    const struct gggc_io gcc={s,bs_live,bs_gcc_ready,bs_gcc_read,bs_gcc_write,bs_wait};
    const struct ggcx_io cx={s,bs_live,bs_cx_ready,bs_cx_quiet,bs_read,bs_write,bs_wait};
    if(ggrc_open(&s->rcg,&rcg)!=GGRCLK_OK || ggp1_open(&s->pll,&pll)!=GGP1_OK ||
       ggcb_open(&s->branches,&branches)!=GGCB_OK || gggc_open(&s->gcc,&gcc)!=GGGC_OK || ggcx_open(&s->cx,&cx)!=GGCX_OK || !bs_live(s)) {
        (void)bs_poison(s,GGBS_PROTOCOL);return GGPO_UNCERTAIN;
    }
    return GGPO_TAKEN;
}
static int bs_authority(void *p,const struct ggpo_grant *g)
{
    struct ggbs_state *s=p;
    return g && g->epoch==s->grant.epoch && g->serial==s->grant.serial &&
        g->self_vmid==s->grant.self_vmid && !g->reserved &&
        bs_scope(&g->scope,&s->grant.scope) && bs_live(s);
}
static int bs_drop(void *p,const struct ggpo_grant *g)
{
    struct ggbs_state *s=p;
    if(s->retained || s->held_mask || !bs_authority(s,g))return GGBS_RETAINED;
    int result=s->upstream.drop(s->upstream.cookie,g);
    if(result || __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE))return bs_poison(s,GGBS_LOST);
    s->active=0;return 0; /* Successful drop ends authority; no live afterward. */
}
static int bs_resolve(void *p)
{
    struct ggbs_state *s=p;
    if(!bs_live(s))return GGBS_LOST;
    int result=s->upstream.power.resolve(s->upstream.power.cookie);
    if(!bs_live(s))return GGBS_LOST;
    return result;
}
static int bs_quiet(void *p)
{
    struct ggbs_state *s=p;
    if(!bs_live(s))return -1;
    int result=s->upstream.power.quiescent(s->upstream.power.cookie);
    return bs_live(s)?result:-1;
}
static unsigned bs_branch(unsigned id)
{
    switch(id) {
    case GGR_GMU_CLK:return GGCB_GMU;case GGR_CXO_CLK:return GGCB_CXO;
    case GGR_AHB_CLK:return GGCB_AHB;case GGR_HUB_CLK:return GGCB_HUB_AON;
    case GGR_SMMU_CLK:return GGCB_SMMU;default:return GGCB_CLOCKS;
    }
}
static unsigned bs_gcc(unsigned id)
{
    switch(id) {
    case GGR_AXI_CLK:return GGGC_AXI;case GGR_MEMNOC_CLK:return GGGC_MEMNOC;
    case GGR_DVM_CLK:return GGGC_DVM;default:return GGGC_CLOCKS;
    }
}
static enum gg_resource_result bs_acquire(void *p,unsigned id,uint32_t value,uint64_t *out)
{
    struct ggbs_state *s=p;
    if(!out)return GGRC_UNCHANGED_ERROR;
    *out=0;
    if(id>=GGR_COUNT || !bs_live(s))return GGRC_UNCERTAIN;
    uint32_t expected=id==GGR_GMU_RATE?625000000u:id==GGR_HUB_RATE?200000000u:1u;
    if(value!=expected || (s->attempted_mask&(1u<<id)) || s->tickets[id].held ||
       s->serial==UINT64_MAX)return GGRC_UNCHANGED_ERROR;
    /* Enforce the existing one-shot gg_power_acquire order. It is not an
     * arbitrary clock API, a second consumer or a dynamic retune interface. */
    if(s->held_mask!=((1u<<id)-1u))return GGRC_UNCHANGED_ERROR;
    s->resource=id;s->attempted_mask|=1u<<id;
    struct ggbs_ticket *t=&s->tickets[id];
    t->serial=++s->serial;t->held=1;s->held_mask|=1u<<id;*out=t->serial;
    unsigned branch=bs_branch(id),gcc=bs_gcc(id);int code=0;
    if(id==GGR_CX || id==GGR_GMU_RATE || id==GGR_HUB_RATE || branch<GGCB_CLOCKS || gcc<GGGC_CLOCKS) {
        t->local=1;s->retained=1; /* BEFORE any possibly partial mutation. */
        if(id==GGR_CX)code=ggcx_enable(&s->cx);
        else if(id==GGR_GMU_RATE) {
            code=ggrc_apply(&s->rcg,GGRCLK_GMU,19200000u);
            if(!code)code=ggp1_start(&s->pll,1250000000u);
            if(!code)code=ggrc_apply(&s->rcg,GGRCLK_GMU,value);
        } else if(id==GGR_HUB_RATE)code=ggrc_apply(&s->rcg,GGRCLK_HUB,value);
        else if(gcc<GGGC_CLOCKS)code=gggc_enable(&s->gcc,gcc);
        else code=ggcb_apply(&s->branches,branch,1);
        s->clock_code=(uint32_t)code;
        if(code || !bs_live(s)) {
            (void)bs_poison(s,GGBS_CLOCK);return GGRC_UNCERTAIN;
        }
        return GGRC_OK;
    }
    enum gg_resource_result result=s->upstream.power.acquire(s->upstream.power.cookie,id,value,&t->upstream);
    /* Preserve the upstream token before checking for loss. */
    if(!bs_live(s))return GGRC_UNCERTAIN;
    if(result==GGRC_OK && t->upstream)return GGRC_OK;
    if(result==GGRC_UNCHANGED_ERROR && !t->upstream) {
        t->held=0;s->held_mask&=~(1u<<id);*out=0;return result;
    }
    (void)bs_poison(s,GGBS_PROTOCOL);return GGRC_UNCERTAIN;
}
static int bs_release(void *p,unsigned id,uint64_t token)
{
    struct ggbs_state *s=p;
    if(id>=GGR_COUNT || !token || !s->tickets[id].held || s->tickets[id].serial!=token)return GGBS_ARGUMENT;
    /* PLL1 has no safe shutdown here and AON votes cannot be withdrawn like
     * ordinary CBCRs. Refuse ALL withdrawal before the first upstream call. */
    if(s->retained)return bs_poison(s,GGBS_RETAINED);
    if(!bs_live(s))return GGBS_LOST;
    struct ggbs_ticket *t=&s->tickets[id];
    if(t->local || !t->upstream)return bs_poison(s,GGBS_PROTOCOL);
    int result=s->upstream.power.release(s->upstream.power.cookie,id,t->upstream);
    if(!bs_live(s))return GGBS_LOST;
    if(result)return bs_poison(s,GGBS_PROTOCOL);
    t->held=0;s->held_mask&=~(1u<<id);return 0;
}
static int bs_observe(void *p)
{
    struct ggbs_state *s=p;
    if(s->held_mask!=((1u<<GGR_COUNT)-1u) || !bs_live(s))return -1;
    int result=s->upstream.power.observe(s->upstream.power.cookie);
    if(!bs_live(s))return -1;
    if(result!=0 && result!=1){(void)bs_poison(s,GGBS_PROTOCOL);return -1;}
    return result;
}
static void bs_trace(void *p,const struct gg_event *d)
{
    struct ggbs_state *s=p;
    if(s->upstream.power.trace)s->upstream.power.trace(s->upstream.power.cookie,d);
}
__attribute__((used)) int ggbs_init(struct ggbs_state *s,const struct ggpo_source *up,const struct ggbs_io *io)
{
    if(!s || !up || !io || up->abi!=GGPO_ABI || up->bytes!=sizeof(*up) ||
       !up->cookie || !up->take || !up->live || !up->drop || !gg_power_ops_valid(&up->power) ||
       !io->cookie || !io->rcg_ready || !io->root_quiet || !io->pll_ready || !io->pll_parked ||
       !io->branch_ready || !io->branch_quiet || !io->read32 || !io->write32 ||
       !io->gcc_ready || !io->gcc_read32 || !io->gcc_write32 || !io->cx_ready)return GGBS_ARGUMENT;
    if(s->initialized || __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE))return GGBS_USED;
    s->upstream=*up;s->io=*io;s->initialized=1;return GGBS_OK;
}
__attribute__((used)) int ggbs_export(struct ggbs_state *s,struct ggpo_source *out)
{
    if(!s || !out)return GGBS_ARGUMENT;
    const struct ggpo_source zero={0};*out=zero;
    if(!s->initialized || s->take_attempted || s->quarantined || __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE))return GGBS_UNAVAILABLE;
    const struct gg_power_ops power={s,bs_resolve,bs_quiet,bs_acquire,bs_release,bs_observe,bs_wait,bs_trace};
    const struct ggpo_source result={GGPO_ABI,sizeof(*out),s,bs_take,bs_authority,bs_drop,power};
    *out=result;return GGBS_OK;
}
__attribute__((used)) int giulia_gpu_board_source_register(void *service,struct ggbs_state *s)
{
    struct ggpo_source source;
    if(!service || !s)return GGBS_ARGUMENT;
    if(s->bind_attempted)return GGBS_USED;
    int result=ggbs_export(s,&source);if(result)return result;
    s->bind_attempted=1;
    result=giulia_gpu_power_owner_initialize();if(result)return GGBS_UNAVAILABLE;
    result=giulia_gpu_power_owner_register(service,&source);
    if(result)return GGBS_UNAVAILABLE;
    s->bound=1;return GGBS_OK;
}
__attribute__((used)) void ggbs_invalidate(struct ggbs_state *s)
{ if(s)__atomic_store_n(&s->lost,1u,__ATOMIC_RELEASE); }
__attribute__((used)) int ggbs_status(const struct ggbs_state *s,struct ggbs_status *out)
{
    if(!s || !out)return GGBS_ARGUMENT;
    const struct ggbs_status status={s->grant.epoch,s->bound,s->active,s->quarantined,
        __atomic_load_n(&s->lost,__ATOMIC_ACQUIRE),s->retained,s->held_mask,
        s->attempted_mask,s->last_code,s->clock_code,s->reads,s->writes};
    *out=status;return GGBS_OK;
}
#endif
