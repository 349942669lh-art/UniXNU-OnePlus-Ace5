/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RAM-only source-derived policy and callback-boundary failure injection.
 */
#include "giulia_gpu_gcc.h"
#include "gcc-source-policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
struct mock {
    struct gggc_state state;
    uint32_t reg[GGGC_BYTES/4],last_written;
    unsigned events,fail_at,lives,lose_at,valid,ready_value;
    unsigned reads,writes,waits,stall,no_latch,bad_read;
};
static int event(struct mock *m) { return ++m->events==m->fail_at; }
static int live(void *p)
{
    struct mock *m=p;
    if(++m->lives==m->lose_at)m->valid=0;
    return (int)m->valid;
}
static int ready(void *p,unsigned id)
{ struct mock *m=p;CHECK(id<GGGC_CLOCKS);return event(m)?-1:(int)m->ready_value; }
static int known(uint32_t off)
{
    for(unsigned id=0;id<GGGC_CLOCKS;++id)if(source_offsets[id]==off)return 1;
    return 0;
}
static int read32(void *p,uint32_t off,uint32_t *value)
{
    struct mock *m=p;CHECK(known(off));++m->reads;
    if(event(m))return -1;
    *value=m->reg[off/4];
    if(m->no_latch)*value&=~1u;
    if(m->bad_read)*value=0xffffffffu;
    return 0;
}
static int write32(void *p,uint32_t off,uint32_t value)
{
    struct mock *m=p;CHECK(known(off));++m->writes;
    CHECK((m->reg[off/4]^value)==1u); /* Preserve all non-enable fields. */
    m->last_written=value;m->reg[off/4]=value;
    if(!m->stall)m->reg[off/4]&=~(1u<<31);
    return event(m)?-1:0; /* Failure can occur AFTER the device saw the write. */
}
static int wait_us(void *p,unsigned us)
{ struct mock *m=p;CHECK(us==1 || us==10);++m->waits;return event(m)?-1:0; }
static void init(struct mock *m)
{
    memset(m,0,sizeof(*m));m->valid=m->ready_value=1;
    for(unsigned id=0;id<GGGC_CLOCKS;++id)m->reg[source_offsets[id]/4]=0x80000400u;
    const struct gggc_io io={m,live,ready,read32,write32,wait_us};
    CHECK(gggc_open(&m->state,&io)==GGGC_OK && !m->events);
    m->lives=0;
}
static int enable_all(struct mock *m)
{
    for(unsigned id=0;id<GGGC_CLOCKS;++id) {
        int code=gggc_enable(&m->state,id);if(code)return code;
    }
    return 0;
}
static void sealed(struct mock *m)
{
    unsigned events=m->events,lives=m->lives,code=m->state.last_code;
    CHECK(m->state.quarantined);
    for(unsigned id=0;id<GGGC_CLOCKS;++id)CHECK(gggc_enable(&m->state,id)==GGGC_QUARANTINED);
    gggc_invalidate(&m->state);
    CHECK(m->events==events && m->lives==lives && m->state.last_code==code);
}
int main(void)
{
    struct mock m;
    CHECK(!strcmp(gggc_identity(),GGGC_IDENTITY));
    for(unsigned id=0;id<GGGC_CLOCKS;++id) {
        struct gggc_branch b;CHECK(gggc_describe(id,&b)==GGGC_OK);
        CHECK(b.offset==source_offsets[id] && b.policy==source_policies[id] && b.hwcg_mask==source_hwcg[id]);
    }
    init(&m);CHECK(!enable_all(&m));
    CHECK(m.state.requested_mask==15 && m.state.touched_mask==15 && !m.state.inherited_mask);
    CHECK(m.writes==4 && m.waits==1 && m.state.polls==1);
    unsigned events=m.events,lives=m.lives;
    CHECK(!enable_all(&m) && m.writes==4); /* No duplicated writes/ownership. */
    for(unsigned at=1;at<=events;++at) {
        init(&m);m.fail_at=at;CHECK(enable_all(&m)!=GGGC_OK);
        CHECK(m.events==at);sealed(&m);
    }
    for(unsigned at=1;at<=lives;++at) {
        init(&m);m.lose_at=at;CHECK(enable_all(&m)!=GGGC_OK);sealed(&m);
    }
    for(unsigned id=0;id<GGGC_CLOCKS;++id) {
        init(&m);m.reg[source_offsets[id]/4]|=1u;
        m.reg[source_offsets[id]/4]&=~(1u<<31);
        CHECK(gggc_enable(&m.state,id)==GGGC_OK && !m.writes);
        CHECK(m.state.inherited_mask==(1u<<id) && !m.state.touched_mask);
        init(&m);m.ready_value=0;
        CHECK(gggc_enable(&m.state,id)==GGGC_UNAVAILABLE && !m.reads && !m.writes && !m.state.quarantined);
        init(&m);m.bad_read=1;
        CHECK(gggc_enable(&m.state,id)==GGGC_IO && !m.writes);sealed(&m);
        init(&m);m.no_latch=1;
        int code=gggc_enable(&m.state,id);
        if(id==GGGC_AXI) {
            CHECK(code==GGGC_OK && m.writes==1 && !m.waits && !m.state.polls);
            CHECK(gggc_enable(&m.state,id)==GGGC_OK && m.writes==1);
        } else { CHECK(code==GGGC_READBACK && m.writes==1);sealed(&m); }
    }
    /* HWCG bypasses MEMNOC halt checks, exactly as the pinned source says. */
    init(&m);m.stall=1;m.reg[source_offsets[GGGC_MEMNOC]/4]|=2u;
    CHECK(gggc_enable(&m.state,GGGC_MEMNOC)==GGGC_OK && !m.waits && !m.state.polls);
    init(&m);m.stall=1;
    CHECK(gggc_enable(&m.state,GGGC_MEMNOC)==GGGC_TIMEOUT);
    CHECK(m.state.polls==GGGC_POLLS && m.waits==GGGC_POLLS-1 && m.writes==1);sealed(&m);
    init(&m);m.stall=1;CHECK(gggc_enable(&m.state,GGGC_MEMNOC)==GGGC_TIMEOUT);
    unsigned timeout_events=m.events,timeout_lives=m.lives;
    /* Inject at every read/wait/live boundary of the longest polling path. */
    for(unsigned at=1;at<=timeout_events;++at) {
        init(&m);m.stall=1;m.fail_at=at;
        CHECK(gggc_enable(&m.state,GGGC_MEMNOC)!=GGGC_OK && m.events==at);sealed(&m);
    }
    for(unsigned at=1;at<=timeout_lives;++at) {
        init(&m);m.stall=1;m.lose_at=at;
        CHECK(gggc_enable(&m.state,GGGC_MEMNOC)==GGGC_LOST);sealed(&m);
    }
    /* AXI never polls halt; DVM uses delay even with CLK_OFF set. */
    init(&m);m.stall=1;
    CHECK(gggc_enable(&m.state,GGGC_AXI)==GGGC_OK && !m.waits && !m.state.polls);
    CHECK(gggc_enable(&m.state,GGGC_DVM)==GGGC_OK && m.waits==1 && !m.state.polls);
    /* Loss of a previously held ordinary vote is not silently repaired. */
    for(unsigned id=0;id<GGGC_CLOCKS;++id)if(id!=GGGC_AXI) {
        init(&m);CHECK(gggc_enable(&m.state,id)==GGGC_OK);
        m.reg[source_offsets[id]/4]&=~1u;unsigned writes=m.writes;
        CHECK(gggc_enable(&m.state,id)==GGGC_LOST && m.writes==writes);sealed(&m);
    }
    init(&m);gggc_invalidate(&m.state);sealed(&m);gggc_invalidate(NULL);
    struct gggc_state empty={0};struct gggc_branch b={1,1,1};
    CHECK(gggc_enable(&empty,0)==GGGC_USED);
    CHECK(gggc_enable(NULL,0)==GGGC_ARGUMENT && gggc_enable(&empty,GGGC_CLOCKS)==GGGC_ARGUMENT);
    CHECK(gggc_describe(GGGC_CLOCKS,&b)==GGGC_ARGUMENT && !b.offset && !b.policy && !b.hwcg_mask);
    CHECK(gggc_describe(0,NULL)==GGGC_ARGUMENT && gggc_open(NULL,NULL)==GGGC_ARGUMENT);
    CHECK(gggc_open(&empty,NULL)==GGGC_ARGUMENT);
    init(&m);CHECK(gggc_open(&m.state,&m.state.io)==GGGC_USED);
    printf("GPU GCC RAM-only assertions: %u\n",checks);return 0;
}
