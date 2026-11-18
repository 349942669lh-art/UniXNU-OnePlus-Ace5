/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * CPU RAM model, not hardware execution or a production platform provider.
 */
#include "giulia_gpu_gpucc.h"
#include "gpucc-source-policy.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
struct mock {
    uint32_t reg[GGCB_CLOCKS];
    unsigned live_calls,ready_calls,quiet_calls,reads,writes,waits,events;
    unsigned fail_event,lose_at_live,pending[GGCB_CLOCKS],ticks[GGCB_CLOCKS];
    unsigned wait_1,wait_10,never_ready,unlatched_ahb,ignore_vote,all_ones;
    int ready,quiet;
    struct ggcb_state state;
};
static unsigned index_at(uint32_t offset)
{
    for(unsigned i=0;i<GGCB_CLOCKS;i++)if(source_offsets[i]==offset)return i;
    CHECK(0);return 0;
}
static int live(void *p)
{
    struct mock *m=p;++m->live_calls;
    return m->lose_at_live && m->live_calls>=m->lose_at_live?0:1;
}
static int ready(void *p,unsigned id)
{ struct mock *m=p;CHECK(id<GGCB_CLOCKS);++m->ready_calls;return m->ready; }
static int quiet(void *p,unsigned id)
{ struct mock *m=p;CHECK(id<GGCB_CLOCKS);++m->quiet_calls;return m->quiet; }
static int event(struct mock *m)
{ return ++m->events==m->fail_event; }
static int read32(void *p,uint32_t offset,uint32_t *value)
{
    struct mock *m=p;unsigned id=index_at(offset);++m->reads;
    if(event(m))return -7;
    if(m->pending[id] && !m->never_ready && ++m->ticks[id]>=3) {
        if(m->pending[id]==1)m->reg[id]&=~0x80000000u;
        else m->reg[id]|=0x80000000u;
        m->pending[id]=0;
    }
    *value=m->all_ones?0xffffffffu:m->reg[id];return 0;
}
static int write32(void *p,uint32_t offset,uint32_t value)
{
    struct mock *m=p;unsigned id=index_at(offset);++m->writes;
    CHECK(((value^m->reg[id])&~1u)==0); /* Only vote bit changes. */
    if(!m->ignore_vote) {
        m->reg[id]=value;m->ticks[id]=0;
        if(id==GGCB_AHB && m->unlatched_ahb)m->reg[id]&=~1u;
        if(source_policies[id]!=GGCB_DELAY) {
            if(value&1u){m->reg[id]|=0x80000000u;m->pending[id]=1;}
            else if(source_policies[id]==GGCB_HALT){m->reg[id]&=~0x80000000u;m->pending[id]=2;}
            else {m->reg[id]&=~0x80000000u;m->pending[id]=0;}
        }
    }
    /* Failure can occur AFTER a real write. No fake unchanged result. */
    return event(m)?-8:0;
}
static int wait_us(void *p,unsigned us)
{
    struct mock *m=p;++m->waits;
    CHECK(us==1 || us==10);
    if(us==1)++m->wait_1;else ++m->wait_10;
    return event(m)?-9:0;
}
static void init(struct mock *m)
{
    memset(m,0,sizeof(*m));m->ready=m->quiet=1;
    for(unsigned i=0;i<GGCB_CLOCKS;i++)m->reg[i]=0x84104240u;
    const struct ggcb_io io={m,live,ready,quiet,read32,write32,wait_us};
    CHECK(ggcb_open(&m->state,&io)==GGCB_OK);
    CHECK(!m->reads && !m->writes && !m->waits);
}
static void sealed(struct mock *m)
{
    unsigned events=m->events;
    CHECK(m->state.quarantined);
    for(unsigned id=0;id<GGCB_CLOCKS;id++)
        CHECK(ggcb_apply(&m->state,id,1)==GGCB_QUARANTINED);
    CHECK(m->events==events);
}
int main(void)
{
    struct mock m;struct ggcb_branch b;
    CHECK(!strcmp(ggcb_identity(),GGCB_IDENTITY));
    for(unsigned id=0;id<GGCB_CLOCKS;id++) {
        CHECK(ggcb_describe(id,&b)==GGCB_OK);
        CHECK(b.offset==source_offsets[id] && b.policy==source_policies[id] && b.aon==source_aon[id]);
    }
    CHECK(ggcb_describe(GGCB_CLOCKS,&b)==GGCB_ARGUMENT && !b.offset && !b.policy && !b.aon);
    CHECK(ggcb_describe(0,NULL)==GGCB_ARGUMENT);
    CHECK(ggcb_open(NULL,NULL)==GGCB_ARGUMENT);
    CHECK(ggcb_apply(NULL,0,1)==GGCB_ARGUMENT);ggcb_invalidate(NULL);
    for(unsigned id=0;id<GGCB_CLOCKS;id++) {
        init(&m);unsigned bit=1u<<id;
        CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK);
        CHECK(m.state.requested_mask==bit && m.state.changed_mask==bit && m.state.touched_mask==bit);
        CHECK(m.writes==1);
        CHECK((m.state.before&~1u)==(0x84104240u&~1u));
        CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK && m.writes==1); /* idempotent, not an extra ref */
        unsigned events=m.events;
        if(source_aon[id]) {
            CHECK(m.state.pinned_mask==bit);
            CHECK(ggcb_apply(&m.state,id,0)==GGCB_PINNED);
            CHECK(m.events==events && m.state.requested_mask==bit);
        } else {
            CHECK(ggcb_apply(&m.state,id,0)==GGCB_OK);
            CHECK(m.writes==2 && !m.state.changed_mask && !m.state.requested_mask);
            if(source_policies[id]==GGCB_VOTED) {
                CHECK(!(m.reg[id]&0x80000000u)); /* another voter still runs clock */
                CHECK(m.wait_10==1);
            }
            CHECK(ggcb_apply(&m.state,id,0)==GGCB_NOT_OWNED);
        }
        /* Inherited votes are never converted into independently clearable votes. */
        init(&m);m.reg[id]=0x04104241u;
        CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK && !m.writes);
        CHECK(m.state.inherited_mask==bit && !m.state.changed_mask);
        events=m.events;
        CHECK(ggcb_apply(&m.state,id,0)==(source_aon[id]?GGCB_PINNED:GGCB_INHERITED));
        CHECK(m.events==events);
    }
    init(&m);m.unlatched_ahb=1;
    CHECK(ggcb_apply(&m.state,GGCB_AHB,1)==GGCB_OK && !(m.reg[GGCB_AHB]&1));
    CHECK(!m.state.polls && m.wait_10==1);
    CHECK(ggcb_apply(&m.state,GGCB_AHB,0)==GGCB_OK && m.wait_10==2);

    init(&m);m.ready=0;
    CHECK(ggcb_apply(&m.state,GGCB_GMU,1)==GGCB_UNAVAILABLE && !m.events && !m.state.quarantined);
    m.ready=-1;CHECK(ggcb_apply(&m.state,GGCB_GMU,1)==GGCB_LOST);sealed(&m);
    init(&m);CHECK(ggcb_apply(&m.state,GGCB_CXO,1)==GGCB_OK);
    unsigned events=m.events;m.quiet=0;
    CHECK(ggcb_apply(&m.state,GGCB_CXO,0)==GGCB_BUSY && m.events==events && !m.state.quarantined);
    m.quiet=-1;CHECK(ggcb_apply(&m.state,GGCB_CXO,0)==GGCB_LOST);sealed(&m);
    init(&m);m.all_ones=1;
    CHECK(ggcb_apply(&m.state,GGCB_SMMU,1)==GGCB_IO && !m.writes);sealed(&m);
    init(&m);m.ignore_vote=1;
    CHECK(ggcb_apply(&m.state,GGCB_SMMU,1)==GGCB_READBACK);sealed(&m);
    init(&m);m.never_ready=1;
    CHECK(ggcb_apply(&m.state,GGCB_CXO,1)==GGCB_TIMEOUT);
    CHECK(m.state.polls==GGCB_POLLS && m.wait_1==GGCB_POLLS && m.writes==1);sealed(&m);
    init(&m);CHECK(ggcb_apply(&m.state,GGCB_CXO,1)==GGCB_OK);
    m.reg[GGCB_CXO]&=~1u;
    CHECK(ggcb_apply(&m.state,GGCB_CXO,0)==GGCB_LOST);sealed(&m);
    init(&m);ggcb_invalidate(&m.state);sealed(&m);
    init(&m);CHECK(ggcb_apply(&m.state,GGCB_CLOCKS,1)==GGCB_ARGUMENT);
    CHECK(ggcb_apply(&m.state,0,2)==GGCB_ARGUMENT && !m.events);
    CHECK(ggcb_open(&m.state,&m.state.io)==GGCB_USED);
    /* Every successful read/write/wait position becomes an injected failure;
     * verify no automatic rollback and no further I/O after quarantine. */
    for(unsigned id=0;id<GGCB_CLOCKS;id++) {
        init(&m);CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK);
        unsigned count=m.events;
        for(unsigned at=1;at<=count;at++) {
            init(&m);m.fail_event=at;
            CHECK(ggcb_apply(&m.state,id,1)!=GGCB_OK);
            CHECK(m.events==at);sealed(&m);
        }
        init(&m);CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK);
        unsigned lives=m.live_calls;
        for(unsigned at=2;at<=lives;at++) {
            init(&m);m.lose_at_live=at;
            CHECK(ggcb_apply(&m.state,id,1)==GGCB_LOST);sealed(&m);
        }
        if(!source_aon[id]) {
            init(&m);CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK);
            unsigned start=m.events;
            CHECK(ggcb_apply(&m.state,id,0)==GGCB_OK);
            count=m.events-start;
            for(unsigned at=1;at<=count;at++) {
                init(&m);CHECK(ggcb_apply(&m.state,id,1)==GGCB_OK);
                m.fail_event=m.events+at;
                CHECK(ggcb_apply(&m.state,id,0)!=GGCB_OK);sealed(&m);
            }
        }
    }
    /* All branch2 status combinations, compared to independent source rule.
     * AON/persistent votes do not change this hardware-ready predicate. */
    for(unsigned high=0;high<16;high++) {
        init(&m);m.reg[GGCB_GMU]=(high<<28)|1u;
        int e=ggcb_apply(&m.state,GGCB_GMU,1);
        unsigned expected=((high&8)==0 || high==2);
        CHECK(e==(expected?GGCB_OK:GGCB_TIMEOUT));
        CHECK(!m.writes);
    }
    printf("GPUCC branch RAM-only assertions: %llu\n",checks);
    return 0;
}
