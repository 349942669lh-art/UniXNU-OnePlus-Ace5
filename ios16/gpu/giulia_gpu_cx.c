/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed CX transactions from pinned public register and sequencing facts.
 * Native allocation, supplies and session lifetime belong to the caller.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_cx.h"
static const struct ggcx_policy cx_policy={
    GGCX_CONTROL,GGCX_STATUS,6u,0xf000u,0x8000u,1u,0x800u,0x80000000u,
    GGCX_POLLS,GGCX_WINDOWS
};
__attribute__((used)) const char *ggcx_identity(void)
{
    return GGCX_IDENTITY;
}
__attribute__((used)) int ggcx_describe(struct ggcx_policy *out)
{
    if(!out)return GGCX_ARGUMENT;
    *out=cx_policy;
    return GGCX_OK;
}
static int cx_fail(struct ggcx_state *s,unsigned code)
{
    if(!s->quarantined)s->last_code=code;
    s->quarantined=1;
    return (int)code;
}
static int cx_live(struct ggcx_state *s)
{
    if(s->quarantined)return GGCX_QUARANTINED;
    if(s->io.live(s->io.cookie)!=1)return cx_fail(s,GGCX_LOST);
    return GGCX_OK;
}
static int cx_read(struct ggcx_state *s,uint32_t offset,uint32_t *value)
{
    int code=cx_live(s);
    if(code)return code;
    s->last_offset=offset;
    ++s->reads;
    int result=s->io.read32(s->io.cookie,offset,value);
    code=cx_live(s);
    if(code)return code;
    if(result || *value==0xffffffffu)return cx_fail(s,GGCX_IO);
    s->observed=*value;
    return GGCX_OK;
}
static int cx_write(struct ggcx_state *s,uint32_t value)
{
    int code=cx_live(s);
    if(code)return code;
    s->mutation_attempted=1;
    s->last_offset=GGCX_CONTROL;
    ++s->writes;
    int result=s->io.write32(s->io.cookie,GGCX_CONTROL,value);
    code=cx_live(s);
    if(code)return code;
    return result?cx_fail(s,GGCX_IO):GGCX_OK;
}
static int cx_wait(struct ggcx_state *s)
{
    int code=cx_live(s);
    if(code)return code;
    ++s->waits;
    int result=s->io.wait_us(s->io.cookie,1);
    code=cx_live(s);
    if(code)return code;
    return result?cx_fail(s,GGCX_IO):GGCX_OK;
}
static int cx_verify(struct ggcx_state *s,uint32_t mask,uint32_t expected)
{
    uint32_t value=0;
    int code=cx_read(s,GGCX_CONTROL,&value);
    if(code)return code;
    return (value&mask)==expected?GGCX_OK:cx_fail(s,GGCX_READBACK);
}
static int cx_poll(struct ggcx_state *s)
{
    s->phase=GGCX_POLL;
    for(unsigned window=0;window<GGCX_WINDOWS;++window) {
        s->poll_window=window+1;
        for(unsigned poll=0;poll<GGCX_POLLS;++poll) {
            uint32_t value=0;
            ++s->polls;
            int code=cx_read(s,GGCX_STATUS,&value);
            if(code)return code;
            if(value&cx_policy.power_mask)return GGCX_OK;
            code=cx_wait(s);
            if(code)return code;
        }
        /* Source check_gdsc_status observes both registers before its second
         * polling window. These reads never replay the enable transaction. */
        uint32_t diagnostic=0;
        int code=cx_read(s,GGCX_CONTROL,&diagnostic);
        if(code)return code;
        code=cx_read(s,GGCX_STATUS,&diagnostic);
        if(code)return code;
    }
    return cx_fail(s,GGCX_TIMEOUT);
}
__attribute__((used)) int ggcx_open(struct ggcx_state *s,const struct ggcx_io *io)
{
    if(!s || !io || !io->live || !io->ready || !io->quiescent ||
       !io->read32 || !io->write32 || !io->wait_us)return GGCX_ARGUMENT;
    if(s->opened || s->quarantined)return GGCX_USED;
    s->io=*io;
    s->opened=1;
    return cx_live(s);
}
__attribute__((used)) void ggcx_invalidate(struct ggcx_state *s)
{
    if(s)(void)cx_fail(s,GGCX_LOST);
}
__attribute__((used)) int ggcx_enable(struct ggcx_state *s)
{
    if(!s)return GGCX_ARGUMENT;
    if(s->quarantined)return GGCX_QUARANTINED;
    if(!s->opened || s->attempted)return GGCX_USED;
    int code=cx_live(s);
    if(code)return code;
    int ready=s->io.ready(s->io.cookie);
    code=cx_live(s);
    if(code)return code;
    if(!ready)return s->last_code=GGCX_UNAVAILABLE;
    if(ready!=1)return cx_fail(s,GGCX_LOST);
    int quiet=s->io.quiescent(s->io.cookie);
    code=cx_live(s);
    if(code)return code;
    if(!quiet)return s->last_code=GGCX_BUSY;
    if(quiet!=1)return cx_fail(s,GGCX_LOST);
    s->attempted=1;
    s->phase=GGCX_PREPARE;
    uint32_t value=0;
    code=cx_read(s,GGCX_CONTROL,&value);
    if(code)return code;
    s->before=value;
    s->inherited=!(value&cx_policy.collapse_mask);
    uint32_t configured=(value&~(cx_policy.mode_mask|cx_policy.wait_mask))|cx_policy.wait_value;
    if(configured!=value) {
        code=cx_write(s,configured);
        if(code)return code;
    }
    code=cx_verify(s,cx_policy.mode_mask|cx_policy.wait_mask,cx_policy.wait_value);
    if(code)return code;
    /* The source's NORMAL-mode handover includes a 1us settle. */
    if(value&2u) {
        code=cx_wait(s);
        if(code)return code;
    }
    s->phase=GGCX_ENABLE;
    code=cx_read(s,GGCX_CONTROL,&value);
    if(code)return code;
    if((value&(cx_policy.mode_mask|cx_policy.wait_mask))!=cx_policy.wait_value)
        return cx_fail(s,GGCX_READBACK);
    if(value&cx_policy.collapse_mask) {
        code=cx_write(s,value&~cx_policy.collapse_mask);
        if(code)return code;
    }
    code=cx_verify(s,cx_policy.mode_mask|cx_policy.wait_mask|cx_policy.collapse_mask,cx_policy.wait_value);
    if(code)return code;
    code=cx_wait(s);
    if(code)return code;
    code=cx_poll(s);
    if(code)return code;
    s->phase=GGCX_RETAIN;
    code=cx_read(s,GGCX_CONTROL,&value);
    if(code)return code;
    if((value&(cx_policy.mode_mask|cx_policy.wait_mask|cx_policy.collapse_mask))!=cx_policy.wait_value)
        return cx_fail(s,GGCX_READBACK);
    if(!(value&cx_policy.retain_mask)) {
        code=cx_write(s,value|cx_policy.retain_mask);
        if(code)return code;
    }
    uint32_t final_mask=cx_policy.mode_mask|cx_policy.wait_mask|cx_policy.collapse_mask|cx_policy.retain_mask;
    code=cx_verify(s,final_mask,cx_policy.wait_value|cx_policy.retain_mask);
    if(code)return code;
    code=cx_wait(s);
    if(code)return code;
    code=cx_wait(s);
    if(code)return code;
    code=cx_read(s,GGCX_STATUS,&value);
    if(code)return code;
    if(!(value&cx_policy.power_mask))return cx_fail(s,GGCX_READBACK);
    code=cx_verify(s,final_mask,cx_policy.wait_value|cx_policy.retain_mask);
    if(code)return code;
    s->enabled=1;
    s->phase=GGCX_DONE;
    s->last_code=GGCX_OK;
    return GGCX_OK;
}
#endif
