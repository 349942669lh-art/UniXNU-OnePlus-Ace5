/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independent fixed-root executor from pinned public register descriptions.
 * No platform mapping, PLL programming, resource grant or boot activation.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_rcg.h"
#define RC_UPDATE 1u
#define RC_FORCE 2u
#define RC_DIRTY 0xf0u
#define RC_OFF 0x80000000u
#define RC_MASK 0x0010371fu

__attribute__((used)) const char *ggrc_identity(void) { return GGRCLK_IDENTITY; }
__attribute__((used)) int ggrc_describe(unsigned root,uint32_t hz,struct ggrc_plan *p)
{
    if(!p)return GGRCLK_ARGUMENT;
    const struct ggrc_plan zero={0,0,0,0,0,0,0};*p=zero;
    if(root>=GGRCLK_ROOTS)return GGRCLK_ARGUMENT;
    uint32_t parent=GGRCLK_XO,parent_hz=19200000u,mux=0,hid=1;
    if(hz!=19200000u) {
        if(root==GGRCLK_GMU && (hz==260000000u || hz==625000000u)) {
            parent=GGRCLK_PLL1;parent_hz=hz*2u;mux=3;hid=3;
        } else if((root==GGRCLK_HUB && (hz==200000000u || hz==300000000u)) ||
                  (root==GGRCLK_FF && hz==200000000u)) {
            parent=GGRCLK_GPLL0;parent_hz=600000000u;mux=5;
            hid=hz==200000000u?5u:3u;
        } else return GGRCLK_ARGUMENT;
    }
    p->root=root;p->hz=hz;
    p->cmd=root==GGRCLK_GMU?0x9318u:(root==GGRCLK_HUB?0x93ecu:0x9474u);
    p->cfg_mask=RC_MASK;p->cfg_value=0x100000u|(mux<<8)|hid;
    p->parent=parent;p->parent_hz=parent_hz;return GGRCLK_OK;
}
static int rc_fail(struct ggrc_state *s,unsigned code)
{ s->quarantined=1;s->last_code=code;return (int)code; }
static int rc_live(struct ggrc_state *s)
{
    if(s->quarantined)return GGRCLK_QUARANTINED;
    if(s->io.live(s->io.cookie)!=1)return rc_fail(s,GGRCLK_LOST);
    return GGRCLK_OK;
}
static int rc_read(struct ggrc_state *s,uint32_t offset,uint32_t *v)
{
    int e=rc_live(s);if(e)return e;
    s->last_offset=offset;++s->reads;
    int io=s->io.read32(s->io.cookie,offset,v);
    e=rc_live(s);if(e)return e;
    if(io || *v==0xffffffffu)return rc_fail(s,GGRCLK_IO);
    s->observed=*v;return GGRCLK_OK;
}
static int rc_write(struct ggrc_state *s,uint32_t offset,uint32_t v)
{
    int e=rc_live(s);if(e)return e;
    s->last_offset=offset;s->touched_mask|=1u<<s->root;++s->writes;
    int io=s->io.write32(s->io.cookie,offset,v);
    e=rc_live(s);if(e)return e;
    if(io)return rc_fail(s,GGRCLK_IO);
    return GGRCLK_OK;
}
static int rc_wait(struct ggrc_state *s)
{
    int e=rc_live(s);if(e)return e;
    ++s->waits;int io=s->io.wait_us(s->io.cookie,1);
    e=rc_live(s);if(e)return e;
    if(io)return rc_fail(s,GGRCLK_IO);
    return GGRCLK_OK;
}
static int rc_poll(struct ggrc_state *s,uint32_t cmd,unsigned updating)
{
    for(unsigned n=0;n<GGRCLK_POLLS;n++) {
        uint32_t v=0;
        if(updating)++s->update_polls;else ++s->root_polls;
        int e=rc_read(s,cmd,&v);if(e)return e;
        if(!(v&RC_FORCE))return rc_fail(s,GGRCLK_READBACK);
        if(updating) { if(!(v&RC_UPDATE))return GGRCLK_OK; }
        else {
            if(v&RC_UPDATE)return rc_fail(s,GGRCLK_LOST);
            if(!(v&RC_OFF))return GGRCLK_OK;
        }
        e=rc_wait(s);if(e)return e;
    }
    return rc_fail(s,GGRCLK_TIMEOUT);
}
__attribute__((used)) int ggrc_open(struct ggrc_state *s,const struct ggrc_io *io)
{
    if(!s || !io || !io->live || !io->ready || !io->quiescent ||
       !io->read32 || !io->write32 || !io->wait_us)return GGRCLK_ARGUMENT;
    if(s->opened || s->quarantined)return GGRCLK_USED;
    s->io=*io;s->opened=1;return rc_live(s);
}
__attribute__((used)) void ggrc_invalidate(struct ggrc_state *s)
{ if(s)(void)rc_fail(s,GGRCLK_LOST); }
__attribute__((used)) int ggrc_apply(struct ggrc_state *s,unsigned root,uint32_t hz)
{
    struct ggrc_plan p;
    if(!s || ggrc_describe(root,hz,&p)!=GGRCLK_OK)return GGRCLK_ARGUMENT;
    if(s->quarantined)return GGRCLK_QUARANTINED;
    if(!s->opened)return GGRCLK_USED;
    s->root=root;s->phase=GGRCLK_READY;
    int e=rc_live(s);if(e)return e;
    int ready=s->io.ready(s->io.cookie,&p);
    e=rc_live(s);if(e)return e;
    if(!ready)return s->last_code=GGRCLK_UNAVAILABLE;
    if(ready!=1)return rc_fail(s,GGRCLK_LOST);
    s->phase=GGRCLK_QUIET;
    int quiet=s->io.quiescent(s->io.cookie,root);
    e=rc_live(s);if(e)return e;
    if(!quiet)return s->last_code=GGRCLK_BUSY;
    if(quiet!=1)return rc_fail(s,GGRCLK_LOST);
    uint32_t cmd=0,cfg=0;
    e=rc_read(s,p.cmd,&cmd);if(e)return e;
    /* Do not adopt another actor's unfinished shadow/update transaction. */
    if(cmd&(RC_UPDATE|RC_DIRTY))return s->last_code=GGRCLK_BUSY;
    const unsigned inherited=!!(cmd&RC_FORCE);
    s->phase=GGRCLK_FORCE;
    if(!inherited) {
        s->force_added_mask|=1u<<root; /* retain even on uncertain write */
        e=rc_write(s,p.cmd,cmd|RC_FORCE);if(e)return e;
    }
    e=rc_poll(s,p.cmd,0);if(e)return e;
    s->phase=GGRCLK_CONFIG;
    e=rc_read(s,p.cmd+4u,&cfg);if(e)return e;
    const uint32_t target=(cfg&~p.cfg_mask)|p.cfg_value;
    e=rc_write(s,p.cmd+4u,target);if(e)return e;
    uint32_t after=0;e=rc_read(s,p.cmd+4u,&after);if(e)return e;
    if((after&p.cfg_mask)!=p.cfg_value)return rc_fail(s,GGRCLK_READBACK);
    s->phase=GGRCLK_UPDATE;
    e=rc_read(s,p.cmd,&cmd);if(e)return e;
    if(!(cmd&RC_FORCE) || (cmd&RC_UPDATE))return rc_fail(s,GGRCLK_LOST);
    e=rc_write(s,p.cmd,cmd|RC_UPDATE);if(e)return e;
    /* UPDATE is self-clearing: equality with the written CMD is NOT required. */
    e=rc_poll(s,p.cmd,1);if(e)return e;
    e=rc_read(s,p.cmd+4u,&after);if(e)return e;
    if((after&p.cfg_mask)!=p.cfg_value)return rc_fail(s,GGRCLK_READBACK);
    s->phase=GGRCLK_RESTORE;
    if(!inherited) {
        e=rc_read(s,p.cmd,&cmd);if(e)return e;
        if(!(cmd&RC_FORCE) || (cmd&RC_UPDATE))return rc_fail(s,GGRCLK_LOST);
        e=rc_write(s,p.cmd,cmd&~RC_FORCE);if(e)return e;
        e=rc_read(s,p.cmd,&cmd);if(e)return e;
        /* Downstream can legitimately be gated; no ROOT_OFF==0 claim here. */
        if(cmd&(RC_FORCE|RC_UPDATE))return rc_fail(s,GGRCLK_READBACK);
        s->force_added_mask&=~(1u<<root);
    }
    s->configured_mask|=1u<<root;s->hz[root]=hz;
    s->phase=GGRCLK_DONE;s->last_code=GGRCLK_OK;return GGRCLK_OK;
}
#endif
