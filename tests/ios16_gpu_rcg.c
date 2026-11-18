/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Normal RAM fixture only. Not evidence of native hardware authority or rate.
 */
#include "giulia_gpu_rcg.h"
#include "rcg-source-policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
struct mock {
    struct ggrc_state state;
    uint32_t cmd[GGRCLK_ROOTS],cfg[GGRCLK_ROOTS];
    unsigned events,reads,writes,waits,lives,fail_at,lose_at;
    unsigned force_ticks[GGRCLK_ROOTS],update_ticks[GGRCLK_ROOTS];
    unsigned never_on,never_update,immediate,ignore_cfg,ignore_force,all_ones;
    unsigned fail_restore,drift_cfg,update_cfg_reads;
    int ready,quiet;
};
static unsigned index_at(uint32_t offset,unsigned *cfg)
{
    for(unsigned i=0;i<GGRCLK_ROOTS;i++) {
        if(offset==source_offsets[i]){*cfg=0;return i;}
        if(offset==source_offsets[i]+4){*cfg=1;return i;}
    }
    CHECK(0);return 0;
}
static int live(void *p)
{ struct mock *m=p;return ++m->lives==m->lose_at?0:1; }
static int ready(void *p,const struct ggrc_plan *plan)
{
    struct mock *m=p;CHECK(plan->root<GGRCLK_ROOTS);
    CHECK(plan->parent_hz>=19200000u);return m->ready;
}
static int quiet(void *p,unsigned root)
{ struct mock *m=p;CHECK(root<GGRCLK_ROOTS);return m->quiet; }
static int event(struct mock *m) { return ++m->events==m->fail_at; }
static int read32(void *p,uint32_t offset,uint32_t *out)
{
    struct mock *m=p;unsigned cfg=0,id=index_at(offset,&cfg);++m->reads;
    if(event(m))return -1;
    if(!cfg) {
        if((m->cmd[id]&2) && !m->never_on && ++m->force_ticks[id]>=2)m->cmd[id]&=~0x80000000u;
        if((m->cmd[id]&1) && !m->never_update && ++m->update_ticks[id]>=2)m->cmd[id]&=~0xf1u;
    } else if(m->drift_cfg && ++m->update_cfg_reads==3) m->cfg[id]^=1;
    *out=m->all_ones?0xffffffffu:(cfg?m->cfg[id]:m->cmd[id]);return 0;
}
static int write32(void *p,uint32_t offset,uint32_t value)
{
    struct mock *m=p;unsigned cfg=0,id=index_at(offset,&cfg);++m->writes;
    if(cfg) {
        CHECK(((m->cfg[id]^value)&~source_mask)==0);
        if(!m->ignore_cfg){m->cfg[id]=value;m->cmd[id]|=0x10;}
    } else {
        CHECK(((m->cmd[id]^value)&~3u)==0); /* only ROOT_EN / UPDATE */
        if(m->fail_restore && !(value&2))return -1;
        if(!m->ignore_force) {
            m->cmd[id]=value;
            if(value&1) { m->update_ticks[id]=0;if(m->immediate)m->cmd[id]&=~0xf1u; }
            if(!(value&2))m->cmd[id]|=0x80000000u;
        }
    }
    /* Error may be reported after mutation. Never assume unchanged. */
    return event(m)?-1:0;
}
static int wait_us(void *p,unsigned us)
{ struct mock *m=p;CHECK(us==1);++m->waits;return event(m)?-1:0; }
static void init(struct mock *m)
{
    memset(m,0,sizeof(*m));m->ready=m->quiet=1;
    for(unsigned i=0;i<GGRCLK_ROOTS;i++){m->cmd[i]=0x80000400u;m->cfg[i]=0x48a0a6e0u;}
    const struct ggrc_io io={m,live,ready,quiet,read32,write32,wait_us};
    CHECK(ggrc_open(&m->state,&io)==GGRCLK_OK && !m->events);
}
static void sealed(struct mock *m)
{
    unsigned events=m->events;CHECK(m->state.quarantined);
    for(unsigned i=0;i<GGRCLK_ROOTS;i++)CHECK(ggrc_apply(&m->state,i,19200000)==GGRCLK_QUARANTINED);
    CHECK(events==m->events);
}
int main(void)
{
    struct mock m;struct ggrc_plan p;
    CHECK(!strcmp(ggrc_identity(),GGRCLK_IDENTITY));
    for(unsigned n=0;n<sizeof(source_plans)/sizeof(source_plans[0]);n++) {
        const struct ggrc_plan *oracle=&source_plans[n];
        CHECK(ggrc_describe(oracle->root,oracle->hz,&p)==GGRCLK_OK);
        CHECK(!memcmp(&p,oracle,sizeof(p)));
        CHECK(p.cmd==source_offsets[p.root] && p.cfg_mask==source_mask);
        for(unsigned inherited=0;inherited<2;inherited++) {
            init(&m);unsigned root=p.root;
            if(inherited)m.cmd[root]|=2;
            uint32_t other=m.cfg[root]&~p.cfg_mask;
            CHECK(ggrc_apply(&m.state,root,p.hz)==GGRCLK_OK);
            CHECK((m.cfg[root]&p.cfg_mask)==p.cfg_value);
            CHECK((m.cfg[root]&~p.cfg_mask)==other);
            CHECK((m.cmd[root]&2)==inherited*2);
            CHECK(!(m.cmd[root]&1) && !m.state.force_added_mask);
            CHECK(m.state.configured_mask==(1u<<root) && m.state.hz[root]==p.hz);
            CHECK(m.state.phase==GGRCLK_DONE && !m.state.quarantined);
            CHECK(m.writes==(inherited?2u:4u));
            if(!inherited)CHECK(m.cmd[root]&0x80000000u); /* gated downstream is legal */
            for(unsigned i=0;i<GGRCLK_ROOTS;i++)if(i!=root)CHECK(m.cfg[i]==0x48a0a6e0u && m.cmd[i]==0x80000400u);
            /* Exhaust all normal I/O/wait failure and live-loss boundaries. */
            unsigned events=m.events,lives=m.lives;
            for(unsigned at=1;at<=events;at++) {
                init(&m);if(inherited)m.cmd[root]|=2;m.fail_at=at;
                CHECK(ggrc_apply(&m.state,root,p.hz)!=GGRCLK_OK);
                CHECK(m.events==at);sealed(&m);
            }
            for(unsigned at=2;at<=lives;at++) {
                init(&m);if(inherited)m.cmd[root]|=2;m.lose_at=at;
                CHECK(ggrc_apply(&m.state,root,p.hz)==GGRCLK_LOST);sealed(&m);
            }
        }
    }
    init(&m);m.ready=0;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_UNAVAILABLE && !m.events);
    m.ready=-1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_LOST);sealed(&m);
    init(&m);m.quiet=0;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_BUSY && !m.events);
    m.quiet=-1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_LOST);sealed(&m);
    for(unsigned bit=0;bit<8;bit++)if((1u<<bit)&0xf1u) {
        init(&m);m.cmd[0]|=1u<<bit;
        CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_BUSY && !m.writes && !m.state.quarantined);
    }
    init(&m);m.never_on=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_TIMEOUT);
    CHECK(m.state.root_polls==GGRCLK_POLLS && m.waits==GGRCLK_POLLS && m.writes==1);
    CHECK(m.state.force_added_mask==1);sealed(&m);
    init(&m);m.never_update=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_TIMEOUT);
    CHECK(m.state.update_polls==GGRCLK_POLLS && m.writes==3 && m.state.force_added_mask==1);sealed(&m);
    init(&m);m.immediate=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_OK); /* self-clear before first read */
    init(&m);m.ignore_cfg=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_READBACK);sealed(&m);
    init(&m);m.ignore_force=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_READBACK);sealed(&m);
    init(&m);m.all_ones=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_IO && !m.writes);sealed(&m);
    init(&m);m.fail_restore=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_IO);
    CHECK(!m.state.configured_mask && m.state.force_added_mask==1);sealed(&m);
    init(&m);m.drift_cfg=1;CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_READBACK);sealed(&m);
    init(&m);CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_OK);
    CHECK(ggrc_apply(&m.state,0,19200000)==GGRCLK_OK);
    CHECK(ggrc_apply(&m.state,0,625000000)==GGRCLK_OK && m.state.hz[0]==625000000);
    CHECK(ggrc_apply(&m.state,0,19200000)==GGRCLK_OK && m.state.hz[0]==19200000);
    CHECK(ggrc_apply(&m.state,1,200000000)==GGRCLK_OK && m.state.configured_mask==3);
    CHECK(ggrc_open(&m.state,&m.state.io)==GGRCLK_USED);
    ggrc_invalidate(&m.state);sealed(&m);
    memset(&m,0,sizeof(m));CHECK(ggrc_apply(&m.state,0,260000000)==GGRCLK_USED);
    CHECK(ggrc_describe(0,260000001,&p)==GGRCLK_ARGUMENT && !p.hz && !p.cmd);
    CHECK(ggrc_describe(GGRCLK_ROOTS,19200000,&p)==GGRCLK_ARGUMENT && !p.cfg_mask);
    CHECK(ggrc_describe(0,19200000,NULL)==GGRCLK_ARGUMENT);
    CHECK(ggrc_open(NULL,NULL)==GGRCLK_ARGUMENT && ggrc_open(&m.state,NULL)==GGRCLK_ARGUMENT);
    struct ggrc_io io={0};CHECK(ggrc_open(&m.state,&io)==GGRCLK_ARGUMENT);
    CHECK(ggrc_apply(NULL,0,19200000)==GGRCLK_ARGUMENT);
    CHECK(ggrc_apply(&m.state,GGRCLK_ROOTS,19200000)==GGRCLK_ARGUMENT);
    CHECK(ggrc_apply(&m.state,0,0)==GGRCLK_ARGUMENT);ggrc_invalidate(NULL);
    for(unsigned root=0;root<GGRCLK_ROOTS;root++) {
        init(&m);
        const uint32_t invalid[]={0,1,19200001u,260000001u,0xffffffffu};
        for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
            CHECK(ggrc_apply(&m.state,root,invalid[i])==GGRCLK_ARGUMENT);
        CHECK(!m.events);
    }
    init(&m);CHECK(ggrc_apply(&m.state,GGRCLK_HUB,625000000)==GGRCLK_ARGUMENT);
    CHECK(ggrc_apply(&m.state,GGRCLK_FF,300000000)==GGRCLK_ARGUMENT && !m.events);
    printf("GPU RCG RAM-only assertions: %llu\n",checks);return 0;
}
