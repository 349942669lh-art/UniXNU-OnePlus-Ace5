/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Exclusive single-CPU polling reservation of two boot-discovered GIC SPIs.
 * Native entry points are installed in pe_fiq.c by ios16_gpu_gunyah_irq_patch.
 * No function acknowledges an interrupt, clears pending state or consumes RM.
 */
#ifndef GIULIA_GPU_GUNYAH_IRQ_H
#define GIULIA_GPU_GUNYAH_IRQ_H
#include <stddef.h>
#include <stdint.h>

enum { GGIRQ_OK=0,GGIRQ_ARGUMENT=1,GGIRQ_USED=2,GGIRQ_CONFLICT=3,
    GGIRQ_ACTIVE=4,GGIRQ_TIMEOUT=5,GGIRQ_HARDWARE=6,GGIRQ_QUARANTINED=7,
    GGIRQ_POLLS=4096,GGIRQ_TIMEOUT_US=10000,GGIRQ_REG_BYTES=0x380,
    GGIRQ_CTLR=0,GGIRQ_TYPER=4,GGIRQ_ENABLE=0x100,GGIRQ_DISABLE=0x180,
    GGIRQ_PENDING=0x200,GGIRQ_ACTIVE_BITS=0x300 };
struct ggirq_io {
    void *cookie;size_t bytes;
    uint32_t (*read32)(void *,unsigned);
    void (*write32)(void *,unsigned,uint32_t);
    void (*barrier)(void *);
    uint64_t (*now_us)(void *);
    /* Native implementation checks initialized controller, boot CPU only,
     * topology.num_cpus==1, thread context and no installed generic handler.
     * The caller holds local interrupts disabled across the complete call. */
    int (*uncontended)(void *);
};
struct ggirq_state {
    const void *owner;
    uint64_t epoch;
    uint32_t intid[2],pending[2],active[2],enabled[2];
    unsigned attempted,reserved,claimed,quarantined,last_result;
};
static inline unsigned ggirq_offset(unsigned base,uint32_t intid)
{return base+4u*(intid/32u);}
static inline uint32_t ggirq_bit(uint32_t intid)
{return UINT32_C(1)<<(intid%32u);}
static inline int ggirq_io_valid(const struct ggirq_io *io)
{return io && io->bytes>=GGIRQ_REG_BYTES && io->read32 && io->write32 &&
    io->barrier && io->now_us && io->uncontended;}
static inline void ggirq_mask(const struct ggirq_io *io,const struct ggirq_state *s)
{
    /* Clear-enable is W1C: never read/modify/write unrelated SPI bits. */
    io->write32(io->cookie,ggirq_offset(GGIRQ_DISABLE,s->intid[0]),ggirq_bit(s->intid[0]));
    io->write32(io->cookie,ggirq_offset(GGIRQ_DISABLE,s->intid[1]),ggirq_bit(s->intid[1]));
    io->barrier(io->cookie);
}
static inline int ggirq_quarantine(const struct ggirq_io *io,struct ggirq_state *s,unsigned why)
{
    s->quarantined=1;s->last_result=why;
    /* A failed precondition never grants authority to mask someone else's
     * IRQ. Once reserved, retain the mask and reservation until reboot. */
    if(s->reserved && ggirq_io_valid(io))ggirq_mask(io,s);
    return (int)why;
}
static inline void ggirq_sample(const struct ggirq_io *io,struct ggirq_state *s)
{
    for(unsigned i=0;i<2;i++) {
        uint32_t id=s->intid[i],bit=ggirq_bit(id);
        s->enabled[i]=!!(io->read32(io->cookie,ggirq_offset(GGIRQ_ENABLE,id))&bit);
        s->active[i]=!!(io->read32(io->cookie,ggirq_offset(GGIRQ_ACTIVE_BITS,id))&bit);
        s->pending[i]=!!(io->read32(io->cookie,ggirq_offset(GGIRQ_PENDING,id))&bit);
    }
}
/* Zero this state once at cold boot. Actual INTIDs, owner and nonzero epoch
 * must come from the verified current boot handoff, never board constants.
 * Every valid claim attempt is one-shot; there is no online release/retry.
 * Existing generic interrupt handlers are rejected before any GIC write.
 * Pending bits may represent real queue data and are preserved throughout.
 */
