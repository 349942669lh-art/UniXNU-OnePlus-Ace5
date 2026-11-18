/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * RAM-only register fixture, never used as a production authority/source.
 */
#include "giulia_gpu_pll1.h"
#include "pll1-source-policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned long long checks;
#define CHECK(x) do { ++checks; if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();} } while(0)
struct mock {
    struct ggp1_state s;
    uint32_t reg[15];
    unsigned events,reads,writes,waits,lives,fail_at,lose_at;
    unsigned no_ack,sticky_ack,no_lock,ignore_l,ignore_alpha,ignore_run,ignore_out;
    unsigned run_ticks,all_ones,drop_reset,drift_alpha,alpha_reads;
    int ready,parked;
};
static unsigned index_at(uint32_t off)
{
    CHECK(off>=source_base && off<=source_base+0x38 && !(off&3));
    unsigned index=(off-source_base)/4;
    CHECK(index!=2 && index!=3); /* STATE and STATUS never touched */
    return index;
}
static int live(void *p)
{ struct mock *m=p;return ++m->lives==m->lose_at?0:1; }
static int ready(void *p,const struct ggp1_plan *plan)
{ struct mock *m=p;CHECK(plan->xo_hz==19200000 && plan->shortfall_hz<293);return m->ready; }
static int parked(void *p) { return ((struct mock *)p)->parked; }
static int event(struct mock *m) { return ++m->events==m->fail_at; }
static int read32(void *p,uint32_t off,uint32_t *v)
{
    struct mock *m=p;unsigned i=index_at(off);++m->reads;
    if(event(m))return -1;
    if(!i && m->reg[1]==1 && !m->no_lock && ++m->run_ticks>=2)m->reg[0]|=source_lock;
    if(i==5 && m->drift_alpha && ++m->alpha_reads==3)m->reg[5]^=1;
    *v=m->all_ones?0xffffffffu:m->reg[i];return 0;
}
static int write32(void *p,uint32_t off,uint32_t value)
{
    struct mock *m=p;unsigned i=index_at(off);++m->writes;
    CHECK(i==0 || i==1 || i==4 || i==5);
    if(i==0) {
        CHECK(((m->reg[0]^value)&~(source_out|source_latch))==0);
        uint32_t v=(m->reg[0]&~(source_out|source_latch))|(value&(source_out|source_latch));
        if(m->ignore_out)v&=~source_out;
        if((v&source_latch) && !m->no_ack)v|=source_ack;
        if(!(v&source_latch) && !m->sticky_ack)v&=~source_ack;
        if(m->drop_reset)v&=~source_reset;
        m->reg[0]=v;
    } else if(i==1) {
        CHECK(value==1);
        if(!m->ignore_run){m->reg[1]=value;m->run_ticks=0;}
    } else if(i==4) {
        CHECK(((m->reg[4]^value)&0xffff0000u)==0);
        if(!m->ignore_l)m->reg[4]=value;
    } else if(!m->ignore_alpha)m->reg[5]=value;
    /* Deliberately can fail AFTER mutation. No rollback may follow. */
    return event(m)?-1:0;
}
static int wait_us(void *p,unsigned us)
{ struct mock *m=p;CHECK(us==1);++m->waits;return event(m)?-1:0; }
static void init(struct mock *m)
{
    memset(m,0,sizeof(*m));m->ready=m->parked=1;
    for(unsigned n=0;n<sizeof(source_init)/sizeof(source_init[0]);n++)
        m->reg[index_at(source_init[n][0])]=source_init[n][1];
    m->reg[0]=source_reset|(1u<<1); /* unrelated MODE bit retained */
    const struct ggp1_io io={m,live,ready,parked,read32,write32,wait_us};
    CHECK(ggp1_open(&m->s,&io)==GGP1_OK && !m->events);
}
static void sealed(struct mock *m)
{
    unsigned ev=m->events;CHECK(m->s.quarantined && !m->s.started);
    CHECK(ggp1_start(&m->s,520000000)==GGP1_QUARANTINED);
    CHECK(ggp1_start(&m->s,1250000000)==GGP1_QUARANTINED && m->events==ev);
}
static void denied(struct mock *m,int code)
{
    CHECK(ggp1_start(&m->s,520000000)==code);
    CHECK(!m->writes && !m->s.touched && !m->s.quarantined && !m->s.started);
}
int main(void)
{
    struct mock m;struct ggp1_plan p;
    CHECK(!strcmp(ggp1_identity(),GGP1_IDENTITY));
    for(unsigned i=0;i<sizeof(source_plans)/sizeof(source_plans[0]);i++) {
        const struct ggp1_plan *oracle=&source_plans[i];
        CHECK(ggp1_describe(oracle->requested_hz,&p)==GGP1_OK);
        CHECK(!memcmp(&p,oracle,sizeof(p)));
        init(&m);CHECK(ggp1_start(&m.s,p.requested_hz)==GGP1_OK);
        CHECK(m.s.phase==GGP1_DONE && m.s.started && m.s.touched && !m.s.quarantined);
        CHECK(!memcmp(&m.s.applied,&p,sizeof(p)) && m.writes==6);
        CHECK(m.reg[0]==(source_reset|source_lock|source_out|(1u<<1)));
        CHECK(m.reg[1]==1 && m.reg[4]==(0x44440000u|p.l) && m.reg[5]==p.alpha);
        unsigned events=m.events,lives=m.lives;
        CHECK(ggp1_start(&m.s,p.requested_hz)==GGP1_USED && m.events==events);
        CHECK(ggp1_open(&m.s,&m.s.io)==GGP1_USED);
        /* Every successful-path callback boundary, including after writes. */
        for(unsigned at=1;at<=events;at++) {
            init(&m);m.fail_at=at;
            CHECK(ggp1_start(&m.s,p.requested_hz)==GGP1_IO);
            CHECK(m.events==at);sealed(&m);
        }
        for(unsigned at=2;at<=lives;at++) {
            init(&m);m.lose_at=at;
            CHECK(ggp1_start(&m.s,p.requested_hz)==GGP1_LOST);sealed(&m);
        }
    }
    init(&m);m.ready=0;denied(&m,GGP1_UNAVAILABLE);CHECK(!m.events);
    m.ready=-1;CHECK(ggp1_start(&m.s,520000000)==GGP1_LOST);sealed(&m);
    init(&m);m.parked=0;denied(&m,GGP1_BUSY);CHECK(!m.events);
    m.parked=2;CHECK(ggp1_start(&m.s,520000000)==GGP1_LOST);sealed(&m);
    const uint32_t busy_bits[]={source_out,source_latch,source_ack,1u<<20};
    for(unsigned i=0;i<sizeof(busy_bits)/sizeof(busy_bits[0]);i++) {
        init(&m);m.reg[0]|=busy_bits[i];denied(&m,GGP1_BUSY);
    }
    init(&m);m.reg[1]=1;denied(&m,GGP1_BUSY);
    init(&m);m.reg[6]|=1u<<25;denied(&m,GGP1_BUSY);
    init(&m);m.reg[0]&=~source_reset;denied(&m,GGP1_UNAVAILABLE);
    init(&m);m.reg[0]|=1u<<8;denied(&m,GGP1_UNAVAILABLE);
    init(&m);m.reg[4]=0;denied(&m,GGP1_UNAVAILABLE);
    init(&m);m.reg[4]&=0xffff0000u;denied(&m,GGP1_UNAVAILABLE);
    init(&m);m.reg[6]&=~1u;denied(&m,GGP1_UNAVAILABLE);
    for(unsigned n=0;n<sizeof(source_init)/sizeof(source_init[0]);n++) {
        unsigned i=index_at(source_init[n][0]);
        if(i<7)continue;
        init(&m);m.reg[i]^=1;denied(&m,GGP1_UNAVAILABLE);
    }
    init(&m);m.no_ack=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_READBACK);
    CHECK(m.reg[0]&source_latch);sealed(&m);
    init(&m);m.sticky_ack=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_TIMEOUT);
    CHECK(m.s.ack_polls==GGP1_POLLS && m.reg[1]==0 && m.writes==4);sealed(&m);
    init(&m);m.no_lock=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_TIMEOUT);
    CHECK(m.s.lock_polls==GGP1_POLLS && !(m.reg[0]&source_out) && m.writes==5);sealed(&m);
    init(&m);m.ignore_l=1;CHECK(ggp1_start(&m.s,1250000000)==GGP1_READBACK);sealed(&m);
    init(&m);m.ignore_alpha=1;CHECK(ggp1_start(&m.s,1250000000)==GGP1_READBACK);sealed(&m);
    init(&m);m.ignore_run=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_READBACK);sealed(&m);
    init(&m);m.ignore_out=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_READBACK);sealed(&m);
    init(&m);m.drop_reset=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_LOST);sealed(&m);
    init(&m);m.drift_alpha=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_READBACK);sealed(&m);
    init(&m);m.all_ones=1;CHECK(ggp1_start(&m.s,520000000)==GGP1_IO && !m.writes);sealed(&m);
    init(&m);ggp1_invalidate(&m.s);sealed(&m);ggp1_invalidate(NULL);
    memset(&m,0,sizeof(m));CHECK(ggp1_start(&m.s,520000000)==GGP1_USED);
    CHECK(ggp1_open(NULL,NULL)==GGP1_ARGUMENT && ggp1_open(&m.s,NULL)==GGP1_ARGUMENT);
    struct ggp1_io io={0};CHECK(ggp1_open(&m.s,&io)==GGP1_ARGUMENT);
    CHECK(ggp1_start(NULL,520000000)==GGP1_ARGUMENT);
    CHECK(ggp1_describe(520000000,NULL)==GGP1_ARGUMENT);
    const uint32_t invalid[]={0,1,19200000,260000000,519999999,520000001,625000000,1249999999,1250000001,0xffffffffu};
    init(&m);
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        memset(&p,0xaa,sizeof(p));CHECK(ggp1_describe(invalid[i],&p)==GGP1_ARGUMENT);
        const struct ggp1_plan zero={0};CHECK(!memcmp(&p,&zero,sizeof(p)));
        CHECK(ggp1_start(&m.s,invalid[i])==GGP1_ARGUMENT && !m.events);
    }
    printf("GPU PLL1 RAM-only assertions: %llu\n",checks);return 0;
}
