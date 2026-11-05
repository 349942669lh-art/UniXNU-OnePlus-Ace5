/* Actual runtime + encoder/queue code, real shared-ledger algorithms, fake
 * hardware providers. No phone/MMIO/firmware execution or acceleration proof. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_submit.h"

static struct gpr_broker broker;
static unsigned mark_calls,mark_fail,release_calls,release_fail,cases;
int gpr_global_initialize(void){return 0;}
int gpr_global_open_epoch(const void *o,uint64_t e){return gpr_open_epoch(&broker,o,e);}
int gpr_global_retire_epoch(const void *o,uint64_t e){return gpr_retire_epoch(&broker,o,e);}
int gpr_global_claim(const struct gpr_key *k,const void *o,uint64_t e,unsigned f,uint64_t v,struct gpr_token *t)
{return gpr_claim(&broker,k,o,e,f,v,t);}
int gpr_global_owns(const struct gpr_token *t,const void *o,uint64_t e){return gpr_owns(&broker,t,o,e);}
int gpr_global_release(const struct gpr_token *t,const void *o,uint64_t e)
{return ++release_calls==release_fail?GPR_UNCERTAIN:gpr_release(&broker,t,o,e);}
int gpr_global_mark_dma(const struct gpr_token *t,const void *o,uint64_t e)
{return ++mark_calls==mark_fail?GPR_UNCERTAIN:gpr_mark_dma(&broker,t,o,e);}
int gpr_global_quiesce(const struct gpr_token *t,const void *o,uint64_t e){return gpr_quiesce(&broker,t,o,e);}
int gpr_global_quarantine(const struct gpr_token *t,const void *o,uint64_t e){return gpr_quarantine(&broker,t,o,e);}

struct fixture {
    struct ggrt_state runtime;
    struct ggsub_state submit;
    struct ggrt_ops runtime_ops;
    struct ggsub_ops submit_ops;
    struct ggh_io io;
    uint32_t memory[GGH_BYTES/4+8];
    unsigned authority,quiet,observed,proof,power_fail,power_uncertain,power_held;
    unsigned acquires,releases,power_release_fail,pin_calls,cancel_calls,rings,fences,retires;
    unsigned pin_result,pin_zero,ring_fail,cancel_fail,retire_fail,fence_result;
    unsigned lose_on_pin,lose_on_ring,lose_on_fence,corrupt_on_pin,full_on_pin;
    unsigned barriers,lose_barrier,advance_pin,advance_fence,lose_on_retire;
    unsigned power_calls,lose_after_power_call,loss_kind,ownership_lost;
    unsigned authority_checks,deny_authority_at;
    uint64_t now;
};
static struct fixture f;
static int authority(void *p,uint64_t e)
{
    struct fixture *x=p;assert(e==77);
    if(++x->authority_checks==x->deny_authority_at) {
        x->authority=0;x->ownership_lost=1;
    }
    return x->authority;
}
static void power_enter(struct fixture *x)
{
    /* A real hardware callback must never follow a detected loss. This also
     * catches optimistic rollback after resolve/quiescent/poll/wait failure. */
    assert(!x->ownership_lost && x->authority);
    for(unsigned i=0;i<GGRT_LEASES;i++)
        assert(gpr_owns(&broker,&x->runtime.leases[i],&x->runtime,77));
    ++x->power_calls;
}
static void power_leave(struct fixture *x)
{
    if(x->power_calls!=x->lose_after_power_call)return;
    x->ownership_lost=1;
    if(x->loss_kind==0)x->authority=0;
    else {
        unsigned slot=x->loss_kind-1;
        assert(slot<GGRT_LEASES);
        assert(!gpr_quarantine(&broker,&x->runtime.leases[slot],&x->runtime,77));
    }
}
static int resolve(void *p){struct fixture *x=p;power_enter(x);power_leave(x);return 0;}
static int quiet(void *p){struct fixture *x=p;power_enter(x);power_leave(x);return x->quiet;}
static enum gg_resource_result power_acquire(void *p,unsigned i,uint32_t v,uint64_t *t)
{
    struct fixture *x=p;power_enter(x);++x->acquires;
    assert(i<GGR_COUNT && v==(i==GGR_GMU_RATE?625000000u:i==GGR_HUB_RATE?200000000u:1));
    if(x->power_fail==i+1) {
        power_leave(x);return x->power_uncertain?GGRC_UNCERTAIN:GGRC_UNCHANGED_ERROR;
    }
    x->power_held|=1u<<i;*t=100+i;power_leave(x);return GGRC_OK;
}
static int power_release(void *p,unsigned i,uint64_t t)
{
    struct fixture *x=p;power_enter(x);++x->releases;assert(t==100+i && (x->power_held&(1u<<i)));
    if(x->power_release_fail==i+1){power_leave(x);return 5;}
    x->power_held&=~(1u<<i);power_leave(x);return 0;
}
static int observe(void *p){struct fixture *x=p;power_enter(x);power_leave(x);return x->observed;}
static int wait_us(void *p,unsigned us){struct fixture *x=p;assert(us==10);power_enter(x);power_leave(x);return 0;}
static volatile uint32_t *hdr(unsigned q){return f.memory+GGH_HEADER_WORDS+q*GGH_QUEUE_HEADER_WORDS;}
static volatile uint32_t *ring_memory(unsigned q){return f.memory+GGH_RING_OFFSET/4+q*GGH_RING_WORDS;}
static int proof(void *p,uint64_t e,uint32_t c,uint32_t q)
{assert(e==77 && c==9 && q>=3 && q<17);return ((struct fixture *)p)->proof;}
static int pin(void *p,uint64_t e,const struct ggsub_request *r,uint64_t *token)
{
    struct fixture *x=p;assert(e==77 && r->context==9);++x->pin_calls;
    if(!x->pin_zero)*token=1000+r->timestamp;
    if(x->lose_on_pin)x->authority=0;
    if(x->corrupt_on_pin)hdr(r->queue)[3]=0;
    if(x->full_on_pin)hdr(r->queue)[10]=(hdr(r->queue)[11]+4)&1023;
    x->now+=x->advance_pin;
    return (int)x->pin_result;
}
static int cancel(void *p,uint64_t t){struct fixture *x=p;assert(t>1000);++x->cancel_calls;return x->cancel_fail;}
static int ring_hw(void *p,uint32_t mask)
{
    struct fixture *x=p;assert(mask==(1u<<(x->submit.request.queue-1)));
    assert(x->runtime.dma && x->submit.busy && x->submit.published);
    for(unsigned i=1;i<GGRT_LEASES;i++)assert(broker.leases[x->runtime.leases[i].slot].dma_active);
    ++x->rings;if(x->lose_on_ring)x->authority=0;return x->ring_fail;
}
static int fence(void *p,uint64_t token,uint64_t e,uint32_t c,uint32_t ts)
{
    struct fixture *x=p;assert(e==77 && c==9 && token==1000+ts);++x->fences;
    if(x->lose_on_fence)x->proof=0;
    x->now+=x->advance_fence;
    return (int)x->fence_result;
}
static int retire(void *p,uint64_t t){struct fixture *x=p;assert(t>1000);++x->retires;if(x->lose_on_retire)x->authority=0;return x->retire_fail;}
static uint64_t now_us(void *p){return ((struct fixture *)p)->now;}
static void barrier(void *p){struct fixture *x=p;if(++x->barriers==x->lose_barrier)x->authority=0;}
static void reset(void)
{
    memset(&broker,0,sizeof(broker));memset(&f,0,sizeof(f));
    mark_calls=mark_fail=release_calls=release_fail=0;
    f.authority=f.quiet=f.observed=f.proof=1;f.now=100;
    f.runtime_ops=(struct ggrt_ops){&f,authority,{&f,resolve,quiet,power_acquire,power_release,observe,wait_us,0}};
    f.submit_ops=(struct ggsub_ops){&f,proof,pin,cancel,ring_hw,fence,retire,now_us};
    f.io=(struct ggh_io){f.memory,GGH_BYTES,GGH_IOVA,&f,barrier};
    assert(!ggh_init(f.memory,GGH_BYTES,GGH_IOVA));f.memory[GGH_BYTES/4]=0xabcdef;
}
static void ready(void)
{
    reset();assert(!giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77));
    assert(giulia_gpu_runtime_live(&f.runtime));
    assert(!giulia_gpu_submit_init(&f.submit,&f.runtime,&f.io,&f.submit_ops));
}
static struct ggsub_request request(unsigned q,unsigned count)
{
    struct ggsub_request r={0};r.context=9;r.queue=q;r.timestamp=1;r.count=count;
    for(unsigned i=0;i<count && i<GGSUB_MAX_IBS;i++)r.ibs[i]=(struct ggsub_ib){0x61000000+4096*i,64+4*i};
    return r;
}
static unsigned held(void){unsigned n=0;for(unsigned i=0;i<GPR_MAX_LEASES;i++)n+=broker.leases[i].state!=0;return n;}
static void power_boundary_tests(void)
{
    /* Refusal at each pre-call guard must prevent that callback altogether.
     * The first two authority checks precede the guarded power transaction. */
    for(unsigned call=0;call<GGR_COUNT+4;call++) {
        reset();f.deny_authority_at=3+2*call;
        if(call==GGR_COUNT+3)f.observed=0;
        assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_QUARANTINED);
        assert(f.ownership_lost && f.power_calls==call && !f.releases);
        assert(f.runtime.quarantined && held()==GGRT_LEASES);++cases;
    }
    for(unsigned call=0;call<GGR_COUNT+1;call++) {
        ready();unsigned before=f.power_calls;
        f.deny_authority_at=f.authority_checks+2+2*call;
        assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED);
        assert(f.ownership_lost && f.power_calls==before+call);
        assert(f.runtime.quarantined && held()==GGRT_LEASES);++cases;
    }
    ready();assert(!giulia_gpu_runtime_dma(&f.runtime));
    unsigned before=f.power_calls;f.deny_authority_at=f.authority_checks+2;
    assert(giulia_gpu_runtime_drain(&f.runtime)==GGRT_LOST);
    assert(f.ownership_lost && f.power_calls==before && f.runtime.dma);
    assert(f.runtime.quarantined && held()==GGRT_LEASES);++cases;
    /* Loss of platform authority OR any one of the four broker tokens at
     * every provider return boundary: no later I/O, no unauthorized rollback,
     * no DMA retirement or epoch reuse, even when the callback reports success. */
    for(unsigned loss=0;loss<=GGRT_LEASES;loss++) {
        for(unsigned call=1;call<=GGR_COUNT+4;call++) {
            reset();f.loss_kind=loss;f.lose_after_power_call=call;
            if(call==GGR_COUNT+4)f.observed=0; /* include the first poll wait */
            assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_QUARANTINED);
            assert(f.ownership_lost && f.power_calls==call && !f.releases);
            assert(f.runtime.quarantined && f.runtime.power.quarantined);
            assert(!f.runtime.ready && !f.runtime.power.ready && held()==GGRT_LEASES);
            assert(f.runtime.power.held==f.power_held);
            for(unsigned i=0;i<GGR_COUNT;i++)
                assert(f.runtime.power.tokens[i]==((f.power_held&(1u<<i))?100+i:0));
            assert(f.runtime.epoch_open && !giulia_gpu_runtime_live(&f.runtime));
            unsigned before=f.power_calls;
            assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED);
            assert(giulia_gpu_runtime_drain(&f.runtime)==GGRT_LOST);
            assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,78)==GGRT_USED);
            assert(f.power_calls==before);++cases;
        }
        for(unsigned call=1;call<=GGR_COUNT+1;call++) {
            ready();f.loss_kind=loss;unsigned before=f.power_calls;
            f.lose_after_power_call=before+call;
            assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED);
            assert(f.ownership_lost && f.power_calls==before+call);
            assert(f.releases==call-1 && held()==GGRT_LEASES);
            assert(f.runtime.quarantined && !f.runtime.ready && !f.runtime.power.ready);
            assert(f.runtime.epoch_open);++cases;
        }
        ready();assert(!giulia_gpu_runtime_dma(&f.runtime));
        f.loss_kind=loss;unsigned before=f.power_calls;
        f.lose_after_power_call=before+1;
        assert(giulia_gpu_runtime_drain(&f.runtime)==GGRT_LOST);
        assert(f.ownership_lost && f.power_calls==before+1 && !f.releases);
        assert(f.runtime.dma && f.runtime.quarantined && held()==GGRT_LEASES);
        for(unsigned i=1;i<GGRT_LEASES;i++)
            assert(broker.leases[f.runtime.leases[i].slot].dma_active);
        ++cases;
        /* A definite resource failure does not permit rollback if the same
         * callback lost authority. No provider may touch hardware afterwards. */
        for(unsigned resource=1;resource<=GGR_COUNT;resource++) {
            reset();f.loss_kind=loss;f.power_fail=resource;
            f.lose_after_power_call=resource+2;
            assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_QUARANTINED);
            assert(f.ownership_lost && !f.releases && held()==GGRT_LEASES);++cases;
        }
    }
}
static void runtime_tests(void)
{
    reset();f.authority=0;
    assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_AUTHORITY);
    assert(!held() && !f.acquires);++cases;
    /* A conflicting alias or masked SID must stop before power mutation. */
    for(unsigned conflict=0;conflict<3;conflict++) {
        reset();int other;
        struct gpr_key k=conflict==0?(struct gpr_key){GPR_MMIO,0,0,0,0x03d90000,0x1000}:
            (struct gpr_key){GPR_DMA_STREAM,0,conflict==1?0:4,conflict==1?0:1,0x03da0000,0};
        struct gpr_token t;assert(!gpr_open_epoch(&broker,&other,1));
        assert(!gpr_claim(&broker,&k,&other,1,0,0,&t));
        assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_CONFLICT);
        assert(!f.acquires && !f.runtime.held && held()==1);++cases;
    }
    for(unsigned i=1;i<=GGR_COUNT;i++) {
        reset();f.power_fail=i;
        assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_POWER);
        assert(!held() && !f.power_held && !f.runtime.ready);++cases;
        reset();f.power_fail=i;f.power_uncertain=1;
        assert(giulia_gpu_runtime_acquire(&f.runtime,&f.runtime_ops,77)==GGRT_QUARANTINED);
        assert(held()==GGRT_LEASES && !f.releases && !giulia_gpu_runtime_live(&f.runtime));
        assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED);++cases;
    }
    ready();assert(!giulia_gpu_runtime_dma(&f.runtime));f.quiet=0;
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_BUSY);
    assert(giulia_gpu_runtime_drain(&f.runtime)==GGRT_BUSY && !f.releases);
    f.quiet=1;assert(!giulia_gpu_runtime_drain(&f.runtime));
    assert(!giulia_gpu_runtime_release(&f.runtime) && !held() && !f.power_held);++cases;
    ready();f.authority=0;
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_LOST && !f.releases && held()==GGRT_LEASES);++cases;
    ready();assert(!gpr_global_release(&f.runtime.leases[0],&f.runtime,77));
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_LOST && !f.releases);++cases;
    for(unsigned i=1;i<GGRT_LEASES;i++) {
        ready();mark_fail=i;assert(giulia_gpu_runtime_dma(&f.runtime)==GGRT_QUARANTINED);
        assert(held()==GGRT_LEASES && !f.releases);++cases;
    }
    ready();f.power_release_fail=GGR_DVM_CLK+1;
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED && held()==GGRT_LEASES);++cases;
    ready();f.quiet=0;
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED);
    assert(!f.runtime.power.ready && !f.releases);++cases;
    ready();release_fail=1;
    assert(giulia_gpu_runtime_release(&f.runtime)==GGRT_QUARANTINED && f.runtime.held);++cases;
}
static void encoding_tests(void)
{
    uint32_t words[GGSUB_WORDS];
    for(unsigned q=3;q<17;q++)for(unsigned n=1;n<=GGSUB_MAX_IBS;n++) {
        struct ggsub_request r=request(q,n);memset(words,0x55,sizeof(words));
        assert(!giulia_gpu_submit_encode(&r,1,words,GGSUB_WORDS));
        assert(words[0]==((1u<<20)|((8+3*n)<<8)|130));
        assert(words[1]==9 && words[2]==32 && words[3]==1 && words[6]==n);
        assert(!words[4] && !words[5] && !words[7]);
        for(unsigned i=0;i<n;i++)assert(words[8+3*i]==r.ibs[i].address && !words[9+3*i] && words[10+3*i]==r.ibs[i].bytes);
        ++cases;
    }
    for(unsigned bad=0;bad<10;bad++) {
        struct ggsub_request r=request(3,1);
        switch(bad) {
        case 0:r.context=0;break;case 1:r.timestamp=0;break;case 2:r.queue=2;break;
        case 3:r.queue=17;break;case 4:r.count=0;break;case 5:r.count=5;break;
        case 6:r.ibs[0].address=1ull<<32;break;case 7:r.ibs[0].address=0xfffffffc;break;
        case 8:r.ibs[0].bytes=0;break;case 9:r.ibs[0].bytes=3;break;
        }
        memset(words,0x55,sizeof(words));uint32_t before[GGSUB_WORDS];memcpy(before,words,sizeof(words));
        assert(giulia_gpu_submit_encode(&r,1,words,GGSUB_WORDS)==GGSUB_ARGUMENT && !memcmp(words,before,sizeof(words)));++cases;
    }
}
static void submission_tests(void)
{
    for(unsigned q=3;q<17;q++)for(unsigned start=0;start<1024;start+=4)for(unsigned n=1;n<=GGSUB_MAX_IBS;n++) {
        ready();struct ggsub_request r=request(q,n);hdr(q)[10]=hdr(q)[11]=start;
        assert(!giulia_gpu_submit(&f.submit,&r,1000));
        unsigned words=8+3*n,padded=(words+3)&~3u;
        assert(f.rings==1 && !f.fences && !f.submit.completed && f.submit.published);
        assert(hdr(q)[11]==((start+padded)&1023));
        assert(ring_memory(q)[start]==((1u<<20)|(words<<8)|130));
        for(unsigned i=words;i<padded;i++)assert(ring_memory(q)[(start+i)&1023]==0xfafafafa);
        assert(f.memory[GGH_BYTES/4]==0xabcdef);
        assert(giulia_gpu_submit_poll(&f.submit)==GGSUB_PENDING && !f.retires);
        assert(giulia_gpu_submit(&f.submit,&r,1000)==GGSUB_BUSY);
        f.fence_result=1;assert(giulia_gpu_submit_poll(&f.submit)==GGSUB_COMPLETE && f.retires==1);
        assert(f.runtime.dma && !f.releases); /* Fence is not global drain. */
        assert(giulia_gpu_submit(&f.submit,&r,1000)==GGSUB_ARGUMENT);++cases;
    }
    for(unsigned bad=0;bad<14;bad++) {
        ready();struct ggsub_request r=request(3,1);int expected=GGSUB_FAULT;
        switch(bad) {
        case 0:f.proof=0;expected=GGSUB_NOT_READY;break;
        case 1:hdr(3)[10]=4;expected=GGSUB_FULL;break;
        case 2:f.pin_result=1;f.pin_zero=1;expected=GGSUB_PIN;break;
        case 3:f.pin_zero=1;break;
        case 4:f.pin_result=2;break;
        case 5:f.lose_on_pin=1;expected=GGSUB_LOST;break;
        case 6:f.full_on_pin=1;expected=GGSUB_FULL;break;
        case 7:f.corrupt_on_pin=1;break;
        case 8:f.ring_fail=1;break;
        case 9:f.lose_on_ring=1;expected=GGSUB_LOST;break;
        case 10:f.lose_barrier=4;expected=GGSUB_LOST;break;
        case 11:f.full_on_pin=1;f.cancel_fail=1;break;
        case 12:mark_fail=2;expected=GGSUB_LOST;break;
        case 13:f.advance_pin=1001;expected=GGSUB_TIMEOUT;break;
        }
        assert(giulia_gpu_submit(&f.submit,&r,1000)==expected);
        assert(!f.submit.completed && !f.retires);
        if(bad==8 || bad==9 || bad==10)assert(f.submit.published && f.submit.quarantined && f.submit.token);
        else assert(!f.submit.published && !f.rings);
        if(f.submit.quarantined)assert(held()==GGRT_LEASES && !f.releases);
        ++cases;
    }
    for(unsigned bad=0;bad<8;bad++) {
        ready();struct ggsub_request r=request(3,1);assert(!giulia_gpu_submit(&f.submit,&r,1000));
        int expected=GGSUB_FAULT;
        switch(bad) {
        case 0:f.now=1100;expected=GGSUB_TIMEOUT;break;
        case 1:f.now=99;expected=GGSUB_TIMEOUT;break;
        case 2:f.fence_result=2;break;
        case 3:f.fence_result=1;f.lose_on_fence=1;expected=GGSUB_LOST;break;
        case 4:f.fence_result=1;f.retire_fail=1;break;
        case 5:f.authority=0;expected=GGSUB_LOST;break;
        case 6:f.fence_result=1;f.advance_fence=1001;expected=GGSUB_TIMEOUT;break;
        case 7:f.fence_result=1;f.lose_on_retire=1;expected=GGSUB_LOST;break;
        }
        assert(giulia_gpu_submit_poll(&f.submit)==expected);
        assert(f.submit.quarantined && (bad==7?!f.submit.token:!!f.submit.token) && !f.submit.completed && !f.cancel_calls && !f.releases);
        assert(giulia_gpu_submit_poll(&f.submit)==GGSUB_QUARANTINED);++cases;
    }
    ready();struct ggsub_request r=request(3,1);f.submit.sequence=4095;
    assert(giulia_gpu_submit(&f.submit,&r,1000)==GGSUB_EXHAUSTED && !f.pin_calls);++cases;
}
int main(void)
{
    runtime_tests();power_boundary_tests();encoding_tests();submission_tests();
    printf("GPU runtime/submission: %u host-only cases passed; no hardware provider or acceleration tested\n",cases);
    return 0;
}