static inline int ggirq_claim(const struct ggirq_io *io,struct ggirq_state *s,
    const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx)
{
    if(!ggirq_io_valid(io) || !s || !owner || !epoch || tx<32 || tx>1019 ||
       rx<32 || rx>1019 || tx==rx)return GGIRQ_ARGUMENT;
    if(s->attempted || s->reserved || s->claimed || s->quarantined || s->epoch || s->owner)return GGIRQ_USED;
    s->attempted=1;s->owner=owner;s->epoch=epoch;s->intid[0]=tx;s->intid[1]=rx;
    if(io->uncontended(io->cookie)!=1)return ggirq_quarantine(io,s,GGIRQ_CONFLICT);
    uint32_t supported=32u*((io->read32(io->cookie,GGIRQ_TYPER)&31u)+1u);
    if(tx>=supported || rx>=supported)return ggirq_quarantine(io,s,GGIRQ_HARDWARE);
    ggirq_sample(io,s);unsigned already_active=s->active[0] || s->active[1];
    s->reserved=1;ggirq_mask(io,s);
    uint64_t start=io->now_us(io->cookie),last=start;
    for(unsigned polls=0;;polls++) {
        uint64_t now=io->now_us(io->cookie);
        if(now<last || now-start>=GGIRQ_TIMEOUT_US || polls==GGIRQ_POLLS)
            return ggirq_quarantine(io,s,GGIRQ_TIMEOUT);
        last=now;
        if(!(io->read32(io->cookie,GGIRQ_CTLR)&UINT32_C(0x80000000)))break;
    }
    io->barrier(io->cookie);ggirq_sample(io,s);
    if(already_active || s->active[0] || s->active[1])return ggirq_quarantine(io,s,GGIRQ_ACTIVE);
    if(s->enabled[0] || s->enabled[1])return ggirq_quarantine(io,s,GGIRQ_HARDWARE);
    if(io->uncontended(io->cookie)!=1)return ggirq_quarantine(io,s,GGIRQ_CONFLICT);
    s->claimed=1;s->last_result=GGIRQ_OK;return GGIRQ_OK;
}
static inline int ggirq_matches(const struct ggirq_state *s,const void *owner,
    uint64_t epoch,uint32_t tx,uint32_t rx)
{return s && owner && epoch && s->owner==owner && s->epoch==epoch &&
    s->intid[0]==tx && s->intid[1]==rx;}
/* Returns exactly 1 for a current masked, inactive reservation. A new pending
 * notification does not revoke polling ownership. Any hardware/controller
 * drift permanently invalidates it and retains the masks. */
static inline int ggirq_owns(const struct ggirq_io *io,struct ggirq_state *s,
    const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx)
{
    if(!ggirq_io_valid(io) || !ggirq_matches(s,owner,epoch,tx,rx) ||
       !s->claimed || !s->reserved || s->quarantined)return 0;
    if(io->uncontended(io->cookie)!=1) {
        ggirq_quarantine(io,s,GGIRQ_CONFLICT);return 0;
    }
    ggirq_sample(io,s);
    if((io->read32(io->cookie,GGIRQ_CTLR)&UINT32_C(0x80000000)) ||
       s->enabled[0] || s->enabled[1] || s->active[0] || s->active[1]) {
        ggirq_quarantine(io,s,GGIRQ_HARDWARE);return 0;
    }
    return 1;
}
static inline void ggirq_invalidate(const struct ggirq_io *io,struct ggirq_state *s)
{if(s && s->attempted)ggirq_quarantine(io,s,GGIRQ_QUARANTINED);}
/* Called after the IRQ entry already read IAR. The existing IRQ entry still
 * performs its normal EOI. This hook only latches loss and masks the pair;
 * it must never run a message receive, doorbell clean or notification ACK. */
static inline int ggirq_dispatch(const struct ggirq_io *io,struct ggirq_state *s,uint32_t intid)
{
    if(!s || !s->reserved || (intid!=s->intid[0] && intid!=s->intid[1]))return 0;
    ggirq_invalidate(io,s);return 1;
}

#ifdef __cplusplus
extern "C" {
#endif
/* These symbols exist only with the actual platform patch installed. Native
 * wrappers disable interrupts while checking/changing GIC and reservation.
 * Claim/owns return 1 on success. No API grants HVC/capability compatibility.
 * The owner must also serialize its worker and hold guard_begin/end across
 * each actual HVC (never sleep under that guard) to close the validation gap.
 */
int giulia_gpu_gunyah_irq_claim(const void *,uint64_t,uint32_t,uint32_t);
int giulia_gpu_gunyah_irq_owns(const void *,uint64_t,uint32_t,uint32_t);
void giulia_gpu_gunyah_irq_invalidate(const void *,uint64_t);
int giulia_gpu_gunyah_irq_guard_begin(const void *,uint64_t,uint32_t,uint32_t,unsigned *);
int giulia_gpu_gunyah_irq_guard_end(const void *,uint64_t,uint32_t,uint32_t,unsigned);
int giulia_gpu_gunyah_irq_dispatch(uint32_t);
int giulia_gpu_gunyah_irq_cpu_uncontended(void);
void giulia_gpu_gunyah_irq_handler_registered(void);
void giulia_gpu_gunyah_irq_controller_changed(void);
#ifdef __cplusplus
}
#endif
#endif
