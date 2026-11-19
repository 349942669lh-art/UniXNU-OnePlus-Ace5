/* Actual owner/HVC/RM/session source with platform/HVC instruction mocks.
 * No device reads, HVCs, IRQ operations, assignments or firmware are used. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_gunyah_owner.h"

static const uint32_t ids[]={GGRQ_VM_GET_VMID,GGRQ_VM_LOOKUP_NAME,GGRQ_VM_GET_ID,
    GGRQ_VM_GET_STATE,GGRQ_MEM_LOOKUP,GGRQ_VM_GET_RESOURCES,GGRQ_VM_GET_STATE};
struct fixture {
    struct ggown_state *s;
    struct ggr_report report;
    struct ggown_policy policy;
    uint64_t tick,epoch;
    uint32_t tx,rx;
    unsigned worker,stopping,irq_enabled,claimed,irq_live,guard;
    unsigned claims,owns,begins,ends,invalidates,hvcs,uids,identifies,sends,receives;
    unsigned queries,pending,seq,full,empty,delays,notifications;
    unsigned claim_fail,begin_fail_at,end_fail_at,unexpected_irq_at,end_fail_hvc_at,begin_fail_after_hvcs;
    unsigned uid_fail,api_fail,bad_step,bad_status,transport_step,wrong_seq,missing_parcel;
    unsigned loss_hvc_at,stop_hvc_at,late_hvc_at,late_stage,rewind_stage;
    unsigned notify_step,notified,poll_left,poll_peer,reenter,late_delay,stop_delay;
    unsigned external,pending_external,external_calls;
    unsigned last_rx,self_reply_bad;
    unsigned live_checks,loss_live_at,init_reenter,stop_notification,delay_error,stop_ready;
    uint8_t before_rx[240];
};
static struct fixture f;
static uint16_t u16(const uint8_t *p){return (uint16_t)(p[0]|(unsigned)p[1]<<8);}
static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t x)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static void platform_args(const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx)
{assert(owner==f.s && epoch==f.epoch && tx==f.tx && rx==f.rx);}
int giulia_gpu_gunyah_irq_claim(const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx)
{
    platform_args(owner,epoch,tx,rx);++f.claims;
    assert(!f.guard && !f.claimed && !f.hvcs);
    if(f.claim_fail)return 0;
    f.claimed=f.irq_live=1;return 1;
}
int giulia_gpu_gunyah_irq_owns(const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx)
{
    platform_args(owner,epoch,tx,rx);++f.owns;
    return f.claimed && f.irq_live;
}
void giulia_gpu_gunyah_irq_invalidate(const void *owner,uint64_t epoch)
{
    assert(owner==f.s && epoch==f.epoch);++f.invalidates;f.irq_live=0;
}
int giulia_gpu_gunyah_irq_guard_begin(const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx,unsigned *saved)
{
    platform_args(owner,epoch,tx,rx);++f.begins;assert(saved && !f.guard);
    if(!f.irq_live || f.begin_fail_at==f.begins ||
       (f.begin_fail_after_hvcs && f.hvcs==f.begin_fail_after_hvcs))return 0;
    *saved=f.irq_enabled;f.irq_enabled=0;f.guard=1;return 1;
}
int giulia_gpu_gunyah_irq_guard_end(const void *owner,uint64_t epoch,uint32_t tx,uint32_t rx,unsigned saved)
{
    platform_args(owner,epoch,tx,rx);
    assert((f.guard || (!f.irq_live && !f.irq_enabled)) && saved<=1);++f.ends;
    if(f.last_rx) {
        assert(!memcmp(f.before_rx,f.s->session.rm.rx,sizeof(f.before_rx)));
        f.last_rx=0;
    }
    if(f.ends==1) {
        struct ggh_hvc_compatibility zero={0};
        assert(!f.s->compatible && !memcmp(&f.s->compatibility,&zero,sizeof(zero)));
    }
    int valid=f.irq_live && !f.irq_enabled && f.end_fail_at!=f.ends &&
        (!f.end_fail_hvc_at || f.hvcs!=f.end_fail_hvc_at);
    if(!valid)f.irq_live=0;
    f.irq_enabled=saved;f.guard=0;return valid;
}
int giulia_gpu_gunyah_irq_dispatch(uint32_t intid)
{if(f.claimed && (intid==f.tx || intid==f.rx)){f.irq_live=0;return 1;}return 0;}
void giulia_gpu_gunyah_irq_handler_registered(void){f.irq_live=0;}
void giulia_gpu_gunyah_irq_controller_changed(void){f.irq_live=0;}

static int live(void *cookie,uint64_t epoch)
{
    assert(cookie==&f && epoch==f.epoch);++f.live_checks;
    if(f.init_reenter && f.s->initialized==2) {
        struct ggown_status status;
        assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_BUSY);
        assert(ggown_probe(f.s,100000)==GGOWN_BUSY);
        assert(ggown_get_status(f.s,&status)==GGOWN_BUSY);
    }
    if(f.loss_live_at==f.live_checks)assert(ggown_invalidate(f.s)==GGOWN_LOST);
    if(f.stop_ready && f.s->stage==GGOWN_READY)f.stopping=1;
    return f.worker && !f.stopping;
}
static uint64_t now(void *cookie)
{
    assert(cookie==&f);
    if(f.s->timing && f.late_stage && f.s->stage==f.late_stage)
        f.tick=f.s->start_us+f.s->timeout_us;
    if(f.s->timing && f.rewind_stage && f.s->stage==f.rewind_stage) {
        f.rewind_stage=0;return f.s->last_us-1;
    }
    return f.tick++;
}
static int delay(void *cookie,unsigned us)
{
    assert(cookie==&f && !f.guard && !f.s->guard_active);++f.delays;f.tick+=us;
    if(f.late_delay)f.tick=f.s->start_us+f.s->timeout_us;
    if(f.stop_delay){f.stopping=1;assert(ggown_invalidate(f.s)==GGOWN_LOST);}
    return f.delay_error?-1:0;
}
static int notification(void *cookie,uint32_t id,const void *data,size_t bytes)
{
    assert(cookie==&f && !f.guard && !f.s->guard_active && id==GGRQ_VM_STATUS && data && bytes==8);
    ++f.notifications;
    if(f.stop_notification){f.stopping=1;assert(ggown_invalidate(f.s)==GGOWN_LOST);}
    return 1;
}
static void report_init(void)
{
    ggr_report_init(&f.report);f.report.source=GGR_SOURCE_BOOT_FDT;f.report.fdt_bytes=4096;
    f.report.fdt_crc32=0x01234567;f.report.epoch=f.epoch=UINT64_C(0x123456789abcdef0);
    f.report.tx_cap=UINT64_C(0x09c762828ef31568);f.report.rx_cap=UINT64_C(0x09c762828ef3445a);
    f.report.rm_node=256;f.report.gic_node=128;f.report.interrupt_parent=1;f.report.interrupt_cells=3;
    f.report.tx_irq[1]=0x3a0;f.report.rx_irq[1]=0x3a1;
    f.report.tx_irq[2]=f.report.rx_irq[2]=1;
    f.tx=f.report.tx_irq[1]+32;f.rx=f.report.rx_irq[1]+32;ggr_report_seal(&f.report);
    assert(ggr_report_valid(&f.report));
}
static void setup(void)
{
    memset(&f,0,sizeof(f));f.s=calloc(1,sizeof(*f.s));assert(f.s);
    f.worker=f.irq_enabled=1;report_init();
    struct ggown_policy policy={&f,live,now,delay,notification};f.policy=policy;
}
static void start(void)
{setup();assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_OK);}
static void cleanup(void){free(f.s);}
static void guarded_hvc(void)
{
    assert(f.guard && f.s->guard_active && !f.irq_enabled && f.claimed && f.irq_live);
    ++f.hvcs;
    if(f.reenter) {
        assert(ggown_bootstrap(f.s,100000)==GGOWN_BUSY);
        assert(ggown_probe(f.s,100000)==GGOWN_BUSY);
        assert(ggown_poll(f.s,1,100000)==GGOWN_BUSY);
        struct gg_rq_snapshot out;
        assert(ggown_copy_snapshot(f.s,&out)==GGOWN_BUSY);
    }
}
static void hvc_faults(void)
{
    if(f.loss_hvc_at==f.hvcs)assert(ggown_invalidate(f.s)==GGOWN_LOST);
    if(f.stop_hvc_at==f.hvcs)f.stopping=1;
    if(f.late_hvc_at==f.hvcs)f.tick=f.s->start_us+f.s->timeout_us;
    if(f.unexpected_irq_at==f.hvcs)f.irq_enabled=1;
}
void ggh_hvc_test_probe_invoke(struct ggh_hvc_regs *r)
{
    guarded_hvc();for(unsigned i=1;i<8;i++)assert(r->x[i]==0);
    if(r->x[0]==UINT64_C(0x8600ff01)) {
        ++f.uids;assert(!f.identifies);
        r->x[0]=UINT64_C(0x12345678cd8fd5c1);r->x[1]=0xdb5f53a4;
        r->x[2]=0x36ce6592;r->x[3]=0x145f3d67;
        if(f.uid_fail)r->x[0]^=1;
    } else {
        assert(r->x[0]==UINT64_C(0xc6008000) && f.uids==1);++f.identifies;
        r->x[0]=f.api_fail?UINT64_C(0x100008001):UINT64_C(0xab00000000008001);
        r->x[1]=UINT64_C(0x123456789abcdef0);r->x[2]=UINT64_MAX;r->x[3]=0x55;
    }
    hvc_faults();
}
void ggh_hvc_test_doorbell_invoke(struct ggh_hvc_regs *r)
{(void)r;assert(0);}
static void send_payload(const uint8_t *p,size_t n,unsigned step)
{
    if(step==0)assert(n==8);
    else if(step==1) {
        const uint8_t name[]={13,0,0,0,'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        assert(n==28 && !memcmp(p+8,name,sizeof(name)));
    } else if(step==4) {
        assert(n==52 && u32(p+12)==5 && u16(p+20)==0x45 && u16(p+24)==0x73);
        assert(u32(p+32)==0xd4e23000 && u32(p+40)==0x2dd000);
    } else assert(n==12 && u32(p+8)==(step==5?0:0x73));
}
static size_t receive_payload(uint8_t *p)
{
    memset(p,0,240);p[0]=0x21;p[1]=2;p[2]=(uint8_t)f.seq;p[3]=(uint8_t)(f.seq>>8);
    if(!f.pending) {
        assert(f.poll_left);--f.poll_left;p[1]=3;put32(p+4,GGRQ_VM_STATUS);
        p[8]=(uint8_t)(f.poll_peer?0x73:0x74);p[12]=2;return 16;
    }
    unsigned step=f.pending_external?1:f.queries;
    if(f.notify_step==step && !f.notified++) {
        p[1]=3;put32(p+4,GGRQ_VM_STATUS);p[8]=0x74;p[12]=2;return 16;
    }
    put32(p+4,f.pending_external?GGRQ_VM_GET_VMID:ids[step-1]);size_t n=16;
    switch(step) {
    case 1:
        p[12]=0x45;
        if(f.self_reply_bad==1)p[12]=0;
        if(f.self_reply_bad==2)p[14]=1;
        if(f.self_reply_bad==3)put32(p+8,14);
        if(f.self_reply_bad==4)n=15;
        break;
    case 2:p[12]=1;p[16]=0x73;n=20;break;
    case 3: {
        const uint8_t identity[]={1,0,0,0,2,0,13,0,
            'q','c','o','m',',','c','p','u','s','y','s','v','m',0,0,0};
        memcpy(p+12,identity,sizeof(identity));n=36;break;
    }
    case 4:case 7:p[12]=2;p[13]=4;break;
    case 5:put32(p+12,0x98765432);if(f.missing_parcel)put32(p+8,14);break;
    case 6:
        put32(p+12,1);p[16]=1;p[18]=0x73;put32(p+20,0x5678);put32(p+24,6);
        put32(p+28,0x9abcdef0);put32(p+32,0x12345678);put32(p+36,0x9876);put32(p+40,456);n=60;break;
    default:assert(0);
    }
    if(f.wrong_seq && step==f.wrong_seq)p[2]^=1;
    f.pending=f.pending_external=0;return n;
}
void ggh_hvc_test_invoke(struct ggh_hvc_regs *r)
{
    guarded_hvc();assert(f.s->compatible && f.s->bootstrapped);
    assert(!r->x[5] && !r->x[6] && !r->x[7]);
    if(r->x[0]==UINT64_C(0xc600801b)) {
        ++f.sends;assert(r->x[1]==f.report.tx_cap && r->x[4]==1);
        if(f.full){--f.full;r->x[0]=61;hvc_faults();return;}
        const uint8_t *p=(const uint8_t *)(uintptr_t)r->x[3];size_t n=(size_t)r->x[2];
        assert(p[0]==0x21 && p[1]==1 && !f.pending);
        f.seq=u16(p+2);
        if(f.external) {
            assert(u32(p+4)==GGRQ_VM_GET_VMID && n==8);f.pending_external=1;++f.external_calls;
        } else {
            assert(f.queries<7 && u32(p+4)==ids[f.queries]);
            assert(f.seq==f.queries+f.external_calls);send_payload(p,n,f.queries);++f.queries;
        }
        f.pending=1;r->x[0]=0;r->x[1]=0; /* Accepted final queue slot. */
    } else {
        ++f.receives;assert(r->x[0]==UINT64_C(0xc600801c) && r->x[1]==f.report.rx_cap);
        assert(r->x[3]==240 && !r->x[4]);
        memcpy(f.before_rx,f.s->session.rm.rx,sizeof(f.before_rx));f.last_rx=1;
        if(f.empty || (!f.pending && !f.poll_left)){r->x[0]=60;hvc_faults();return;}
        size_t n=receive_payload((uint8_t *)(uintptr_t)r->x[2]);r->x[0]=0;r->x[1]=n;r->x[2]=0;
        if(f.bad_step && f.queries==f.bad_step)r->x[0]=f.bad_status?UINT64_C(0x100000000):5;
        if(f.transport_step && f.queries==f.transport_step)r->x[1]=241;
    }
    hvc_faults();
}
static void failed(void)
{
    struct ggown_status status;assert(ggown_get_status(f.s,&status)==GGOWN_OK);
    assert(status.invalid && !f.guard && !f.s->guard_active && f.irq_enabled && !f.irq_live);
    struct gg_rq_snapshot out,saved;memset(&out,0xa5,sizeof(out));saved=out;
    assert(ggown_copy_snapshot(f.s,&out)!=GGOWN_OK && !memcmp(&out,&saved,sizeof(out)));
    unsigned calls=f.hvcs;
    assert(ggown_probe(f.s,100000)!=GGOWN_OK && ggown_bootstrap(f.s,100000)!=GGOWN_OK);
    assert(f.hvcs==calls);
}
static void normal_and_poll(void)
{
    start();struct ggown_status status;
    assert(ggown_get_status(f.s,&status)==GGOWN_OK && status.stage==GGOWN_CONFIGURED);
    assert(!status.compatible && !status.irq_claimed && !f.hvcs && !f.claims);
    /* Freeze descriptors. The worker policy still reads its actual stop bit. */
    struct ggr_report saved_report=f.report;
    f.policy.live=NULL;f.policy.now_us=NULL;
    f.reenter=1;f.full=1;f.notify_step=4;
    assert(ggown_probe(f.s,1000000)==GGOWN_OK);
    assert(f.uids==1 && f.identifies==1 && f.queries==7 && f.delays==1 && f.notifications==1);
    assert(f.begins==f.ends && !f.guard && f.irq_enabled && f.claims==1);
    assert(ggown_get_status(f.s,&status)==GGOWN_OK && status.stage==GGOWN_READY);
    assert(status.compatible && status.bootstrapped && status.irq_claimed && !status.invalid);
    assert(status.epoch==f.epoch && status.api_info==UINT64_C(0xab00000000008001));
    assert(status.next_sequence==7 && status.hvc_calls==f.hvcs && !status.rm_quarantined);
    struct gg_rq_snapshot out;
    assert(ggown_copy_snapshot(f.s,&out)==GGOWN_OK && gg_rq_snapshot_complete(&out));
    assert(out.epoch==saved_report.epoch && out.self_vmid==0x45 && out.peer_vmid==0x73);
    assert(out.parcel_handle==0x98765432 && out.doorbell.capid==UINT64_C(0x123456789abcdef0));
    unsigned sent=f.sends;
    assert(ggown_probe(f.s,100000)==GGOWN_USED && f.sends==sent);
    assert(ggown_poll(f.s,4,100000)==GGOWN_IDLE && f.sends==sent);
    f.poll_left=2;assert(ggown_poll(f.s,2,100000)==GGOWN_BUDGET && f.sends==sent);
    assert(ggown_poll(f.s,2,100000)==GGOWN_IDLE && f.sends==sent);
    assert(ggown_get_status(f.s,&status)==GGOWN_OK && status.next_sequence==7);
    f.poll_left=f.poll_peer=1;assert(ggown_poll(f.s,2,100000)!=GGOWN_OK);
    failed();cleanup();
}
static void bootstrap_and_borrow(void)
{
    start();assert(ggown_bootstrap(f.s,100000)==GGOWN_OK);
    assert(f.hvcs==2 && !f.queries && f.claims==1 && f.s->session.rm.next_sequence==0);
    struct ggrm_io io;struct ggrm_state *channel=NULL;
    assert(ggown_get_channel(f.s,&io,&channel)==GGOWN_OK && channel==&f.s->session.rm);
    f.external=1;assert(ggrm_call(&io,channel,GGRQ_VM_GET_VMID,NULL,0,100000)==GGRM_OK);
    f.external=0;assert(channel->next_sequence==1 && !f.queries);
    assert(ggown_probe(f.s,1000000)==GGOWN_OK && f.hvcs==18 && channel->next_sequence==8);
    assert(ggown_get_channel(f.s,&io,&channel)==GGOWN_USED);
    assert(ggown_invalidate(f.s)==GGOWN_LOST);failed();cleanup();
    start();assert(ggown_bootstrap(f.s,100000)==GGOWN_OK);
    giulia_gpu_gunyah_irq_handler_registered();
    assert(ggown_probe(f.s,1000000)!=GGOWN_OK && !f.queries);failed();cleanup();
    start();assert(ggown_bootstrap(f.s,100000)==GGOWN_OK);f.tick=0;
    assert(ggown_probe(f.s,1000000)!=GGOWN_OK && !f.queries && f.hvcs==2);failed();cleanup();
    start();assert(ggown_bootstrap(f.s,100000)==GGOWN_OK);
    assert(ggown_get_channel(f.s,&io,&channel)==GGOWN_OK);channel->quarantined=1;
    struct ggown_status status;
    assert(ggown_get_status(f.s,&status)==GGOWN_OK && status.invalid);
    uint8_t frame[8]={0x21,1,0,0,0x24,0,0,0x56};
    assert(!io.valid(io.cookie,f.epoch) && io.send(io.cookie,frame,sizeof(frame),1)<0 && f.hvcs==2);
    assert(ggown_probe(f.s,100000)!=GGOWN_OK && f.hvcs==2);failed();cleanup();
}
static void self_identity(void)
{
    start();f.external=1;
    assert(ggown_query_self(f.s,0)==GGOWN_ARGUMENT && !f.hvcs && !f.s->self_attempted);
    assert(ggown_query_self(f.s,1000000)==GGOWN_OK);
    assert(f.hvcs==4 && f.external_calls==1 && !f.queries);
    assert(f.s->session.rm.next_sequence==1 && !f.s->session.attempted);
    struct ggown_status status;
    assert(ggown_get_status(f.s,&status)==GGOWN_OK);
    assert(status.self_attempted && status.self_observed && status.self_vmid==0x45);
    assert(!status.self_rpc_result && !status.self_remote_error && !status.invalid);
    unsigned calls=f.hvcs;
    assert(ggown_query_self(f.s,1000000)==GGOWN_USED && f.hvcs==calls);
    f.external=0;
    assert(ggown_probe(f.s,1000000)==GGOWN_OK && f.queries==7);
    assert(f.s->session.rm.next_sequence==8);
    assert(ggown_invalidate(f.s)==GGOWN_LOST);
    assert(ggown_get_status(f.s,&status)==GGOWN_OK && status.invalid);
    assert(status.self_observed && status.self_vmid==0x45);
    failed();cleanup();
    for(unsigned mode=1;mode<=4;mode++) {
        start();f.external=1;f.self_reply_bad=mode;
        assert(ggown_query_self(f.s,1000000)==GGOWN_SESSION);
        assert(ggown_get_status(f.s,&status)==GGOWN_OK);
        assert(status.self_attempted && !status.self_observed && !status.self_vmid);
        if(mode==3)assert(status.self_rpc_result==GGRM_REMOTE && status.self_remote_error==14);
        calls=f.hvcs;
        assert(ggown_query_self(f.s,1000000)!=GGOWN_OK && f.hvcs==calls);
        failed();cleanup();
    }
    for(unsigned at=1;at<=4;at++)for(unsigned mode=0;mode<3;mode++) {
        start();f.external=1;
        if(mode==0)f.loss_hvc_at=at;
        if(mode==1)f.stop_hvc_at=at;
        if(mode==2)f.late_hvc_at=at;
        assert(ggown_query_self(f.s,1000000)!=GGOWN_OK && f.hvcs==at);
        assert(!f.s->self_observed && !f.s->self_vmid);
        failed();cleanup();
    }
    start();assert(ggown_probe(f.s,1000000)==GGOWN_OK);
    calls=f.hvcs;
    assert(ggown_query_self(f.s,1000000)==GGOWN_USED && f.hvcs==calls);
    cleanup();
}
static void boot_and_guard_failures(void)
{
    for(unsigned mode=0;mode<5;mode++) {
        start();if(mode==0)f.claim_fail=1;if(mode==1)f.uid_fail=1;if(mode==2)f.api_fail=1;
        if(mode==3)f.begin_fail_at=1;
        if(mode==4)f.end_fail_at=1;
        assert(ggown_probe(f.s,100000)!=GGOWN_OK && !f.queries);
        assert(!f.s->compatible);failed();cleanup();
    }
    for(unsigned end=3;end<=4;end++) {
        start();f.end_fail_hvc_at=end;
        assert(ggown_probe(f.s,100000)!=GGOWN_OK && f.sends==1 && f.queries==1);
        if(end==3)assert(!f.receives);
        else for(unsigned i=0;i<240;i++)assert(!f.s->session.rm.rx[i]);
        failed();cleanup();
    }
    for(unsigned at=1;at<=4;at++) {
        start();f.unexpected_irq_at=at;
        assert(ggown_probe(f.s,100000)!=GGOWN_OK);failed();cleanup();
    }
    start();f.begin_fail_after_hvcs=3;
    assert(ggown_probe(f.s,100000)!=GGOWN_OK && f.sends==1 && !f.receives);failed();cleanup();
}
static void transport_and_stop(void)
{
    for(unsigned step=1;step<=7;step++)for(unsigned mode=0;mode<3;mode++) {
        start();if(mode==0){f.bad_step=step;f.bad_status=1;}
        if(mode==1)f.transport_step=step;
        if(mode==2)f.wrong_seq=step;
        assert(ggown_probe(f.s,1000000)!=GGOWN_OK && f.queries==step);
        if(mode==0)assert(f.s->hvc.native_code==UINT64_C(0x100000000));
        failed();cleanup();
    }
    start();f.missing_parcel=1;
    assert(ggown_probe(f.s,1000000)==GGOWN_SESSION && f.queries==5);
    assert(f.s->session.rm.next_sequence==5);failed();cleanup();
    for(unsigned at=1;at<=16;at++)for(unsigned mode=0;mode<3;mode++) {
        start();if(mode==0)f.loss_hvc_at=at;if(mode==1)f.stop_hvc_at=at;if(mode==2)f.late_hvc_at=at;
        assert(ggown_probe(f.s,1000000)!=GGOWN_OK && f.hvcs==at);failed();cleanup();
    }
    for(unsigned stage=GGOWN_COMPATIBILITY;stage<=GGOWN_DISCOVERY;stage++)for(unsigned mode=0;mode<2;mode++) {
        start();if(mode==0)f.late_stage=stage;else f.rewind_stage=stage;
        assert(ggown_probe(f.s,1000000)!=GGOWN_OK);failed();cleanup();
    }
    start();f.full=1;f.stop_delay=1;
    assert(ggown_probe(f.s,1000000)!=GGOWN_OK && f.delays==1);failed();cleanup();
    start();f.full=1;f.late_delay=1;
    assert(ggown_probe(f.s,1000000)!=GGOWN_OK && f.delays==1);failed();cleanup();
    start();f.full=1;f.delay_error=1;
    assert(ggown_probe(f.s,1000000)!=GGOWN_OK && f.delays==1);failed();cleanup();
    start();assert(ggown_probe(f.s,1000000)==GGOWN_OK);f.poll_left=f.stop_notification=1;
    assert(ggown_poll(f.s,2,100000)!=GGOWN_IDLE && f.notifications==1);failed();cleanup();
    start();assert(ggown_probe(f.s,1000000)==GGOWN_OK);f.stopping=1;
    unsigned calls=f.hvcs;assert(ggown_invalidate(f.s)==GGOWN_LOST && f.hvcs==calls);
    failed();cleanup();
    start();f.stop_ready=1;
    assert(ggown_probe(f.s,1000000)==GGOWN_LOST && f.queries==7);failed();cleanup();
}
static void arguments_and_policy(void)
{
    assert(ggown_init(NULL,NULL,NULL)==GGOWN_ARGUMENT);
    assert(ggown_probe(NULL,100)==GGOWN_ARGUMENT && ggown_invalidate(NULL)==GGOWN_ARGUMENT);
    start();assert(ggown_probe(f.s,0)==GGOWN_ARGUMENT && !f.hvcs && !f.claims);
    assert(ggown_bootstrap(f.s,3000001)==GGOWN_ARGUMENT && !f.hvcs && !f.claims);
    assert(ggown_poll(f.s,0,100)==GGOWN_ARGUMENT);
    assert(ggown_get_status(f.s,(struct ggown_status *)f.s)==GGOWN_ARGUMENT);
    assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_USED);cleanup();
    for(unsigned mode=0;mode<4;mode++) {
        setup();if(mode==0)f.policy.live=NULL;if(mode==1)f.policy.delay_us=NULL;
        if(mode==2)f.report.crc32^=1;
        if(mode==3){f.report.source=0;ggr_report_seal(&f.report);}
        assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_ARGUMENT && !f.hvcs && !f.claims);cleanup();
    }
    setup();f.worker=0;
    assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_LOST && !f.hvcs && !f.claims);
    failed();cleanup();
    start();f.worker=0;
    assert(ggown_probe(f.s,1000000)==GGOWN_LOST && !f.hvcs && !f.claims);failed();cleanup();
    setup();f.init_reenter=1;
    assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_OK);
    assert(ggown_probe(f.s,1000000)==GGOWN_OK);cleanup();
    setup();f.loss_live_at=1;
    assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_LOST && !f.hvcs && !f.claims);
    failed();cleanup();
    start();f.loss_live_at=f.live_checks+1;
    assert(ggown_probe(f.s,1000000)==GGOWN_LOST && !f.hvcs && !f.claims);
    failed();cleanup();
    setup();f.report.tx_cap=0;ggr_report_seal(&f.report);
    assert(ggown_init(f.s,&f.report,&f.policy)==GGOWN_OK);
    assert(ggown_probe(f.s,1000000)==GGOWN_OK);cleanup();
}
int main(void)
{
    normal_and_poll();bootstrap_and_borrow();self_identity();boot_and_guard_failures();
    transport_and_stop();arguments_and_policy();
    puts("GPU Gunyah native owner: real HVC/RM/session bridge, IRQ guards, boot policy, staged output, sequence borrowing, stop and failures passed (platform/HVC mocks only)");
    return 0;
}
