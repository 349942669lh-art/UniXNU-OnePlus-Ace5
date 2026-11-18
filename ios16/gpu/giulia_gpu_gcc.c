/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independently implemented transactions from pinned GCC/branch policy data.
 * This module has no authority source, IOKit registration or terminal command.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gcc.h"
static const struct gggc_branch gcc_branches[GGGC_CLOCKS]={
    {0x4,GGGC_DIRECT,0}, {0x154,GGGC_SKIP,2},
    {0x10,GGGC_VOTED,2}, {0x18,GGGC_DELAY,0}
};
__attribute__((used)) const char *gggc_identity(void) { return GGGC_IDENTITY; }
__attribute__((used)) int gggc_describe(unsigned id,struct gggc_branch *out)
{
    if(!out)return GGGC_ARGUMENT;
    const struct gggc_branch zero={0,0,0};*out=zero;
    if(id>=GGGC_CLOCKS)return GGGC_ARGUMENT;
    *out=gcc_branches[id];return GGGC_OK;
}
static int gc_fail(struct gggc_state *s,unsigned code)
{
    if(!s->quarantined)s->last_code=code;
    s->quarantined=1;return (int)code;
}
static int gc_live(struct gggc_state *s)
{
    if(s->quarantined)return GGGC_QUARANTINED;
    return s->io.live(s->io.cookie)==1?GGGC_OK:gc_fail(s,GGGC_LOST);
}
static int gc_read(struct gggc_state *s,uint32_t off,uint32_t *value)
{
    int e=gc_live(s);if(e)return e;
    s->last_offset=off;++s->reads;e=s->io.read32(s->io.cookie,off,value);
    int live=gc_live(s);if(live)return live;
    if(e || *value==0xffffffffu)return gc_fail(s,GGGC_IO);
    s->observed=*value;return GGGC_OK;
}
static int gc_write(struct gggc_state *s,uint32_t off,uint32_t value)
{
    int e=gc_live(s);if(e)return e;
    s->touched_mask|=1u<<s->branch;s->last_offset=off;++s->writes;
    e=s->io.write32(s->io.cookie,off,value);
    int live=gc_live(s);if(live)return live;
    return e?gc_fail(s,GGGC_IO):GGGC_OK;
}
static int gc_wait(struct gggc_state *s,unsigned us)
{
    int e=gc_live(s);if(e)return e;
    ++s->waits;e=s->io.wait_us(s->io.cookie,us);
    int live=gc_live(s);if(live)return live;
    return e?gc_fail(s,GGGC_IO):GGGC_OK;
}
__attribute__((used)) int gggc_open(struct gggc_state *s,const struct gggc_io *io)
{
    if(!s || !io || !io->live || !io->ready || !io->read32 || !io->write32 || !io->wait_us)
        return GGGC_ARGUMENT;
    if(s->opened || s->quarantined)return GGGC_USED;
    s->opened=1;s->io=*io;return gc_live(s);
}
__attribute__((used)) void gggc_invalidate(struct gggc_state *s)
{ if(s)(void)gc_fail(s,GGGC_LOST); }
__attribute__((used)) int gggc_enable(struct gggc_state *s,unsigned id)
{
    if(!s || id>=GGGC_CLOCKS)return GGGC_ARGUMENT;
    if(s->quarantined)return GGGC_QUARANTINED;
    if(!s->opened)return GGGC_USED;
    s->branch=id;
    int e=gc_live(s);if(e)return e;
    int ready=s->io.ready(s->io.cookie,id);
    e=gc_live(s);if(e)return e;
    if(!ready)return s->last_code=GGGC_UNAVAILABLE;
    if(ready!=1)return gc_fail(s,GGGC_LOST);
    const struct gggc_branch *b=&gcc_branches[id];const uint32_t bit=1u<<id;
    uint32_t value=0;e=gc_read(s,b->offset,&value);if(e)return e;
    s->before=value;
    if(s->requested_mask&bit) {
        if(b->policy!=GGGC_SKIP && !(value&1u))return gc_fail(s,GGGC_LOST);
        /* A prior accepted AXI SKIP write has no required latched readback. */
        if(b->policy==GGGC_SKIP)return s->last_code=GGGC_OK;
    } else if(value&1u)s->inherited_mask|=bit;
    if(!(value&1u)) {
        e=gc_write(s,b->offset,value|1u);if(e)return e;
    }
    if(b->policy==GGGC_DELAY) { e=gc_wait(s,10);if(e)return e; }
    e=gc_read(s,b->offset,&value);if(e)return e;
    if(b->policy!=GGGC_SKIP && !(value&1u))return gc_fail(s,GGGC_READBACK);
    if(b->policy==GGGC_VOTED && !(value&b->hwcg_mask)) {
        for(unsigned poll=0;;++poll) {
            ++s->polls;
            uint32_t status=value&0xf0000000u;
            if(!(status&0x80000000u) || status==0x20000000u)break;
            if(poll+1==GGGC_POLLS)return gc_fail(s,GGGC_TIMEOUT);
            e=gc_wait(s,1);if(e)return e;
            e=gc_read(s,b->offset,&value);if(e)return e;
            if(!(value&1u))return gc_fail(s,GGGC_READBACK);
        }
    }
    s->requested_mask|=bit;return s->last_code=GGGC_OK;
}
#endif
