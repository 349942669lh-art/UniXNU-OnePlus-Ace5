/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RAM-only complete source composition. Native authority and I/O doubles are
 * deliberately confined to this HOST fixture; they do NOT enable hardware.
 */
#include "giulia_gpu_board_source.h"
#include "board-source-policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static unsigned binds;
int giulia_gpu_power_owner_initialize(void){return GGPO_OK;}
int giulia_gpu_power_owner_register(void *service,const struct ggpo_source *source)
{CHECK(service && source && source->abi==GGPO_ABI);++binds;return GGPO_OK;}
struct mock {
    struct ggbs_state s;
    struct ggpo_source exposed;
    struct ggpo_grant grant;
    struct gg_power_state power;
    uint32_t reg[0x9800/4];
    uint32_t gcc[GGGC_BYTES/4];
    unsigned events,fail_at,lives,lose_at,valid,reads,writes,waits;
    unsigned takes,drops,acquires,releases,observes,delegated_mask,trace_count;
    unsigned deny,bad_grant,uncertain,fail_resource,no_ack,no_lock,zero_token,dirty_failure,invalidate_write,cx_stall;
    int ready,quiet;
};
static int event(struct mock *m){return ++m->events==m->fail_at;}
static int take(void *p,uint64_t epoch,const struct ggpo_scope *scope,struct ggpo_grant *g)
{
    struct mock *m=p;++m->takes;CHECK(epoch==17);
    int failed=event(m);
    if(m->deny && !m->bad_grant)return GGPO_UNCHANGED;
    g->epoch=epoch;g->serial=41;g->self_vmid=5;g->scope=*scope;
    if(m->bad_grant)g->scope.sid_mask^=1u;
    if(m->deny)return GGPO_UNCHANGED; /* malformed denial must be retained */
    return failed || m->uncertain?GGPO_UNCERTAIN:GGPO_TAKEN;
}
static int live(void *p,const struct ggpo_grant *g)
{
    struct mock *m=p;CHECK(g->epoch==17 && g->serial==41);
    if(++m->lives==m->lose_at)m->valid=0;
    return (int)m->valid;
}
static int drop(void *p,const struct ggpo_grant *g)
{struct mock *m=p;CHECK(g->serial==41);++m->drops;return event(m)?-1:0;}
static int resolve(void *p){return event(p)?-1:0;}
static int quiet(void *p){struct mock *m=p;return event(m)?-1:m->quiet;}
static enum gg_resource_result acquire(void *p,unsigned id,uint32_t value,uint64_t *token)
{
    struct mock *m=p;CHECK(id==GGR_CX);
    CHECK(value==1);++m->acquires;m->delegated_mask|=1u<<id;
    if(m->fail_resource==id+1u){if(m->dirty_failure)*token=UINT64_MAX-id;return GGRC_UNCHANGED_ERROR;}
    *token=UINT64_MAX-id; /* upstream tokens may use the HIGH bit */
    if(m->zero_token)*token=0;
    return event(m)?GGRC_UNCERTAIN:GGRC_OK;
}
static int release(void *p,unsigned id,uint64_t token)
{struct mock *m=p;CHECK(token==UINT64_MAX-id);++m->releases;return event(m)?-1:0;}
static int observe(void *p){struct mock *m=p;++m->observes;return event(m)?-1:1;}
static int wait_us(void *p,unsigned us)
{struct mock *m=p;CHECK(us==1 || us==10);++m->waits;return event(m)?-1:0;}
static void trace(void *p,const struct gg_event *e)
{struct mock *m=p;CHECK(e->sequence);++m->trace_count;}
static int rcg_ready(void *p,const struct ggrc_plan *plan)
{
    struct mock *m=p;CHECK(plan->root<2);
    if(plan->root==GGRCLK_GMU && plan->parent==GGRCLK_PLL1)
        CHECK(m->s.pll.started && m->s.pll.applied.requested_hz==1250000000u);
    return m->ready;
}
static int root_quiet(void *p,unsigned id)
{struct mock *m=p;CHECK(id<2);return m->quiet;}
static int pll_ready(void *p,const struct ggp1_plan *plan)
{struct mock *m=p;CHECK(plan->requested_hz==1250000000u);return m->ready;}
static int parked(void *p)
{
    struct mock *m=p;CHECK((m->reg[0x931c/4]&0x0010371fu)==0x100001u);
    return m->quiet;
}
static int branch_ready(void *p,unsigned id)
{struct mock *m=p;CHECK(id<GGCB_CLOCKS);return m->ready;}
static int branch_quiet(void *p,unsigned id)
{struct mock *m=p;CHECK(id<GGCB_CLOCKS);return m->quiet;}
static int cx_ready(void *p)
{struct mock *m=p;return m->ready;}
static int gcc_ready(void *p,unsigned id)
{struct mock *m=p;CHECK(id==GGGC_AXI || id==GGGC_MEMNOC || id==GGGC_DVM);return m->ready;}
static int gcc_read(void *p,uint32_t off,uint32_t *v)
{
    struct mock *m=p;CHECK(off==0x154 || off==0x10 || off==0x18);++m->reads;
    if(event(m))return -1;
    *v=m->gcc[off/4];return 0;
}
static int gcc_write(void *p,uint32_t off,uint32_t v)
{
    struct mock *m=p;CHECK(off==0x154 || off==0x10 || off==0x18);++m->writes;
    m->gcc[off/4]=v&~(1u<<31);
    if(m->invalidate_write)ggbs_invalidate(&m->s);
    return event(m)?-1:0;
}
static int read32(void *p,uint32_t off,uint32_t *v)
{
    struct mock *m=p;CHECK(off<sizeof(m->reg) && !(off&3));++m->reads;
    if(event(m))return -1;
    if(off==0x1000 && m->reg[0x1004/4]==1 && !m->no_lock)m->reg[off/4]|=1u<<31;
    *v=m->reg[off/4];return 0;
}
static int write32(void *p,uint32_t off,uint32_t v)
{
    struct mock *m=p;CHECK(off<sizeof(m->reg) && !(off&3));++m->writes;
    CHECK(m->s.retained && m->s.tickets[GGR_CX].held && m->s.tickets[GGR_CX].serial);
    if(off==GGCX_CONTROL) {
        m->reg[off/4]=v;
        if(!(v&1u) && !m->cx_stall)m->reg[GGCX_STATUS/4]|=1u<<31;
    } else if(off==0x1000) {
        m->reg[off/4]=v;
        if((v&(1u<<14)) && !m->no_ack)m->reg[off/4]|=1u<<13;
        if(!(v&(1u<<14)))m->reg[off/4]&=~(1u<<13);
    } else if(off==0x9318 || off==0x93ec) {
        m->reg[off/4]=v;
        if(v&2u)m->reg[off/4]&=~(1u<<31);else m->reg[off/4]|=1u<<31;
        if(v&1u)m->reg[off/4]&=~0xf1u;
    } else if(off==0x931c || off==0x93f0) {
        m->reg[off/4]=v;m->reg[(off-4)/4]|=0x10u;
    } else {
        m->reg[off/4]=v;
        for(unsigned n=0;n<sizeof(branch_offsets)/sizeof(branch_offsets[0]);n++)
            if(off==branch_offsets[n] && (v&1u))m->reg[off/4]&=~(1u<<31);
    }
    if(m->invalidate_write)ggbs_invalidate(&m->s);
    return event(m)?-1:0; /* can fail after mutation */
}
static const struct ggpo_scope scope={
    {{0x03d00000,0x40000},{0x03d68000,0x37000},{0x03d40000,0x10000},
     {0x03d90000,0x9800},{0x03da0000,0x40000},{0x00171000,0x1000}},
     0x23,{332,336,337},GGPO_PREREQUISITES,0};
