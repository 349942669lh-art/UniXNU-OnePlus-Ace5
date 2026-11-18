/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RAM-only transaction and callback boundary tests using public source data.
 */
#include "giulia_gpu_cx.h"
#include "cx-source-policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); abort(); } } while(0)
struct mock {
    struct ggcx_state state;
    uint32_t control,status;
    unsigned events,fail_at,lives,lose_at,valid;
    int ready,quiet;
    unsigned reads,writes,waits,status_reads,power_after,stall,bad_read;
    unsigned no_latch,flip_mode,late_other_bits,drop_final;
    unsigned handover_wait_seen,prepare_readbacks,enable_wait_seen,retain_waits;
};
static int event(struct mock *m) { return ++m->events==m->fail_at; }
static int live(void *p)
{
    struct mock *m=p;
    if(++m->lives==m->lose_at)m->valid=0;
    return (int)m->valid;
}
static int ready(void *p) { struct mock *m=p; return event(m)?-1:m->ready; }
static int quiet(void *p) { struct mock *m=p; return event(m)?-1:m->quiet; }
static int read32(void *p,uint32_t off,uint32_t *value)
{
    struct mock *m=p;
    CHECK(off==source_policy[0] || off==source_policy[1]);
    ++m->reads;
    if(event(m))return -1;
    if(off==source_policy[1]) {
        if(m->state.phase==GGCX_POLL)CHECK(m->enable_wait_seen);
        if(m->state.phase==GGCX_RETAIN)CHECK(m->retain_waits==2);
        ++m->status_reads;
        if(m->power_after && m->status_reads>=m->power_after)m->status|=source_policy[7];
        if(m->drop_final && m->state.phase==GGCX_RETAIN)m->status=0;
        *value=m->status;
    } else {
        if(m->state.phase==GGCX_ENABLE && (m->state.before&2u))CHECK(m->handover_wait_seen);
        if(m->state.phase==GGCX_PREPARE && (m->control&0xf006u)==0x8000u)++m->prepare_readbacks;
        if(m->flip_mode && m->state.phase==GGCX_ENABLE)m->control|=2u;
        if(m->late_other_bits && m->state.phase==GGCX_RETAIN)m->control|=0x10000u;
        *value=m->control;
    }
    if(m->bad_read)*value=0xffffffffu;
    return 0;
}
static int write32(void *p,uint32_t off,uint32_t value)
{
    struct mock *m=p;
    CHECK(off==source_policy[0]);
    uint32_t allowed=m->state.phase==GGCX_PREPARE?(source_policy[2]|source_policy[3]):
                     m->state.phase==GGCX_ENABLE?source_policy[5]:source_policy[6];
    CHECK(!((value^m->control)&~allowed));
    CHECK(m->state.mutation_attempted && m->state.attempted);
    ++m->writes;
    if(!m->no_latch)m->control=value;
    if(!(value&source_policy[5]) && !m->stall)m->status|=source_policy[7];
    return event(m)?-1:0;
}
static int wait_us(void *p,unsigned us)
{
    struct mock *m=p;
    CHECK(us==1);
    ++m->waits;
    if(m->state.phase==GGCX_PREPARE) {
        CHECK((m->control&0xf006u)==0x8000u && m->prepare_readbacks);
        m->handover_wait_seen=1;
    }
    if(m->state.phase==GGCX_ENABLE)m->enable_wait_seen=1;
    if(m->state.phase==GGCX_RETAIN)++m->retain_waits;
    return event(m)?-1:0;
}
static void init(struct mock *m)
{
    memset(m,0,sizeof(*m));
    m->valid=1;m->ready=m->quiet=1;
    m->control=0x50000007u;
    const struct ggcx_io io={m,live,ready,quiet,read32,write32,wait_us};
    CHECK(ggcx_open(&m->state,&io)==GGCX_OK && !m->events);
    m->lives=0;
}
static void sealed(struct mock *m)
{
    unsigned events=m->events,lives=m->lives,code=m->state.last_code;
    CHECK(m->state.quarantined);
    CHECK(ggcx_enable(&m->state)==GGCX_QUARANTINED);
    ggcx_invalidate(&m->state);
    CHECK(m->events==events && m->lives==lives && m->state.last_code==code);
}
int main(void)
{
    CHECK(!strcmp(ggcx_identity(),GGCX_IDENTITY));
    struct ggcx_policy policy={0};
    CHECK(ggcx_describe(&policy)==GGCX_OK);
    const uint32_t actual[]={policy.control_offset,policy.status_offset,policy.mode_mask,
        policy.wait_mask,policy.wait_value,policy.collapse_mask,policy.retain_mask,
        policy.power_mask,policy.polls_per_window,policy.poll_windows};
    CHECK(sizeof(actual)==sizeof(source_policy) && !memcmp(actual,source_policy,sizeof(actual)));
    struct mock m;
    init(&m);CHECK(ggcx_enable(&m.state)==GGCX_OK);
    CHECK(m.state.enabled && m.state.phase==GGCX_DONE && m.state.mutation_attempted);
    CHECK((m.control&0xf807u)==0x8800u && (m.control&0x50000000u)==0x50000000u);
    CHECK(m.writes==3 && m.waits==4 && m.state.polls==1 && m.state.poll_window==1);
    unsigned events=m.events,lives=m.lives;
    CHECK(ggcx_enable(&m.state)==GGCX_USED && m.events==events);
    for(unsigned at=1;at<=events;++at) {
        init(&m);m.fail_at=at;
        CHECK(ggcx_enable(&m.state)!=GGCX_OK && m.events==at);sealed(&m);
    }
    for(unsigned at=1;at<=lives;++at) {
        init(&m);m.lose_at=at;
        CHECK(ggcx_enable(&m.state)==GGCX_LOST);sealed(&m);
    }
    init(&m);m.control=0x50008800u;m.status=0x80000000u;
    CHECK(ggcx_enable(&m.state)==GGCX_OK && m.state.inherited && !m.writes && !m.state.mutation_attempted);
    for(unsigned modes=0;modes<8;++modes) {
        for(unsigned retained=0;retained<2;++retained) {
            init(&m);
            m.control=0x50008000u|modes|(retained?0x800u:0u);
            m.status=(modes&1u)?0u:0x80000000u;
            CHECK(ggcx_enable(&m.state)==GGCX_OK);
            CHECK((m.control&0xf807u)==0x8800u);
            CHECK(m.state.inherited==!(modes&1u));
            CHECK(m.handover_wait_seen==!!(modes&2u));
        }
    }
    init(&m);m.ready=0;
    CHECK(ggcx_enable(&m.state)==GGCX_UNAVAILABLE && !m.reads && !m.state.attempted);
    init(&m);m.quiet=0;
    CHECK(ggcx_enable(&m.state)==GGCX_BUSY && !m.reads && !m.state.attempted);
    init(&m);m.ready=-1;CHECK(ggcx_enable(&m.state)==GGCX_LOST);sealed(&m);
    init(&m);m.quiet=-1;CHECK(ggcx_enable(&m.state)==GGCX_LOST);sealed(&m);
    init(&m);m.bad_read=1;CHECK(ggcx_enable(&m.state)==GGCX_IO && !m.writes);sealed(&m);
    init(&m);m.no_latch=1;CHECK(ggcx_enable(&m.state)==GGCX_READBACK && m.writes==1);sealed(&m);
    init(&m);m.flip_mode=1;CHECK(ggcx_enable(&m.state)==GGCX_READBACK && m.writes==1);sealed(&m);
    init(&m);m.late_other_bits=1;
    CHECK(ggcx_enable(&m.state)==GGCX_OK && (m.control&0x10000u));
    init(&m);m.drop_final=1;CHECK(ggcx_enable(&m.state)==GGCX_READBACK);sealed(&m);
    init(&m);m.stall=1;m.power_after=GGCX_POLLS+5;
    CHECK(ggcx_enable(&m.state)==GGCX_OK && m.state.poll_window==2 && m.writes==3);
    init(&m);m.stall=1;
    CHECK(ggcx_enable(&m.state)==GGCX_TIMEOUT && !m.state.enabled && m.writes==2);
    CHECK(m.state.polls==GGCX_POLLS*GGCX_WINDOWS && m.waits==GGCX_POLLS*GGCX_WINDOWS+2);
    events=m.events;lives=m.lives;sealed(&m);
    for(unsigned at=1;at<=events;++at) {
        init(&m);m.stall=1;m.fail_at=at;
        CHECK(ggcx_enable(&m.state)!=GGCX_OK && m.events==at);sealed(&m);
    }
    for(unsigned at=1;at<=lives;++at) {
        init(&m);m.stall=1;m.lose_at=at;
        CHECK(ggcx_enable(&m.state)==GGCX_LOST);sealed(&m);
    }
    init(&m);m.stall=1;m.control|=0x80000000u;m.status=1u<<16;
    CHECK(ggcx_enable(&m.state)==GGCX_TIMEOUT && m.state.polls==3000);sealed(&m);
    struct ggcx_state empty={0};
    CHECK(ggcx_enable(NULL)==GGCX_ARGUMENT && ggcx_enable(&empty)==GGCX_USED);
    CHECK(ggcx_open(NULL,NULL)==GGCX_ARGUMENT && ggcx_open(&empty,NULL)==GGCX_ARGUMENT);
    CHECK(ggcx_describe(NULL)==GGCX_ARGUMENT);
    init(&m);CHECK(ggcx_open(&m.state,&m.state.io)==GGCX_USED);
    ggcx_invalidate(&m.state);sealed(&m);ggcx_invalidate(NULL);
    printf("GPU CX RAM-only assertions: %llu\n",checks);
    return 0;
}
