/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Independently written restricted ACTIVE TCS transaction engine.
 * Register protocol: pinned OnePlusOSS rpmh-rsc.c, see hardware manifest.
 * This file supplies no native power authority or production IO callbacks.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_rsc.h"

#define RSC_IRQ_ENABLE 0u
#define RSC_IRQ_STATUS 4u
#define RSC_IRQ_CLEAR 8u
#define RSC_TCS_STRIDE 672u
#define RSC_MODE (1u<<16)
#define RSC_TRIGGER (1u<<24)
#define RSC_MESSAGE (8u|(1u<<8)|(1u<<16))
#define RSC_DONE ((1u<<8)|(1u<<16))
#define RSC_SYNC_TRIES 16u
#define RSC_MAX_POLLS 4096u

static int rsc_code(struct ggrsc_state *s,int code)
{ s->last_code=(uint32_t)code;return code; }
static int rsc_fault(struct ggrsc_state *s,int code)
{ s->ready=0;s->quarantined=1;return rsc_code(s,code); }
static int rsc_live(struct ggrsc_state *s)
{ return s->io.live(s->io.cookie,s->config.epoch)==1 ? GGRSC_OK : rsc_fault(s,GGRSC_LOST); }
static int rsc_read(struct ggrsc_state *s,uint32_t offset,uint32_t *out)
{
    if(rsc_live(s))return GGRSC_LOST;
    uint32_t value=0;int code=s->io.read32(s->io.cookie,offset,&value);
    if(rsc_live(s))return GGRSC_LOST;
    if(code || value==0xffffffffu)return rsc_fault(s,GGRSC_IO);
    *out=value;return GGRSC_OK;
}
static int rsc_write(struct ggrsc_state *s,uint32_t offset,uint32_t value)
{
    if(rsc_live(s))return GGRSC_LOST;
    s->mutation_attempted=1;
    int code=s->io.write32(s->io.cookie,offset,value);
    if(rsc_live(s))return GGRSC_LOST;
    return code ? rsc_fault(s,GGRSC_IO) : GGRSC_OK;
}
static int rsc_now(struct ggrsc_state *s,uint64_t *out)
{
    if(rsc_live(s))return GGRSC_LOST;
    uint64_t now=0;int code=s->io.now_us(s->io.cookie,&now);
    if(rsc_live(s))return GGRSC_LOST;
    if(code)return rsc_fault(s,GGRSC_IO);
    if(now<s->last_us)return rsc_fault(s,GGRSC_PROTOCOL);
    s->last_us=now;*out=now;return GGRSC_OK;
}
static int rsc_sync(struct ggrsc_state *s,uint32_t offset,uint32_t value)
{
    int code=rsc_write(s,offset,value);
    if(code)return code;
    for(unsigned i=0;i<RSC_SYNC_TRIES;i++) {
        uint32_t found=0;code=rsc_read(s,offset,&found);
        if(code)return code;
        if(found==value)return GGRSC_OK;
        if(i+1==RSC_SYNC_TRIES)break;
        if(rsc_live(s))return GGRSC_LOST;
        code=s->io.delay_us(s->io.cookie,1);
        if(rsc_live(s))return GGRSC_LOST;
        if(code)return rsc_fault(s,GGRSC_IO);
    }
    return rsc_fault(s,GGRSC_TIMEOUT);
}
static int rsc_clean(struct ggrsc_state *s)
{
    uint32_t value=0;
    int code=rsc_read(s,RSC_IRQ_ENABLE,&value);
    if(code)return code;
    if(!(value&(1u<<s->config.tcs_id)))return rsc_fault(s,GGRSC_PROTOCOL);
    code=rsc_read(s,RSC_IRQ_STATUS,&value);
    if(code)return code;
    if(value&(1u<<s->config.tcs_id))return rsc_fault(s,GGRSC_PROTOCOL);
    const uint32_t offsets[4]={s->control,s->enable,s->wait,s->status};
    for(unsigned i=0;i<4;i++) {
        code=rsc_read(s,offsets[i],&value);
        if(code)return code;
        if((i==3 && !value) || (i!=3 && value))return rsc_fault(s,GGRSC_PROTOCOL);
    }
    return GGRSC_OK;
}
__attribute__((used)) const char *ggrsc_identity(void) { return GGRSC_IDENTITY; }
__attribute__((used)) int ggrsc_open(struct ggrsc_state *s,const struct ggrsc_config *c,const struct ggrsc_io *io)
{
    if(!s || !c || !io)return GGRSC_ARGUMENT;
    if(s->initialized || s->closed || s->quarantined)return rsc_code(s,GGRSC_USED);
    if((c->abi!=GGRSC_ABI && c->abi!=GGRSC_LEGACY_ABI) || (c->version!=GGRSC_V27 && c->version!=GGRSC_V30) ||
       c->tcs_id>=32 || !c->epoch || c->reserved ||
       !c->arc_address || (c->arc_address&0xfff00003u) || ((c->arc_address>>16)&15u)!=3 ||
       !c->selector_mask || (c->selector_mask&0xffff0000u) ||
       !c->timeout_us || c->timeout_us>1000000u ||
       !io->live || !io->read32 || !io->write32 || !io->now_us || !io->delay_us)
        return rsc_code(s,GGRSC_ARGUMENT);
    if(c->abi==GGRSC_ABI && (c->second_arc_address || c->second_selector_mask) &&
       (!c->second_arc_address || c->second_arc_address==c->arc_address ||
        (c->second_arc_address&0xfff00003u) || ((c->second_arc_address>>16)&15u)!=3 ||
        !c->second_selector_mask || (c->second_selector_mask&0xffff0000u)))
        return rsc_code(s,GGRSC_ARGUMENT);
    /* Last slot reserved in the source-authorized TCS mapping. We touch only
     * slot 0 but reject mappings shorter than a complete legacy TCS block. */
    const uint32_t end=RSC_TCS_STRIDE*(c->tcs_id+1u);
    if((c->mapped_bytes&3u) || c->mapped_bytes<end || c->mapped_bytes>65536u)
        return rsc_code(s,GGRSC_ARGUMENT);
    /* Copy the ABI 1 prefix field by field. A binary ABI 1 caller may have
     * allocated only 40 bytes, so a full ABI 2 structure read is invalid. */
    s->config.abi=c->abi;s->config.version=c->version;
    s->config.mapped_bytes=c->mapped_bytes;s->config.tcs_id=c->tcs_id;
    s->config.arc_address=c->arc_address;s->config.selector_mask=c->selector_mask;
    s->config.timeout_us=c->timeout_us;s->config.reserved=c->reserved;
    s->config.epoch=c->epoch;
    if(c->abi==GGRSC_ABI) {
        s->config.second_arc_address=c->second_arc_address;
        s->config.second_selector_mask=c->second_selector_mask;
    }
    s->io=*io;s->initialized=1;
    const uint32_t base=RSC_TCS_STRIDE*c->tcs_id;
    const uint32_t shift=c->version==GGRSC_V30 ? 16u : 0u;
    s->wait=base+0x10u+shift;s->control=base+0x14u+shift;
    s->status=base+0x18u+shift;s->enable=base+0x1cu+shift;
    const uint32_t cmd_shift=c->version==GGRSC_V30 ? 4u : 0u;
    s->msg=base+0x30u+cmd_shift;s->addr=s->msg+4u;
    s->data=s->msg+8u;s->cmd_status=s->msg+12u;
    int code=rsc_clean(s);
    if(code)return code;
    uint64_t now=0;code=rsc_now(s,&now);
    if(code)return code;
    s->ready=1;return rsc_code(s,GGRSC_OK);
}
__attribute__((used)) int ggrsc_submit_arc(struct ggrsc_state *s,uint32_t address,uint32_t selector,uint64_t *serial)
{
    if(!s || !serial)return GGRSC_ARGUMENT;
    /* Output must not alias s. Never erase a retained uncertain serial. */
    *serial=0;
    if(s->quarantined)return rsc_code(s,GGRSC_QUARANTINED);
    if(!s->initialized || !s->ready || s->closed)return rsc_code(s,GGRSC_ARGUMENT);
    if(s->inflight)return rsc_code(s,GGRSC_BUSY);
    uint32_t mask=0;
    if(address==s->config.arc_address)mask=s->config.selector_mask;
    else if(address && address==s->config.second_arc_address)mask=s->config.second_selector_mask;
    if(selector>=16 || !(mask&(1u<<selector)))return rsc_code(s,GGRSC_ARGUMENT);
    if(s->serial==~(uint64_t)0)return rsc_fault(s,GGRSC_PROTOCOL);
    int code=rsc_clean(s);
    if(code)return code;
    uint64_t now=0;code=rsc_now(s,&now);
    if(code)return code;
    s->started_us=now;s->polls=0;s->selector=selector;s->arc_address=address;
    ++s->serial;*serial=s->serial;s->inflight=1;
    /* Hardware sequence matches the public active TCS protocol, all writes
     * before trigger read back boundedly. No readback of self-changing trigger. */
    const uint32_t offsets[8]={s->control,s->control,s->msg,s->addr,s->data,s->wait,s->enable,s->control};
    const uint32_t values[8]={0,0,RSC_MESSAGE,s->arc_address,selector,1,1,RSC_MODE};
    for(unsigned i=0;i<8;i++) {
        code=rsc_sync(s,offsets[i],values[i]);if(code)return code;
    }
    /* Reject stale completion before the only hardware launch. */
    uint32_t irq=0;code=rsc_read(s,RSC_IRQ_STATUS,&irq);if(code)return code;
    if(irq&(1u<<s->config.tcs_id))return rsc_fault(s,GGRSC_PROTOCOL);
    code=rsc_now(s,&now);if(code)return code;
    if(now-s->started_us>=s->config.timeout_us)return rsc_fault(s,GGRSC_TIMEOUT);
    s->trigger_attempted=1;
    code=rsc_write(s,s->control,RSC_MODE|RSC_TRIGGER);
    return code ? code : rsc_code(s,GGRSC_OK);
}
__attribute__((used)) int ggrsc_submit(struct ggrsc_state *s,uint32_t selector,uint64_t *serial)
{
    return ggrsc_submit_arc(s,s ? s->config.arc_address : 0,selector,serial);
}
__attribute__((used)) int ggrsc_poll(struct ggrsc_state *s,uint64_t serial)
{
    if(!s || !serial)return GGRSC_ARGUMENT;
    if(s->quarantined)return rsc_code(s,GGRSC_QUARANTINED);
    if(!s->initialized || !s->ready || s->closed || serial!=s->serial)return rsc_code(s,GGRSC_ARGUMENT);
    if(!s->inflight)return rsc_code(s,GGRSC_ARGUMENT); /* no repeated completion */
    uint64_t now=0;int code=rsc_now(s,&now);if(code)return code;
    if(now-s->started_us>=s->config.timeout_us || ++s->polls>RSC_MAX_POLLS)
        return rsc_fault(s,GGRSC_TIMEOUT);
    uint32_t value=0;code=rsc_read(s,RSC_IRQ_ENABLE,&value);if(code)return code;
    if(!(value&(1u<<s->config.tcs_id)))return rsc_fault(s,GGRSC_PROTOCOL);
    code=rsc_read(s,RSC_IRQ_STATUS,&value);if(code)return code;
    if(!(value&(1u<<s->config.tcs_id)))return rsc_code(s,GGRSC_PENDING);
    code=rsc_read(s,s->cmd_status,&value);if(code)return code;
    if(value!=RSC_DONE)return rsc_fault(s,GGRSC_PROTOCOL);
    code=rsc_read(s,s->status,&value);if(code)return code;
    if(!value)return rsc_fault(s,GGRSC_PROTOCOL);
    /* Explicit untrigger then mode-disable, followed by disable and own-bit
     * W1C only. Do not acknowledge another TCS's IRQ or alter IRQ_ENABLE. */
    const uint32_t offsets[4]={s->control,s->control,s->enable,s->wait};
    const uint32_t values[4]={RSC_MODE,0,0,0};
    for(unsigned i=0;i<4;i++) {code=rsc_sync(s,offsets[i],values[i]);if(code)return code;}
    code=rsc_write(s,RSC_IRQ_CLEAR,1u<<s->config.tcs_id);if(code)return code;
    code=rsc_clean(s);if(code)return code;
    s->completed=s->serial;s->inflight=0;return rsc_code(s,GGRSC_OK);
}
__attribute__((used)) int ggrsc_close(struct ggrsc_state *s)
{
    if(!s || !s->initialized || s->closed)return GGRSC_ARGUMENT;
    if(s->quarantined)return rsc_code(s,GGRSC_QUARANTINED);
    if(s->inflight)return rsc_code(s,GGRSC_BUSY);
    int code=rsc_clean(s);if(code)return code;
    s->ready=0;s->closed=1;return rsc_code(s,GGRSC_OK);
}
__attribute__((used)) void ggrsc_invalidate(struct ggrsc_state *s)
{ if(s && s->initialized && !s->closed)(void)rsc_fault(s,GGRSC_LOST); }
#endif
