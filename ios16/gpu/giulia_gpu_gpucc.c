/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independently implemented bounded transactions from pinned public branch
 * register/policy descriptions. No raw address API or production activation.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gpucc.h"

static const struct ggcb_branch gpucc_branches[GGCB_CLOCKS]={
    {0x911c,GGCB_DELAY,0}, {0x913c,GGCB_VOTED,1},
    {0x9144,GGCB_HALT,0}, {0x7000,GGCB_VOTED,0},
    {0x93e8,GGCB_VOTED,1}, {0x9148,GGCB_VOTED,1}
};
__attribute__((used)) const char *ggcb_identity(void) { return GGCB_IDENTITY; }
__attribute__((used)) int ggcb_describe(unsigned id,struct ggcb_branch *out)
{
    if(!out)return GGCB_ARGUMENT;
    const struct ggcb_branch zero={0,0,0};*out=zero;
    if(id>=GGCB_CLOCKS)return GGCB_ARGUMENT;
    *out=gpucc_branches[id];return GGCB_OK;
}
static int cb_fail(struct ggcb_state *s,unsigned code)
{ s->quarantined=1;s->last_code=code;return (int)code; }
static int cb_live(struct ggcb_state *s)
{
    if(s->quarantined)return GGCB_QUARANTINED;
    if(s->io.live(s->io.cookie)!=1)return cb_fail(s,GGCB_LOST);
    return GGCB_OK;
}
static int cb_read(struct ggcb_state *s,uint32_t offset,uint32_t *value)
{
    int e=cb_live(s);if(e)return e;
    s->last_offset=offset;++s->reads;
    e=s->io.read32(s->io.cookie,offset,value);
    int live=cb_live(s);if(live)return live;
    if(e || *value==0xffffffffu)return cb_fail(s,GGCB_IO);
    s->observed=*value;return GGCB_OK;
}
static int cb_write(struct ggcb_state *s,uint32_t offset,uint32_t value)
{
    int e=cb_live(s);if(e)return e;
    s->phase=GGCB_PHASE_WRITE;s->last_offset=offset;
    s->touched_mask|=1u<<s->branch;++s->writes;
    e=s->io.write32(s->io.cookie,offset,value);
    int live=cb_live(s);if(live)return live;
    if(e)return cb_fail(s,GGCB_IO);
    return GGCB_OK;
}
static int cb_wait(struct ggcb_state *s,unsigned us)
{
    int e=cb_live(s);if(e)return e;
    ++s->waits;e=s->io.wait_us(s->io.cookie,us);
    int live=cb_live(s);if(live)return live;
    if(e)return cb_fail(s,GGCB_IO);
    return GGCB_OK;
}
__attribute__((used)) int ggcb_open(struct ggcb_state *s,const struct ggcb_io *io)
{
    if(!s || !io || !io->live || !io->ready || !io->quiescent ||
       !io->read32 || !io->write32 || !io->wait_us)return GGCB_ARGUMENT;
    if(s->opened || s->quarantined)return GGCB_USED;
    s->opened=1;s->io=*io;
    return cb_live(s);
}
__attribute__((used)) void ggcb_invalidate(struct ggcb_state *s)
{ if(s)(void)cb_fail(s,GGCB_LOST); }

__attribute__((used)) int ggcb_apply(struct ggcb_state *s,unsigned id,unsigned enabling)
{
    if(!s || id>=GGCB_CLOCKS || enabling>1)return GGCB_ARGUMENT;
    if(s->quarantined)return GGCB_QUARANTINED;
    if(!s->opened)return GGCB_USED;
    const struct ggcb_branch *b=&gpucc_branches[id];
    const uint32_t bit=1u<<id;
    s->branch=id;s->phase=GGCB_PHASE_READY;
    int e=cb_live(s);if(e)return e;
    if(!enabling) {
        if(b->aon)return s->last_code=GGCB_PINNED;
        if(s->inherited_mask&bit)return s->last_code=GGCB_INHERITED;
        if(!(s->changed_mask&bit))return s->last_code=GGCB_NOT_OWNED;
    }
    int ready=s->io.ready(s->io.cookie,id);
    e=cb_live(s);if(e)return e;
    if(ready==0)return s->last_code=GGCB_UNAVAILABLE;
    if(ready!=1)return cb_fail(s,GGCB_LOST);
    if(!enabling) {
        s->phase=GGCB_PHASE_QUIET;
        int quiet=s->io.quiescent(s->io.cookie,id);
        e=cb_live(s);if(e)return e;
        if(quiet==0)return s->last_code=GGCB_BUSY;
        if(quiet!=1)return cb_fail(s,GGCB_LOST);
    }
    s->phase=GGCB_PHASE_READ;
    uint32_t before=0;e=cb_read(s,b->offset,&before);if(e)return e;
    s->before=before;
    /* A latched vote disappearing between calls contradicts the source's
     * durable exclusion/backing contract. DELAY AHB is not a latched vote. */
    if(b->policy!=GGCB_DELAY && ((s->requested_mask|s->inherited_mask)&bit) && !(before&1u))
        return cb_fail(s,GGCB_LOST);
    if(enabling && (before&1u) && !(s->requested_mask&bit))s->inherited_mask|=bit;
    const uint32_t desired=(before&~1u)|enabling;
    if(desired!=before) {
        e=cb_write(s,b->offset,desired);if(e)return e;
    }
    if(b->policy==GGCB_DELAY || (!enabling && b->policy==GGCB_VOTED)) {
        s->phase=GGCB_PHASE_DELAY;
        e=cb_wait(s,10);if(e)return e;
        uint32_t after=0;e=cb_read(s,b->offset,&after);if(e)return e;
        /* VOTED disable may leave CLK_OFF clear because of other voters.
         * AHB uses DELAY and may read zero even after a successful enable. */
        if(b->policy!=GGCB_DELAY && (after&1u)!=enabling)return cb_fail(s,GGCB_READBACK);
    } else {
        s->phase=GGCB_PHASE_POLL;
        unsigned completed=0;
        for(unsigned poll=0;poll<GGCB_POLLS;poll++) {
            uint32_t after=0;++s->polls;
            e=cb_read(s,b->offset,&after);if(e)return e;
            if((after&1u)!=enabling)return cb_fail(s,GGCB_READBACK);
            const uint32_t status=after&0xf0000000u;
            unsigned ready_bit=enabling?(!(status&0x80000000u) || status==0x20000000u):!!(status&0x80000000u);
            if(ready_bit){completed=1;break;}
            e=cb_wait(s,1);if(e)return e;
        }
        if(!completed)return cb_fail(s,GGCB_TIMEOUT);
    }
    if(enabling) {
        s->requested_mask|=bit;
        if(!(s->inherited_mask&bit))s->changed_mask|=bit;
        if(b->aon)s->pinned_mask|=bit;
    } else {
        s->requested_mask&=~bit;s->changed_mask&=~bit;
    }
    s->phase=GGCB_PHASE_DONE;return s->last_code=GGCB_OK;
}
#endif
