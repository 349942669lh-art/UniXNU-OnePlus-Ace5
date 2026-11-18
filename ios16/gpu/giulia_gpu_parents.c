/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Fixed register transaction, not a general register command interface.
 * Register policy from pinned gcc-pineapple.c and clk-branch.c; see the
 * accompanying public source manifest. Every callback is guarded, including
 * the post-write delay; no I/O or optimistic rollback follows uncertainty.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_parents.h"

static unsigned parent_bit(unsigned clock) { return 1u<<(15+clock); }
__attribute__((used)) void ggcp_quarantine(struct ggcp_state *s)
{ if(s){s->quarantined=1;s->ready=0;s->last_code=GGCP_QUARANTINED;} }
static int parent_fail(struct ggcp_state *s,unsigned code)
{ ggcp_quarantine(s);s->last_code=code;return (int)code; }
static int parent_live(struct ggcp_state *s)
{
    if(s->quarantined)return 0;
    if(s->io.live(s->io.cookie)==1)return 1;
    (void)parent_fail(s,GGCP_LOST);return 0;
}
static int parent_read(struct ggcp_state *s,uint32_t *out)
{
    if(!parent_live(s))return GGCP_LOST;
    int e=s->io.read32(s->io.cookie,out);
    if(!parent_live(s))return GGCP_LOST;
    if(e || *out==UINT32_MAX)return parent_fail(s,GGCP_IO);
    return GGCP_OK;
}
static int parent_snapshot(struct ggcp_state *s,uint32_t *out)
{
    int e=parent_read(s,out);if(e)return e;
    if((*out&GGCP_MASK)!=s->enabled)return parent_fail(s,GGCP_LOST);
    return GGCP_OK;
}
static int parent_write(struct ggcp_state *s,uint32_t before,unsigned enabled)
{
    const uint32_t value=(before&~GGCP_MASK)|enabled;
    if(!parent_live(s))return GGCP_LOST;
    s->write_attempted=1; /* Mark BEFORE a possibly partial MMIO operation. */
    int e=s->io.write32(s->io.cookie,value);
    if(!parent_live(s))return GGCP_LOST;
    if(e)return parent_fail(s,GGCP_IO);
    e=s->io.settle(s->io.cookie,GGCP_SETTLE_US);
    if(!parent_live(s))return GGCP_LOST;
    if(e)return parent_fail(s,GGCP_IO);
    uint32_t after=0;e=parent_read(s,&after);if(e)return e;
    /* The native RMW guard excludes other writers DURING this transaction.
     * Unrelated fields may change BETWEEN transactions, never within one. */
    if(after!=value)return parent_fail(s,GGCP_IO);
    s->enabled=enabled;return GGCP_OK;
}
__attribute__((used)) int ggcp_init(struct ggcp_state *s,const struct ggcp_io *io)
{
    if(!s || !io || !io->live || !io->read32 || !io->write32 ||
       !io->settle || !io->quiescent)return GGCP_ARGUMENT;
    if(s->attempted || s->quarantined || s->closed)return GGCP_USED;
    s->attempted=1;s->io=*io;
    uint32_t value=0;int e=parent_read(s,&value);if(e)return e;
    /* Do not adopt inherited bits as independent votes or ever clear them. */
    if(value&GGCP_MASK)return s->last_code=GGCP_INHERITED;
    s->ready=1;return s->last_code=GGCP_OK;
}
__attribute__((used)) int ggcp_acquire(struct ggcp_state *s,unsigned clock,uint64_t *ticket)
{
    if(!ticket)return GGCP_ARGUMENT;
    *ticket=0;
    if(!s || clock>=GGCP_CLOCKS)return GGCP_ARGUMENT;
    if(s->quarantined)return GGCP_QUARANTINED;
    if(!s->ready || s->closed)return GGCP_USED;
    unsigned slot=0;
    while(slot<GGCP_SLOTS && s->tickets[slot].serial)slot++;
    if(slot==GGCP_SLOTS || s->serial==UINT64_MAX)return GGCP_FULL;
    uint32_t before=0;int e=parent_snapshot(s,&before);if(e)return e;
    /* Publish/pin the native token before attempting the write. */
    s->tickets[slot].serial=++s->serial;s->tickets[slot].clock=clock;
    *ticket=s->serial;
    if(!s->refs[clock]++) {
        e=parent_write(s,before,s->enabled|parent_bit(clock));if(e)return e;
    }
    return s->last_code=GGCP_OK;
}
__attribute__((used)) int ggcp_release(struct ggcp_state *s,uint64_t ticket)
{
    if(!s || !ticket)return GGCP_ARGUMENT;
    if(s->quarantined)return GGCP_QUARANTINED;
    if(!s->ready || s->closed)return GGCP_USED;
    unsigned slot=0;
    while(slot<GGCP_SLOTS && s->tickets[slot].serial!=ticket)slot++;
    if(slot==GGCP_SLOTS)return GGCP_ARGUMENT;
    const unsigned clock=s->tickets[slot].clock;
    uint32_t before=0;int e=parent_snapshot(s,&before);if(e)return e;
    if(s->refs[clock]==1) {
        if(!parent_live(s))return GGCP_LOST;
        int quiet=s->io.quiescent(s->io.cookie);
        if(!parent_live(s))return GGCP_LOST;
        if(!quiet)return s->last_code=GGCP_BUSY;
        if(quiet!=1)return parent_fail(s,GGCP_IO);
        /* Quiescence can inspect dependencies but may not change this vote
         * register. Re-read after it rather than writing a stale snapshot. */
        e=parent_snapshot(s,&before);if(e)return e;
        e=parent_write(s,before,s->enabled&~parent_bit(clock));if(e)return e;
    }
    --s->refs[clock];s->tickets[slot].serial=0;
    return s->last_code=GGCP_OK;
}
__attribute__((used)) int ggcp_close(struct ggcp_state *s)
{
    if(!s)return GGCP_ARGUMENT;
    if(s->quarantined)return GGCP_QUARANTINED;
    if(s->closed)return GGCP_USED;
    if(s->refs[0] || s->refs[1])return GGCP_BUSY;
    if(s->ready) {
        uint32_t value=0;int e=parent_snapshot(s,&value);if(e)return e;
    }
    s->ready=0;s->closed=1;return s->last_code=GGCP_OK;
}
#endif
