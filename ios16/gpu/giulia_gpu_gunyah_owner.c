/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#if !defined(GG_GUNYAH_OWNER_STANDALONE)
#include <pexpert/arm64/board_config.h>
#include <sys/param.h>
#include <sys/systm.h>
#endif
#if defined(GG_GUNYAH_OWNER_STANDALONE) || defined(ARM64_BOARD_CONFIG_SWEET)
#include "giulia_gpu_gunyah_owner.h"

static int ggown_lost(const struct ggown_state *s)
{return __atomic_load_n(&s->loss,__ATOMIC_ACQUIRE)!=0;}
static int ggown_range(const void *p,size_t n)
{return p && n && n<=UINTPTR_MAX-(uintptr_t)p;}
static int ggown_outside(const struct ggown_state *s,const void *p,size_t n)
{
    return ggown_range(s,sizeof(*s)) && ggown_range(p,n) &&
        ((uintptr_t)p+n<=(uintptr_t)s || (uintptr_t)s+sizeof(*s)<=(uintptr_t)p);
}
static int ggown_clock(struct ggown_state *s)
{
    if(ggown_lost(s) || s->time_failed)return 0;
    if(!s->timing)return 1;
    uint64_t now=s->policy.now_us(s->policy.cookie);
    if(ggown_lost(s))return 0;
    if(now<s->last_us || now-s->start_us>=s->timeout_us){s->time_failed=1;return 0;}
    s->last_us=now;return 1;
}
static int ggown_common(struct ggown_state *s)
{
    uint64_t epoch=s->epoch;void *cookie=s->policy.cookie;
    int (*live)(void *,uint64_t)=s->policy.live;
    if(ggown_lost(s) || s->invalid || !epoch || !live ||
       !ggr_report_valid(&s->report) || s->report.epoch!=epoch ||
       s->tx_intid!=s->report.tx_irq[1]+32u || s->rx_intid!=s->report.rx_irq[1]+32u)return 0;
    return live(cookie,epoch)==1 && !ggown_lost(s) && !s->invalid &&
        s->epoch==epoch && s->policy.live==live && s->policy.cookie==cookie &&
        ggown_clock(s) && s->epoch==epoch && s->report.epoch==epoch &&
        ggr_report_valid(&s->report);
}
static int ggown_valid(void *cookie,uint64_t epoch)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    return epoch==s->epoch && s->compatible && s->irq_claimed &&
        s->session.epoch==epoch && s->session.rm.epoch==epoch &&
        !s->session.invalid && !s->session.rm.quarantined &&
        !__atomic_load_n(&s->session.loss_pending,__ATOMIC_ACQUIRE) && ggown_common(s) &&
        giulia_gpu_gunyah_irq_owns(s,epoch,s->tx_intid,s->rx_intid)==1 && ggown_common(s);
}
static void ggown_loss(struct ggown_state *s)
{
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);
    s->invalid=1;s->stage=GGOWN_FAILED;
    (void)ggrs_invalidate(&s->session);
    giulia_gpu_gunyah_irq_invalidate(s,s->epoch);
}
static int ggown_leave(struct ggown_state *s,unsigned result)
{
    /* Recheck worker stop policy and native IRQ ownership at the final
     * successful boundary, after guards have restored the caller's IRQs.
     * A handler registration/stop between the prior check and this exit
     * cannot leave an apparently accepted transport or snapshot result. */
    if((result==GGOWN_OK || result==GGOWN_IDLE || result==GGOWN_BUDGET) &&
       !ggown_valid(s,s->epoch))ggown_loss(s);
    if(ggown_lost(s)) {
        ggown_loss(s);
        if(result==GGOWN_OK || result==GGOWN_IDLE || result==GGOWN_BUDGET)result=GGOWN_LOST;
    }
    s->timing=0;s->last_result=result;
    __atomic_store_n(&s->busy,0u,__ATOMIC_RELEASE);return (int)result;
}
static int ggown_fail(struct ggown_state *s,unsigned result)
{ggown_loss(s);return ggown_leave(s,result);}
static int ggown_enter(struct ggown_state *s)
{
    if(!s)return GGOWN_ARGUMENT;
    unsigned init=__atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE),expected=0;
    if(init!=1)return init==2?GGOWN_BUSY:GGOWN_ARGUMENT;
    return __atomic_compare_exchange_n(&s->busy,&expected,1u,0,__ATOMIC_ACQUIRE,
        __ATOMIC_RELAXED)?GGOWN_OK:GGOWN_BUSY;
}
static int ggown_start(struct ggown_state *s,unsigned timeout_us)
{
    if(!timeout_us || timeout_us>3000000)return GGOWN_ARGUMENT;
    if(ggown_lost(s) || s->invalid || !ggown_common(s))return GGOWN_LOST;
    uint64_t now=s->policy.now_us(s->policy.cookie);
    if(now<s->last_us){s->time_failed=1;return GGOWN_LOST;}
    s->start_us=s->last_us=now;s->timeout_us=timeout_us;s->timing=1;
    return ggown_common(s)?GGOWN_OK:GGOWN_LOST;
}
static int ggown_guard(struct ggown_state *s,unsigned *saved)
{
    if(s->guard_active || !ggown_common(s))return 0;
    if(giulia_gpu_gunyah_irq_guard_begin(s,s->epoch,s->tx_intid,s->rx_intid,saved)!=1)return 0;
    s->guard_active=s->guard_open=1;
    return 1;
}
/* A compatibility probe contains two HVCs. Check the actual CPU IRQ state
 * at every live checkpoint, including between those HVCs. End(saved=0)
 * deliberately keeps IRQs disabled, then begin revalidates the reservation.
 * There is no scheduling window or sleep. The outer end alone restores the
 * original caller state, including after a failed checkpoint. */