static void init_hub(struct mock *m,unsigned hub_branch)
{
    memset(m,0,sizeof(*m));m->valid=1;m->ready=m->quiet=1;
    for(unsigned n=0;n<sizeof(pll_init)/sizeof(pll_init[0]);n++)m->reg[pll_init[n][0]/4]=pll_init[n][1];
    m->reg[GGCX_CONTROL/4]=7u;
    m->reg[0x1000/4]=4;m->reg[0x9318/4]=m->reg[0x93ec/4]=0x80000400u;
    m->reg[0x931c/4]=m->reg[0x93f0/4]=0x100001u;
    for(unsigned n=0;n<sizeof(branch_offsets)/sizeof(branch_offsets[0]);n++)m->reg[branch_offsets[n]/4]=0x80000000u;
    const struct gg_power_ops power={m,resolve,quiet,acquire,release,observe,wait_us,trace};
    const struct ggpo_source up={GGPO_ABI,sizeof(up),m,take,live,drop,power};
    const struct ggbs_io io={m,rcg_ready,root_quiet,pll_ready,parked,branch_ready,branch_quiet,read32,write32,
        gcc_ready,gcc_read,gcc_write,cx_ready,hub_branch};
    CHECK(ggbs_init(&m->s,&up,&io)==GGBS_OK && !m->events);
    CHECK(ggbs_export(&m->s,&m->exposed)==GGBS_OK && !m->events);
}
static void init(struct mock *m)
{ init_hub(m,0); }
static int boot(struct mock *m)
{
    int e=m->exposed.take(m->exposed.cookie,17,&scope,&m->grant);
    if(e!=GGPO_TAKEN)return -1;
    return gg_power_acquire(&m->exposed.power,&m->power);
}
static void sealed(struct mock *m)
{
    unsigned events=m->events;
    CHECK(m->s.quarantined || m->s.lost);
    uint64_t token=0;
    CHECK(m->exposed.power.acquire(m->exposed.power.cookie,GGR_CX,1,&token)==GGRC_UNCERTAIN);
    CHECK(!token && m->events==events);
    CHECK(m->exposed.power.observe(m->exposed.power.cookie)==-1 && m->events==events);
    CHECK(m->exposed.drop(m->exposed.cookie,&m->grant)!=0 && m->events==events);
}
int main(void)
{
    struct mock *m=calloc(1,sizeof(*m));CHECK(m);
    CHECK(!strcmp(ggbs_identity(),GGBS_IDENTITY));
    const unsigned bindings[]={0,GGCB_HUB_AON,GGCB_HUB_CX_INT};
    for(unsigned binding=0;binding<sizeof(bindings)/sizeof(bindings[0]);binding++) {
        const unsigned hub=bindings[binding]?bindings[binding]:GGCB_HUB_AON;
        init_hub(m,bindings[binding]);CHECK(boot(m)==GGC_OK);
        CHECK((m->reg[(hub==GGCB_HUB_CX_INT?0x9148:0x93e8)/4]&1u)==1u);
        CHECK(m->reg[(hub==GGCB_HUB_CX_INT?0x93e8:0x9148)/4]==0x80000000u);
        CHECK(m->s.held_mask==((1u<<GGR_COUNT)-1u) && m->power.ready && m->s.retained);
        CHECK(!m->acquires && !m->delegated_mask && m->s.cx.enabled);
        CHECK((m->reg[GGCX_CONTROL/4]&0xf807u)==0x8800u);
        CHECK(m->s.gcc.requested_mask==((1u<<GGGC_AXI)|(1u<<GGGC_MEMNOC)|(1u<<GGGC_DVM)));
        CHECK(!(m->s.gcc.requested_mask&(1u<<GGGC_CFG)) && !m->gcc[4/4]);
        CHECK(m->s.pll.applied.requested_hz==1250000000u && m->s.rcg.hz[0]==625000000u && m->s.rcg.hz[1]==200000000u);
        CHECK(m->s.branches.requested_mask==((1u<<GGCB_GMU)|(1u<<GGCB_CXO)|(1u<<GGCB_AHB)|(1u<<hub)|(1u<<GGCB_SMMU)));
        unsigned events=m->events,lives=m->lives,reads=m->reads,writes=m->writes;
        struct ggbs_status status;CHECK(ggbs_status(&m->s,&status)==GGBS_OK);
        CHECK(m->events==events && status.retained && status.active && !status.quarantined);
        uint64_t token=99;CHECK(m->exposed.power.acquire(m->exposed.power.cookie,GGR_GMU_RATE,625000000,&token)==GGRC_UNCHANGED_ERROR && !token);
        CHECK(gg_power_release(&m->exposed.power,&m->power)!=GGC_OK);
        CHECK(!m->releases && !m->drops && m->reads==reads && m->writes==writes && m->power.held==((1u<<GGR_COUNT)-1u));sealed(m);
        for(unsigned at=1;at<=events;at++) {
            init_hub(m,bindings[binding]);m->fail_at=at;CHECK(boot(m)!=GGC_OK);
            if(m->s.quarantined || m->s.lost)sealed(m);
            CHECK(m->events==at && (m->writes!=0)==(m->s.writes!=0));
        }
        for(unsigned at=1;at<=lives;at++) {
            init_hub(m,bindings[binding]);m->lose_at=at;CHECK(boot(m)!=GGC_OK);sealed(m);
        }
    }
    uint64_t token=0;
    init(m);
    struct ggbs_state rejected={0};
    struct ggbs_io invalid=m->s.io;
    invalid.hub_branch=GGCB_GMU;
    CHECK(ggbs_init(&rejected,&m->s.upstream,&invalid)==GGBS_ARGUMENT);
    CHECK(!rejected.initialized && !m->events && !m->takes);
    invalid.hub_branch=GGCB_CLOCKS;
    CHECK(ggbs_init(&rejected,&m->s.upstream,&invalid)==GGBS_ARGUMENT);
    CHECK(!rejected.initialized && !m->events);
    init(m);m->deny=1;CHECK(boot(m)!=GGC_OK && !m->s.active && !m->reads && !m->writes);
    init(m);m->deny=m->bad_grant=1;CHECK(boot(m)!=GGC_OK && m->s.quarantined && !m->reads);sealed(m);
    init(m);m->bad_grant=1;CHECK(boot(m)!=GGC_OK && m->s.quarantined && !m->reads);sealed(m);
    init(m);m->uncertain=1;CHECK(boot(m)!=GGC_OK && m->s.active && !m->reads);sealed(m);
    init(m);m->ready=0;CHECK(boot(m)!=GGC_OK && m->s.retained && !m->writes);sealed(m);
    init(m);m->no_ack=1;CHECK(boot(m)!=GGC_OK && m->s.retained);sealed(m);
    init(m);m->no_lock=1;CHECK(boot(m)!=GGC_OK && m->s.pll.lock_polls==GGP1_POLLS);sealed(m);
    init(m);m->invalidate_write=1;CHECK(boot(m)!=GGC_OK && m->s.lost && m->writes==1);sealed(m);
    init(m);m->reg[0x1000/4]|=1u<<8;CHECK(boot(m)!=GGC_OK && !m->s.pll.started);sealed(m);
    init(m);m->cx_stall=1;
    CHECK(boot(m)!=GGC_OK && m->s.cx.last_code==GGCX_TIMEOUT && m->s.retained);
    CHECK(!m->acquires && !m->releases && !m->s.pll.started);sealed(m);
    init(m);CHECK(m->exposed.take(m->exposed.cookie,17,&scope,&m->grant)==GGPO_TAKEN);
    token=0;CHECK(m->exposed.power.acquire(m->exposed.power.cookie,GGR_CX,1,&token)==GGRC_OK);
    CHECK(m->exposed.power.release(m->exposed.power.cookie,GGR_CX,token+1)==GGBS_ARGUMENT);
    CHECK(m->exposed.power.release(m->exposed.power.cookie,GGR_CX,token)==GGBS_RETAINED);
    CHECK(!m->releases && m->s.held_mask==1u);sealed(m);
    init(m);CHECK(m->exposed.take(m->exposed.cookie,17,&scope,&m->grant)==GGPO_TAKEN);
    unsigned old=m->events;token=99;
    CHECK(m->exposed.power.acquire(m->exposed.power.cookie,GGR_GMU_RATE,260000000,&token)==GGRC_UNCHANGED_ERROR && !token && m->events==old);
    CHECK(m->exposed.power.acquire(m->exposed.power.cookie,GGR_HUB_RATE,200000000,&token)==GGRC_UNCHANGED_ERROR && !token && m->events==old);
    ggbs_invalidate(&m->s);sealed(m);ggbs_invalidate(NULL);
    init(m);CHECK(giulia_gpu_board_source_register(m,&m->s)==GGBS_OK && m->s.bound && binds==1 && !m->events);
    CHECK(giulia_gpu_board_source_register(m,&m->s)==GGBS_USED && binds==1);
    CHECK(ggbs_init(NULL,NULL,NULL)==GGBS_ARGUMENT && ggbs_export(NULL,NULL)==GGBS_ARGUMENT);
    CHECK(ggbs_status(NULL,NULL)==GGBS_ARGUMENT && giulia_gpu_board_source_register(NULL,NULL)==GGBS_ARGUMENT);
    memset(m,0,sizeof(*m));CHECK(ggbs_export(&m->s,&m->exposed)==GGBS_UNAVAILABLE);
    free(m);printf("GPU board source RAM-only assertions: %llu\n",checks);return 0;
}
