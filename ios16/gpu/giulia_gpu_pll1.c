/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent fixed PLL1 executor using pinned public register facts.
 * No Linux implementation copied, no mapping/provider or boot activation.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_pll1.h"
#define P_MODE 0x1000u
#define P_OP 0x1004u
#define P_L 0x1010u
#define P_ALPHA 0x1014u
#define P_USER 0x1018u
#define P_OUT 1u
#define P_RESET 4u
#define P_PCAL (1u<<8)
#define P_ACK (1u<<13)
#define P_LATCH (1u<<14)
#define P_FSM (1u<<20)
#define P_LOCK (1u<<31)

__attribute__((used)) const char *ggp1_identity(void) { return GGP1_IDENTITY; }
__attribute__((used)) int ggp1_describe(uint32_t hz,struct ggp1_plan *p)
{
    if(!p)return GGP1_ARGUMENT;
    const struct ggp1_plan zero={0,0,0,0,0,0,0};*p=zero;
    if(hz!=520000000u && hz!=1250000000u)return GGP1_ARGUMENT;
    p->requested_hz=hz;p->xo_hz=19200000u;p->l=hz/p->xo_hz;
    p->alpha=(uint32_t)(((uint64_t)(hz%p->xo_hz)<<16)/p->xo_hz);
    p->floor_hz=p->xo_hz*p->l+(uint32_t)(((uint64_t)p->xo_hz*p->alpha)>>16);
    p->reported_hz=((p->floor_hz+999u)/1000u)*1000u;
    p->shortfall_hz=hz-p->floor_hz;
    return GGP1_OK;
}
static int p_fail(struct ggp1_state *s,unsigned code)
{ s->quarantined=1;s->last_code=code;return (int)code; }
static int p_live(struct ggp1_state *s)
{
    if(s->quarantined)return GGP1_QUARANTINED;
    if(s->io.live(s->io.cookie)!=1)return p_fail(s,GGP1_LOST);
    return GGP1_OK;
}
static int p_read(struct ggp1_state *s,uint32_t offset,uint32_t *v)
{
    int e=p_live(s);if(e)return e;
    s->last_offset=offset;++s->reads;
    int io=s->io.read32(s->io.cookie,offset,v);
    e=p_live(s);if(e)return e;
    if(io || *v==0xffffffffu)return p_fail(s,GGP1_IO);
    s->observed=*v;return GGP1_OK;
}
static int p_write(struct ggp1_state *s,uint32_t offset,uint32_t v)
{
    int e=p_live(s);if(e)return e;
    s->last_offset=offset;s->touched=1;++s->writes;
    int io=s->io.write32(s->io.cookie,offset,v);
    e=p_live(s);if(e)return e;
    if(io)return p_fail(s,GGP1_IO);
    return GGP1_OK;
}
static int p_wait(struct ggp1_state *s)
{
    int e=p_live(s);if(e)return e;
    ++s->waits;int io=s->io.wait_us(s->io.cookie,1);
    e=p_live(s);if(e)return e;
    if(io)return p_fail(s,GGP1_IO);
    return GGP1_OK;
}
static int p_mode(struct ggp1_state *s,uint32_t *v)
{
    int e=p_read(s,P_MODE,v);if(e)return e;
    if(!(*v&P_RESET) || (*v&(P_PCAL|P_FSM)))return p_fail(s,GGP1_LOST);
    return GGP1_OK;
}
static int p_rate_check(struct ggp1_state *s,const struct ggp1_plan *p)
{
    uint32_t l=0,a=0;
    int e=p_read(s,P_L,&l);if(e)return e;
    e=p_read(s,P_ALPHA,&a);if(e)return e;
    if(l!=(0x44440000u|p->l) || a!=p->alpha)return p_fail(s,GGP1_READBACK);
    return GGP1_OK;
}
__attribute__((used)) int ggp1_open(struct ggp1_state *s,const struct ggp1_io *io)
{
    if(!s || !io || !io->live || !io->ready || !io->parked ||
       !io->read32 || !io->write32 || !io->wait_us)return GGP1_ARGUMENT;
    if(s->opened || s->quarantined)return GGP1_USED;
    s->io=*io;s->opened=1;return p_live(s);
}
__attribute__((used)) void ggp1_invalidate(struct ggp1_state *s)
{ if(s)(void)p_fail(s,GGP1_LOST); }
__attribute__((used)) int ggp1_start(struct ggp1_state *s,uint32_t hz)
{
    struct ggp1_plan plan;
    if(!s || ggp1_describe(hz,&plan)!=GGP1_OK)return GGP1_ARGUMENT;
    if(s->quarantined)return GGP1_QUARANTINED;
    if(!s->opened || s->started)return GGP1_USED;
    s->phase=GGP1_READY;
    int e=p_live(s);if(e)return e;
    int ready=s->io.ready(s->io.cookie,&plan);
    e=p_live(s);if(e)return e;
    if(!ready)return s->last_code=GGP1_UNAVAILABLE;
    if(ready!=1)return p_fail(s,GGP1_LOST);
    int parked=s->io.parked(s->io.cookie);
    e=p_live(s);if(e)return e;
    if(!parked)return s->last_code=GGP1_BUSY;
    if(parked!=1)return p_fail(s,GGP1_LOST);
    s->phase=GGP1_INSPECT;
    uint32_t mode=0,op=0,l=0,user=0;
    e=p_read(s,P_MODE,&mode);if(e)return e;
    e=p_read(s,P_OP,&op);if(e)return e;
    e=p_read(s,P_USER,&user);if(e)return e;
    if(op || (mode&(P_OUT|P_LATCH|P_ACK|P_FSM)) || (user&(1u<<25)))
        return s->last_code=GGP1_BUSY;
    if(!(mode&P_RESET) || (mode&P_PCAL))return s->last_code=GGP1_UNAVAILABLE;
    /* Refuse cold/uninitialized or differently configured PLLs; do not
     * silently configure analog controls or trigger first calibration. */
    e=p_read(s,P_L,&l);if(e)return e;
    if((l&0xffff0000u)!=0x44440000u || !(l&0xffffu) || (user&7u)!=7u)
        return s->last_code=GGP1_UNAVAILABLE;
    static const uint32_t config[][2]={
        {0x101cu,5u},{0x1020u,0x20485699u},{0x1024u,0x00182261u},
        {0x1028u,0x82aa299cu},{0x102cu,0u},{0x1030u,3u},
        {0x1034u,0x00009000u},{0x1038u,0x34u}
    };
    for(unsigned n=0;n<sizeof(config)/sizeof(config[0]);n++) {
        uint32_t v=0;e=p_read(s,config[n][0],&v);if(e)return e;
        if(v!=config[n][1])return s->last_code=GGP1_UNAVAILABLE;
    }
    s->phase=GGP1_RATE;
    e=p_write(s,P_L,(l&0xffff0000u)|plan.l);if(e)return e;
    e=p_write(s,P_ALPHA,plan.alpha);if(e)return e;
    e=p_rate_check(s,&plan);if(e)return e;
    s->phase=GGP1_LATCH;
    e=p_mode(s,&mode);if(e)return e;
    if(mode&(P_OUT|P_LATCH|P_ACK))return p_fail(s,GGP1_LOST);
    e=p_write(s,P_MODE,mode|P_LATCH);if(e)return e;
    /* At least two 19.2 MHz reference cycles before ACK inspection. */
    e=p_wait(s);if(e)return e;
    e=p_mode(s,&mode);if(e)return e;
    if((mode&(P_LATCH|P_ACK|P_OUT))!=(P_LATCH|P_ACK))return p_fail(s,GGP1_READBACK);
    e=p_write(s,P_MODE,mode&~P_LATCH);if(e)return e;
    /* Stricter than the reference driver: bound ACK deassertion as well.
     * This conservative gate still needs explicit real-hardware validation. */
    for(unsigned n=0;;n++) {
        ++s->ack_polls;e=p_mode(s,&mode);if(e)return e;
        if(mode&(P_LATCH|P_OUT))return p_fail(s,GGP1_READBACK);
        if(!(mode&P_ACK))break;
        e=p_wait(s);if(e)return e;
        if(n+1==GGP1_POLLS)return p_fail(s,GGP1_TIMEOUT);
    }
    s->phase=GGP1_RUN;
    e=p_write(s,P_OP,1u);if(e)return e;
    e=p_read(s,P_OP,&op);if(e)return e;
    if(op!=1u)return p_fail(s,GGP1_READBACK);
    s->phase=GGP1_LOCK;
    for(unsigned n=0;;n++) {
        ++s->lock_polls;e=p_mode(s,&mode);if(e)return e;
        if(mode&(P_LATCH|P_ACK|P_OUT))return p_fail(s,GGP1_READBACK);
        if(mode&P_LOCK)break;
        e=p_wait(s);if(e)return e;
        if(n+1==GGP1_POLLS)return p_fail(s,GGP1_TIMEOUT);
    }
    e=p_rate_check(s,&plan);if(e)return e;
    s->phase=GGP1_OUTPUT;
    e=p_write(s,P_MODE,mode|P_OUT);if(e)return e;
    e=p_mode(s,&mode);if(e)return e;
    if((mode&(P_LOCK|P_OUT|P_ACK|P_LATCH))!=(P_LOCK|P_OUT))return p_fail(s,GGP1_READBACK);
    e=p_rate_check(s,&plan);if(e)return e;
    e=p_read(s,P_OP,&op);if(e)return e;
    e=p_read(s,P_USER,&user);if(e)return e;
    if(op!=1u || (user&((1u<<25)|7u))!=7u)return p_fail(s,GGP1_LOST);
    s->applied=plan;s->started=1;s->phase=GGP1_DONE;s->last_code=GGP1_OK;
    return GGP1_OK;
}
#endif