static int ggown_checkpoint(struct ggown_state *s)
{
    if(!s->guard_active || !s->guard_open)return 0;
    int valid=giulia_gpu_gunyah_irq_guard_end(s,s->epoch,s->tx_intid,s->rx_intid,0);
    s->guard_open=0;
    if(valid!=1){(void)ggown_invalidate(s);return 0;}
    unsigned saved=1;
    if(giulia_gpu_gunyah_irq_guard_begin(s,s->epoch,s->tx_intid,s->rx_intid,&saved)!=1) {
        (void)ggown_invalidate(s);return 0;
    }
    s->guard_open=1;
    if(saved){(void)ggown_invalidate(s);return 0;}
    return 1;
}
static int ggown_unguard(struct ggown_state *s,unsigned saved)
{
    /* Even if a checkpoint invalidated the reservation, native end restores
     * the original IRQ state after performing its fail-closed check. */
    int valid=giulia_gpu_gunyah_irq_guard_end(s,s->epoch,s->tx_intid,s->rx_intid,saved);
    s->guard_active=s->guard_open=0;
    return valid==1 && ggown_common(s);
}
static int ggown_probe_live(void *cookie,uint64_t epoch)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    return s->guard_active==1 && epoch==s->epoch && ggown_common(s) &&
        giulia_gpu_gunyah_irq_owns(s,epoch,s->tx_intid,s->rx_intid)==1 &&
        ggown_common(s) && ggown_checkpoint(s);
}
static int ggown_hvc_live(void *cookie,uint64_t epoch,uint64_t tx,uint64_t rx)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    return s->guard_active==1 && tx==s->report.tx_cap && rx==s->report.rx_cap &&
        ggown_valid(s,epoch) && ggown_checkpoint(s);
}
static int ggown_send(void *cookie,const void *p,size_t n,unsigned flags)
{
    struct ggown_state *s=(struct ggown_state *)cookie;unsigned saved=0;
    if(!ggown_valid(s,s->epoch) || !ggown_guard(s,&saved))return -1;
    int result=ggh_hvc_send(&s->hvc,p,n,flags);
    if(s->hvc.attempted)++s->hvc_calls;
    int valid=ggown_unguard(s,saved);
    return valid && ggown_valid(s,s->epoch)?result:-1;
}
static int ggown_receive(void *cookie,void *p,size_t capacity,size_t *bytes)
{
    struct ggown_state *s=(struct ggown_state *)cookie;unsigned saved=0;
    uint8_t scratch[GGH_HVC_MAX_BYTES];size_t received=0;
    if(!p || !bytes || !capacity || capacity>sizeof(scratch) ||
       !ggown_valid(s,s->epoch) || !ggown_guard(s,&saved))return -1;
    int result=ggh_hvc_recv(&s->hvc,scratch,capacity,&received);
    if(s->hvc.attempted)++s->hvc_calls;
    int valid=ggown_unguard(s,saved);
    if(!valid || !ggown_valid(s,s->epoch))return -1;
    if(!result) {
        for(size_t i=0;i<received;i++)((uint8_t *)p)[i]=scratch[i];
        *bytes=received;
    }
    return result;
}
static uint64_t ggown_now(void *cookie)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    if(s->timing){(void)ggown_clock(s);return s->last_us;}
    uint64_t now=s->policy.now_us(s->policy.cookie);
    if(now<s->last_us)s->time_failed=1;else s->last_us=now;
    return now;
}
static int ggown_delay(void *cookie,unsigned us)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    if(s->guard_active || !ggown_valid(s,s->epoch) ||
       (s->timing && us>=s->timeout_us-(unsigned)(s->last_us-s->start_us)))return -1;
    int result=s->policy.delay_us(s->policy.cookie,us);
    return ggown_valid(s,s->epoch)?result:-1;
}
static int ggown_notification(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    struct ggown_state *s=(struct ggown_state *)cookie;
    if(s->guard_active || !ggown_valid(s,s->epoch))return 0;
    int result=s->policy.notification(s->policy.cookie,id,p,bytes);
    return ggown_valid(s,s->epoch)?result:0;
}
static int ggown_boot(struct ggown_state *s)
{
    if(s->bootstrapped)return GGOWN_OK;
    if(s->attempted)return GGOWN_USED;
    s->attempted=1;s->stage=GGOWN_CLAIM;
    if(!ggown_common(s) || giulia_gpu_gunyah_irq_claim(s,s->epoch,s->tx_intid,s->rx_intid)!=1)
        return GGOWN_IRQ;
    s->irq_claimed=1;s->stage=GGOWN_COMPATIBILITY;
    unsigned saved=0;
    if(!ggown_guard(s,&saved))return GGOWN_IRQ;
    struct ggh_hvc_compatibility compatibility;
    s->probe_result=(unsigned)ggh_hvc_probe_compatibility(&s->compatibility_probe,&compatibility);
    s->hvc_calls+=s->compatibility_probe.attempted;
    int guarded=ggown_unguard(s,saved);
    if(!guarded || s->probe_result!=GGH_PROBE_OK)return GGOWN_HYPERVISOR;
    s->compatibility=compatibility;s->compatible=1;
    s->hvc.epoch=s->epoch;s->hvc.tx_cap=s->report.tx_cap;s->hvc.rx_cap=s->report.rx_cap;
    s->hvc.lease_cookie=s;s->hvc.live=ggown_hvc_live;
    struct ggrm_io io={s,ggown_valid,ggown_send,ggown_receive,ggown_now,ggown_delay,ggown_notification};
    s->io=io;
    s->session_result=(unsigned)ggrs_init(&s->session,&io,s->epoch);
    if(s->session_result!=GGRS_OK || !ggown_valid(s,s->epoch))return GGOWN_SESSION;
    s->bootstrapped=1;s->stage=GGOWN_CHANNEL;return GGOWN_OK;
}
int ggown_init(struct ggown_state *s,const struct ggr_report *report,const struct ggown_policy *policy)
{
    if(!ggown_range(s,sizeof(*s)) || (uintptr_t)s%__alignof__(*s) ||
       !ggown_outside(s,report,sizeof(*report)) || (uintptr_t)report%__alignof__(*report) ||
       !ggown_outside(s,policy,sizeof(*policy)) || (uintptr_t)policy%__alignof__(*policy) ||
       !ggr_report_valid(report) || !policy->live || !policy->now_us ||
       !policy->delay_us || !policy->notification)return GGOWN_ARGUMENT;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&s->initialized,&expected,2u,0,__ATOMIC_ACQUIRE,
            __ATOMIC_RELAXED))return expected==2?GGOWN_BUSY:GGOWN_USED;
    if(s->epoch || s->busy || s->attempted || s->invalid || ggown_lost(s)) {
        __atomic_store_n(&s->initialized,0u,__ATOMIC_RELEASE);return GGOWN_ARGUMENT;
    }
    s->report=*report;s->policy=*policy;s->epoch=report->epoch;
    s->tx_intid=report->tx_irq[1]+32u;s->rx_intid=report->rx_irq[1]+32u;
    s->compatibility_probe.epoch=s->epoch;s->compatibility_probe.lease_cookie=s;
    s->compatibility_probe.live=ggown_probe_live;s->stage=GGOWN_CONFIGURED;
    unsigned result=GGOWN_OK;
    if(!ggown_common(s)){ggown_loss(s);result=GGOWN_LOST;}
    s->last_result=result;
    __atomic_store_n(&s->initialized,1u,__ATOMIC_RELEASE);return (int)result;
}
int ggown_bootstrap(struct ggown_state *s,unsigned timeout_us)
{
    int result=ggown_enter(s);if(result)return result;
    if(s->bootstrapped)return ggown_leave(s,GGOWN_USED);
    result=ggown_start(s,timeout_us);
    if(result==GGOWN_ARGUMENT)return ggown_leave(s,(unsigned)result);
    if(result)return ggown_fail(s,(unsigned)result);
    result=ggown_boot(s);
    return result?ggown_fail(s,(unsigned)result):ggown_leave(s,GGOWN_OK);
}
int ggown_query_self(struct ggown_state *s,unsigned timeout_us)
{
    int result=ggown_enter(s);if(result)return result;
    if(s->self_attempted || s->session.attempted || s->session.rm.busy)
        return ggown_leave(s,ggown_lost(s)?GGOWN_LOST:GGOWN_USED);
    result=ggown_start(s,timeout_us);
    if(result==GGOWN_ARGUMENT)return ggown_leave(s,(unsigned)result);
    if(result)return ggown_fail(s,(unsigned)result);
    s->self_attempted=1;
    result=ggown_boot(s);if(result)return ggown_fail(s,(unsigned)result);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    s->self_rpc_result=(unsigned)ggrm_call(&s->io,&s->session.rm,
        GGRQ_VM_GET_VMID,NULL,0,timeout_us);
    s->self_remote_error=s->session.rm.reply.error;
    if(s->self_rpc_result!=GGRM_OK)return ggown_fail(s,GGOWN_SESSION);
    uint16_t vmid=0;
    if(gg_rq_parse_self(s->session.rm.reply.data,s->session.rm.reply.bytes,&vmid))
        return ggown_fail(s,GGOWN_SESSION);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    s->self_vmid=vmid;s->self_observed=1;
    return ggown_leave(s,GGOWN_OK);
}
int ggown_probe(struct ggown_state *s,unsigned timeout_us)
{
    int result=ggown_enter(s);if(result)return result;
    if(s->session.attempted)return ggown_leave(s,ggown_lost(s)?GGOWN_LOST:GGOWN_USED);
    result=ggown_start(s,timeout_us);
    if(result==GGOWN_ARGUMENT)return ggown_leave(s,(unsigned)result);
    if(result)return ggown_fail(s,(unsigned)result);
    result=ggown_boot(s);if(result)return ggown_fail(s,(unsigned)result);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    s->stage=GGOWN_DISCOVERY;
    s->session_result=(unsigned)ggrs_probe(&s->session,
        s->timeout_us-(unsigned)(s->last_us-s->start_us));
    if(s->session_result!=GGRS_OK)return ggown_fail(s,GGOWN_SESSION);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    s->stage=GGOWN_READY;return ggown_leave(s,GGOWN_OK);
}
int ggown_poll(struct ggown_state *s,unsigned frames,unsigned timeout_us)
{
    int result=ggown_enter(s);if(result)return result;
    if(!frames || frames>GGRS_MAX_POLL_FRAMES || !timeout_us || timeout_us>3000000)
        return ggown_leave(s,GGOWN_ARGUMENT);
    if(!s->bootstrapped || s->stage!=GGOWN_READY)return ggown_leave(s,GGOWN_LOST);
    result=ggown_start(s,timeout_us);if(result)return ggown_fail(s,(unsigned)result);
    s->session_result=(unsigned)ggrs_poll(&s->session,frames,timeout_us);
    if(s->session_result!=GGRS_IDLE && s->session_result!=GGRS_BUDGET)
        return ggown_fail(s,GGOWN_SESSION);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    return ggown_leave(s,s->session_result==GGRS_IDLE?GGOWN_IDLE:GGOWN_BUDGET);
}
int ggown_copy_snapshot(struct ggown_state *s,struct gg_rq_snapshot *out)
{
    int result=ggown_enter(s);if(result)return result;
    if(!ggown_outside(s,out,sizeof(*out)) || (uintptr_t)out%__alignof__(*out))
        return ggown_leave(s,GGOWN_ARGUMENT);
    if(!s->bootstrapped || !ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    struct gg_rq_snapshot snapshot;
    s->session_result=(unsigned)ggrs_copy_snapshot(&s->session,&snapshot);
    if(s->session_result!=GGRS_OK)return ggown_leave(s,GGOWN_SESSION);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    result=ggown_leave(s,GGOWN_OK);if(!result)*out=snapshot;return result;
}
int ggown_get_status(struct ggown_state *s,struct ggown_status *out)
{
    int result=ggown_enter(s);if(result)return result;
    if(!ggown_outside(s,out,sizeof(*out)) || (uintptr_t)out%__alignof__(*out)) {
        __atomic_store_n(&s->busy,0u,__ATOMIC_RELEASE);return GGOWN_ARGUMENT;
    }
    uint64_t native_code=s->hvc_calls>s->compatibility_probe.attempted?
        s->hvc.native_code:s->compatibility_probe.last.x[0];
    struct ggown_status status={s->initialized,0,s->bootstrapped,s->compatible,s->irq_claimed,
        s->invalid || ggown_lost(s) || s->session.invalid || s->session.rm.quarantined ||
        __atomic_load_n(&s->session.loss_pending,__ATOMIC_ACQUIRE),
        s->stage,s->last_result,s->probe_result,s->session_result,
        s->session.snapshot.step,s->session.rm.next_sequence,s->session.rm.quarantined,
        s->hvc_calls,s->epoch,native_code,s->compatibility.identify.x[0],
        s->self_attempted,s->self_observed,s->self_vmid,s->self_rpc_result,s->self_remote_error};
    __atomic_store_n(&s->busy,0u,__ATOMIC_RELEASE);*out=status;return GGOWN_OK;
}
int ggown_get_channel(struct ggown_state *s,struct ggrm_io *out,struct ggrm_state **channel)
{
    int result=ggown_enter(s);if(result)return result;
    if(!ggown_outside(s,out,sizeof(*out)) || (uintptr_t)out%__alignof__(*out) ||
       !ggown_outside(s,channel,sizeof(*channel)) || (uintptr_t)channel%__alignof__(*channel) ||
       ((uintptr_t)out<(uintptr_t)channel+sizeof(*channel) && (uintptr_t)channel<(uintptr_t)out+sizeof(*out)))
        return ggown_leave(s,GGOWN_ARGUMENT);
    if(!s->bootstrapped || s->session.attempted || s->session.rm.busy || s->session.rm.quarantined)
        return ggown_leave(s,GGOWN_USED);
    if(!ggown_valid(s,s->epoch))return ggown_fail(s,GGOWN_LOST);
    struct ggrm_io io=s->io;struct ggrm_state *rm=&s->session.rm;
    result=ggown_leave(s,GGOWN_OK);if(!result){*out=io;*channel=rm;}return result;
}
int ggown_invalidate(struct ggown_state *s)
{
    if(!s)return GGOWN_ARGUMENT;
    unsigned init=__atomic_load_n(&s->initialized,__ATOMIC_ACQUIRE);
    if(init!=1 && init!=2)return GGOWN_ARGUMENT;
    __atomic_store_n(&s->loss,1u,__ATOMIC_RELEASE);
    if(init==2)return GGOWN_LOST;
    giulia_gpu_gunyah_irq_invalidate(s,s->epoch);
    (void)ggrs_invalidate(&s->session);
    unsigned expected=0;
    if(__atomic_compare_exchange_n(&s->busy,&expected,1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        ggown_loss(s);return ggown_leave(s,GGOWN_LOST);
    }
    return GGOWN_LOST;
}
#endif
