/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_submit.h"

__attribute__((used)) int giulia_gpu_submit_encode(const struct ggsub_request *r,
    unsigned sequence,uint32_t *out,unsigned capacity)
{
    if(!r || !out || !sequence || sequence>=4096 || !r->context ||
       !r->timestamp || r->queue<3 || r->queue>=GGH_QUEUES ||
       !r->count || r->count>GGSUB_MAX_IBS)return GGSUB_ARGUMENT;
    unsigned words=8+3*r->count;
    if(capacity<words)return GGSUB_ARGUMENT;
    for(unsigned i=0;i<r->count;i++) {
        const struct ggsub_ib *ib=&r->ibs[i];
        if(!ib->address || !ib->bytes || ((ib->address|ib->bytes)&3) ||
           ib->address>=((uint64_t)1<<32) || ib->bytes>((uint64_t)1<<32)-ib->address)
            return GGSUB_ARGUMENT;
    }
    /* Pinned adreno_hfi.h hfi_submit_cmd (8 words) + packed hfi_issue_ib
     * (u64 address, u32 BYTE size). NOT a dword-size PM4 descriptor.
     * Only NOTIFY; no profiling, indirect list, secure, recurring or RAW. */
    out[0]=(sequence<<20)|(words<<8)|130u;
    out[1]=r->context;out[2]=1u<<5;out[3]=r->timestamp;
    out[4]=out[5]=out[7]=0;out[6]=r->count;
    for(unsigned i=0;i<r->count;i++) {
        out[8+3*i]=(uint32_t)r->ibs[i].address;
        out[9+3*i]=(uint32_t)(r->ibs[i].address>>32);
        out[10+3*i]=r->ibs[i].bytes;
    }
    return GGSUB_OK;
}
static int submit_live(struct ggsub_state *s)
{
    return giulia_gpu_runtime_live(s->runtime) &&
        s->ops.proof(s->ops.cookie,s->runtime->epoch,s->request.context,s->request.queue)==1;
}
__attribute__((used)) void giulia_gpu_submit_invalidate(struct ggsub_state *s)
{
    if(!s)return;
    s->quarantined=1;s->completed=0;
    giulia_gpu_runtime_quarantine(s->runtime);
    /* Keep the fence/buffer token, queue and runtime leases until reset. */
}
static int submit_fail(struct ggsub_state *s,int code)
{ giulia_gpu_submit_invalidate(s);return code; }
__attribute__((used)) int giulia_gpu_submit_init(struct ggsub_state *s,
    struct ggrt_state *runtime,const struct ggh_io *io,const struct ggsub_ops *ops)
{
    if(!s || !runtime || !io || !io->words || !io->barrier ||
       !ops || !ops->proof || !ops->pin || !ops->cancel || !ops->ring ||
       !ops->fence || !ops->retire || !ops->now_us)return GGSUB_ARGUMENT;
    if(s->initialized || s->quarantined)return GGSUB_BUSY;
    if(!giulia_gpu_runtime_live(runtime))return GGSUB_NOT_READY;
    s->runtime=runtime;s->io=*io;s->ops=*ops;s->initialized=1;return GGSUB_OK;
}
static int submit_cancel(struct ggsub_state *s,int code)
{
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    if(s->ops.cancel(s->ops.cookie,s->token))return submit_fail(s,GGSUB_FAULT);
    s->token=0;s->busy=0;
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    return code;
}
__attribute__((used)) int giulia_gpu_submit(struct ggsub_state *s,
    const struct ggsub_request *request,unsigned timeout_us)
{
    uint32_t msg[GGSUB_WORDS];
    volatile uint32_t *header,*ring;unsigned r,w;
    if(!s || !s->initialized || !request || !timeout_us || timeout_us>3000000)return GGSUB_ARGUMENT;
    if(s->quarantined)return GGSUB_QUARANTINED;
    if(s->busy)return GGSUB_BUSY;
    if(s->sequence>=4095 || s->last_timestamp==UINT32_MAX)return GGSUB_EXHAUSTED;
    s->request=*request;
    const struct ggsub_request *req=&s->request;
    if(req->timestamp<=s->last_timestamp ||
       giulia_gpu_submit_encode(req,s->sequence+1,msg,GGSUB_WORDS))return GGSUB_ARGUMENT;
    s->completed=s->published=0;
    if(!submit_live(s))return GGSUB_NOT_READY;
    unsigned words=8+3*req->count,padded=(words+3)&~3u;
    int code=ggh_queue(&s->io,req->queue,&header,&ring,&r,&w);
    if(code)return submit_fail(s,GGSUB_FAULT);
    if(((r-w-1)&1023)<padded)return GGSUB_FULL;
    s->start_us=s->last_us=s->ops.now_us(s->ops.cookie);s->timeout_us=timeout_us;
    s->token=0;
    code=s->ops.pin(s->ops.cookie,s->runtime->epoch,&s->request,&s->token);
    if(code==1 && !s->token)return GGSUB_PIN;
    if(code || !s->token)return submit_fail(s,GGSUB_FAULT);
    s->busy=1;
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    /* Re-read after pin; the provider never hands a second producer this
     * queue, but the firmware consumer may have advanced its read index. */
    code=ggh_queue(&s->io,req->queue,&header,&ring,&r,&w);
    if(code)return submit_fail(s,GGSUB_FAULT);
    if(((r-w-1)&1023)<padded)return submit_cancel(s,GGSUB_FULL);
    if(giulia_gpu_runtime_dma(s->runtime))return submit_fail(s,GGSUB_LOST);
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    for(unsigned i=0;i<padded;i++)ring[(w+i)&1023]=i<words?msg[i]:0xfafafafa;
    s->io.barrier(s->io.cookie);
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    uint64_t now=s->ops.now_us(s->ops.cookie);
    if(now<s->last_us || now-s->start_us>=s->timeout_us)
        return submit_cancel(s,GGSUB_TIMEOUT);
    s->last_us=now;
    /* Publication itself can start execution even before the doorbell. */
    s->published=1;s->sequence++;s->last_timestamp=req->timestamp;
    header[11]=(w+padded)&1023;s->io.barrier(s->io.cookie);
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    /* Original driver: BIT(dispatch_index + 2), dispatch_index = queue - 3. */
    if(s->ops.ring(s->ops.cookie,1u<<(req->queue-1)))return submit_fail(s,GGSUB_FAULT);
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    return GGSUB_OK; /* Accepted, NOT completed. Never synthesize a fence. */
}
__attribute__((used)) int giulia_gpu_submit_poll(struct ggsub_state *s)
{
    if(!s || !s->initialized)return GGSUB_ARGUMENT;
    if(s->quarantined)return GGSUB_QUARANTINED;
    if(!s->busy)return s->completed?GGSUB_COMPLETE:GGSUB_NOT_READY;
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    uint64_t now=s->ops.now_us(s->ops.cookie);
    if(now<s->last_us || now<s->start_us || now-s->start_us>=s->timeout_us)
        return submit_fail(s,GGSUB_TIMEOUT);
    s->last_us=now;
    int code=s->ops.fence(s->ops.cookie,s->token,s->runtime->epoch,
        s->request.context,s->request.timestamp);
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    now=s->ops.now_us(s->ops.cookie);
    if(now<s->last_us || now-s->start_us>=s->timeout_us)return submit_fail(s,GGSUB_TIMEOUT);
    s->last_us=now;
    if(!code)return GGSUB_PENDING;
    if(code!=1)return submit_fail(s,GGSUB_FAULT);
    if(s->ops.retire(s->ops.cookie,s->token))return submit_fail(s,GGSUB_FAULT);
    s->token=0;s->busy=0;
    if(!submit_live(s))return submit_fail(s,GGSUB_LOST);
    s->completed=1;
    /* A per-command fence does not clear runtime DMA marks or power leases. */
    return GGSUB_COMPLETE;
}
#endif
