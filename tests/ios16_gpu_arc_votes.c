/* RAM-only production-code tests. No actual RPMh or hardware completion. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_arc_votes.h"
static unsigned checks;
#define CHECK(e) do { ++checks;assert(e); } while(0)
static const struct ggarc_table table={GGARC_ABI,1,0x30010,1,6,6,0,{48,64,128,192,256,384}};
static struct ggav_state fresh(unsigned min,unsigned max)
{
    struct ggav_state s={0};
    CHECK(ggav_init(&s,91,&table,min,max)==GGAV_OK);
    CHECK(!s.applied_valid && !s.pending.serial);
    return s;
}
static void apply(struct ggav_state *s,const struct ggav_transaction *t)
{ CHECK(ggav_finish(s,t->epoch,t->serial,GGAV_APPLIED,&t->plan)==GGAV_OK); }
static void basic(void)
{
    struct ggav_state s=fresh(64,384);struct ggav_transaction a,b,c;
    CHECK(ggav_add(&s,128,384,&a)==GGAV_OK && a.ticket && a.plan.selector==2);
    CHECK(!s.votes[0].ticket && !s.applied_valid); /* Pending != committed. */
    CHECK(ggav_add(&s,64,128,&b)==GGAV_BUSY && !b.serial);
    CHECK(ggav_finish(&s,90,a.serial,GGAV_APPLIED,&a.plan)==GGAV_STALE);
    CHECK(ggav_finish(&s,91,a.serial+1,GGAV_APPLIED,&a.plan)==GGAV_STALE);
    CHECK(s.pending.serial==a.serial);apply(&s,&a);
    CHECK(ggav_finish(&s,91,a.serial,GGAV_APPLIED,&a.plan)==GGAV_STALE);
    CHECK(ggav_add(&s,192,256,&b)==GGAV_OK && b.plan.logical_level==192);apply(&s,&b);
    CHECK(ggav_add(&s,64,128,&c)==GGAV_RANGE && !c.serial);
    CHECK(ggav_base(&s,256,384,&c)==GGAV_OK && c.plan.logical_level==256);apply(&s,&c);
    CHECK(ggav_remove(&s,b.ticket,&c)==GGAV_OK && c.plan.logical_level==256);apply(&s,&c);
    CHECK(ggav_remove(&s,b.ticket,&c)==GGAV_STALE && !c.serial);
    CHECK(ggav_base(&s,64,384,&c)==GGAV_OK && c.plan.logical_level==128);apply(&s,&c);
    CHECK(ggav_remove(&s,a.ticket,&c)==GGAV_OK && c.plan.logical_level==64);apply(&s,&c);
    CHECK(s.applied_valid && s.applied.selector==1); /* No clients != OFF. */
    struct ggav_status status;
    CHECK(ggav_status(&s,&status)==GGAV_OK && !status.count && !status.pending_serial);
    CHECK(ggav_init(&s,92,&table,64,384)==GGAV_USED);
}
static void failures(void)
{
    for(unsigned op=GGAV_ADD;op<=GGAV_BASE;op++)for(unsigned kind=0;kind<7;kind++) {
        struct ggav_state s=fresh(64,384);struct ggav_transaction a,t;
        CHECK(ggav_add(&s,128,256,&a)==GGAV_OK);apply(&s,&a);
        if(op==GGAV_ADD)CHECK(ggav_add(&s,192,256,&t)==GGAV_OK);
        else if(op==GGAV_REMOVE)CHECK(ggav_remove(&s,a.ticket,&t)==GGAV_OK);
        else CHECK(ggav_base(&s,192,384,&t)==GGAV_OK);
        const struct ggav_state before=s;
        struct ggarc_plan bad=t.plan;
        if(kind==0) {
            CHECK(ggav_finish(&s,91,t.serial,GGAV_UNCHANGED,0)==GGAV_OK);
            CHECK(!s.pending.serial && !memcmp(s.votes,before.votes,sizeof(s.votes)));
            CHECK(!memcmp(&s.applied,&before.applied,sizeof(s.applied)));
            CHECK(s.base_min==before.base_min && s.base_max==before.base_max);
        } else {
            unsigned outcome=GGAV_APPLIED;const struct ggarc_plan *observed=&bad;
            if(kind==1)bad.address+=4;
            if(kind==2)bad.selector++;
            if(kind==3)bad.logical_level++;
            if(kind==4)observed=0;
            if(kind==5){outcome=GGAV_UNKNOWN;observed=0;}
            if(kind==6)outcome=GGAV_UNCHANGED; /* Inconsistent completion. */
            CHECK(ggav_finish(&s,91,t.serial,outcome,observed)==GGAV_UNCERTAIN);
            CHECK(s.quarantined && s.pending.serial==t.serial);
            CHECK(!memcmp(s.votes,before.votes,sizeof(s.votes)));
            CHECK(ggav_finish(&s,91,t.serial,GGAV_UNCHANGED,0)==GGAV_QUARANTINED);
            CHECK(ggav_remove(&s,a.ticket,&t)==GGAV_QUARANTINED && !t.serial);
        }
    }
    struct ggav_state s=fresh(64,384);struct ggav_transaction t;
    uint64_t tickets[GGAV_SLOTS];
    for(unsigned i=0;i<GGAV_SLOTS;i++) {
        CHECK(ggav_add(&s,64,384,&t)==GGAV_OK);tickets[i]=t.ticket;apply(&s,&t);
    }
    CHECK(ggav_add(&s,64,384,&t)==GGAV_FULL && !t.serial);
    CHECK(ggav_remove(&s,tickets[7],&t)==GGAV_OK);apply(&s,&t);
    CHECK(ggav_add(&s,64,384,&t)==GGAV_OK && t.ticket!=tickets[7]);apply(&s,&t);
    CHECK(ggav_remove(&s,tickets[7],&t)==GGAV_STALE);
    s.serial=~(uint64_t)0;
    CHECK(ggav_remove(&s,tickets[0],&t)==GGAV_FULL);
    ggav_quarantine(&s);CHECK(ggav_base(&s,64,384,&t)==GGAV_QUARANTINED);
}
static void intervals(void)
{
    /* Exhaustive two-client intersection vs an independent simple oracle. */
    const unsigned levels[]={0,48,63,64,65,127,128,129,191,192,193,255,256,257,384,385};
    for(unsigned a=0;a<16;a++)for(unsigned b=a;b<16;b++)
    for(unsigned c=0;c<16;c++)for(unsigned d=c;d<16;d++) {
        struct ggav_state s=fresh(48,384);struct ggav_transaction t;
        unsigned first=16;
        for(unsigned i=0;i<table.count;i++)if(table.levels[i]>=levels[a] && table.levels[i]<=levels[b]){first=i;break;}
        int code=ggav_add(&s,levels[a],levels[b],&t);
        CHECK(code==(first==16?GGAV_RANGE:GGAV_OK));
        if(first==16)continue;
        apply(&s,&t);
        unsigned min=levels[a]>levels[c]?levels[a]:levels[c];
        unsigned max=levels[b]<levels[d]?levels[b]:levels[d];
        unsigned selected=16;
        for(unsigned i=0;i<table.count;i++)if(table.levels[i]>=min && table.levels[i]<=max){selected=i;break;}
        code=ggav_add(&s,levels[c],levels[d],&t);
        CHECK(code==(selected==16?GGAV_RANGE:GGAV_OK));
        if(code==GGAV_OK){CHECK(t.plan.selector==selected);apply(&s,&t);}
    }
}
static void arguments(void)
{
    struct ggav_state s={0};struct ggav_transaction t;
    CHECK(ggav_init(0,91,&table,0,384)==GGAV_ARGUMENT);
    CHECK(ggav_init(&s,0,&table,0,384)==GGAV_ARGUMENT);
    CHECK(ggav_init(&s,91,0,0,384)==GGAV_ARGUMENT);
    CHECK(ggav_init(&s,91,&table,385,384)==GGAV_ARGUMENT);
    CHECK(ggav_init(&s,91,&table,0,65536)==GGAV_ARGUMENT);
    CHECK(ggav_add(&s,0,384,&t)==GGAV_USED);
    CHECK(ggav_add(0,0,384,&t)==GGAV_ARGUMENT && !t.serial);
    CHECK(ggav_remove(&s,0,&t)==GGAV_ARGUMENT);
    CHECK(ggav_add(&s,0,384,0)==GGAV_ARGUMENT);
    CHECK(ggav_status(0,0)==GGAV_ARGUMENT);
    struct ggarc_table copy=table;
    CHECK(ggav_init(&s,91,&copy,64,384)==GGAV_OK);
    copy.address=0;copy.levels[2]=0;
    CHECK(ggav_add(&s,128,192,&t)==GGAV_OK && t.plan.logical_level==128);apply(&s,&t);
    CHECK(ggav_base(&s,0,63,&t)==GGAV_RANGE && !t.serial);
}
int main(void)
{
    CHECK(strstr(ggav_identity(),"CPU-only")!=0);
    arguments();basic();failures();intervals();
    printf("ARC votes: %u RAM-only assertions passed; no transport/ownership/hardware tested\n",checks);
    return 0;
}
