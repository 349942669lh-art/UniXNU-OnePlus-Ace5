/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Explicit native RM owner. This is an actual GPU-worker policy and guarded
 * transport, never a factory that turns boot report bits into a fence proof.
 */
int GiuliaGPU::gunyahPolicy(void *cookie,uint64_t epoch)
{
    GiuliaGPU *s=(GiuliaGPU *)cookie;
    return !__atomic_load_n(&s->gunyahStopLatch,__ATOMIC_ACQUIRE) &&
        s->gunyahWorkerArmed && s->gunyahWorker==current_thread() &&
        s->manualMemoryPinned && s->hostPrepared && !s->platformPoisoned &&
        s->bootGunyahCaptured && epoch==s->bootGunyah.epoch;
}
uint64_t GiuliaGPU::gunyahNow(void *)
{
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;
}
int GiuliaGPU::gunyahDelay(void *cookie,unsigned us)
{
    GiuliaGPU *s=(GiuliaGPU *)cookie;
    if(us>1000000 || !gunyahPolicy(s,s->bootGunyah.epoch))return -1;
    IODelay(us);return gunyahPolicy(s,s->bootGunyah.epoch)?0:-1;
}
int GiuliaGPU::gunyahNotification(void *cookie,uint32_t id,const void *p,size_t bytes)
{
    GiuliaGPU *s=(GiuliaGPU *)cookie;uint16_t vmid=0;struct gg_rq_vm_state state;
    if(!gunyahPolicy(s,s->bootGunyah.epoch) || id!=GGRQ_VM_STATUS ||
       gg_rq_parse_status_notification((const uint8_t *)p,bytes,&vmid,&state))return 0;
    IOLog("GIULIA GPU GUNYAH: event=%#x vmid=%u vm_status=%u os_status=%u app_status=%u completion_verified=0\n",
        id,vmid,state.vm_status,state.os_status,state.app_status);
    return 1; /* The session separately invalidates self/peer lifecycle events. */
}
static int gpuGunyahResult(int result)
{
    if(result==GGOWN_OK || result==GGOWN_IDLE || result==GGOWN_BUDGET)return 0;
    if(result==GGOWN_ARGUMENT || result==GGOWN_USED)return 4;
    if(result==GGOWN_BUSY)return 5;
    return 8; /* Native outcome uncertain: preserve state and stop host I/O. */
}
int GiuliaGPU::gunyahCommand(uint32_t action,uint32_t b,uint32_t c,uint32_t d)
{
    if(action>5 || d || (action==0 && (b||c)) ||
       ((action==1 || action==3 || action==5) && (!b || b>3000 || c)) ||
       (action==2 && (!b || b>GGRS_MAX_POLL_FRAMES || !c || c>3000)) ||
       (action==4 && (b||c)))return 4;
    struct ggown_state *owner=__atomic_load_n(&gunyahOwner,__ATOMIC_ACQUIRE);
    if(action==0) {
        struct ggown_status status={};
        if(owner && ggown_get_status(owner,&status)!=GGOWN_OK)return 5;
        IOLog("GIULIA GPU GUNYAH: abi=1 captured=%u initialized=%u bootstrapped=%u compatible=%u irq_claimed=%u invalid=%u stage=%u epoch=%#llx retained_until_reboot=%u provider_ready=0 completion_verified=0\n",
            bootGunyahCaptured,status.initialized,status.bootstrapped,status.compatible,status.irq_claimed,
            status.invalid,status.stage,(unsigned long long)status.epoch,gunyahPinned);
        IOLog("GIULIA GPU GUNYAH: query_step=%u next_sequence=%u rm_quarantined=%u hvc_calls=%u last_result=%u probe_result=%u session_result=%u\n",
            status.query_step,status.next_sequence,status.rm_quarantined,status.hvc_calls,
            status.last_result,status.probe_result,status.session_result);
        IOLog("GIULIA GPU GUNYAH: native_code=%#llx api_info=%#llx\n",
            (unsigned long long)status.native_code,(unsigned long long)status.api_info);
        IOLog("GIULIA GPU GUNYAH: self_attempted=%u self_observed=%u self_vmid=%u rpc_result=%u remote_error=%u historical=1 gpu_grant=0\n",
            status.self_attempted,status.self_observed,status.self_vmid,
            status.self_rpc_result,status.self_remote_error);
        return 0;
    }
    if(!manualMemoryPinned || !hostPrepared || platformPoisoned ||
       __atomic_load_n(&gunyahStopLatch,__ATOMIC_ACQUIRE) ||
       !gunyahWorker || gunyahWorker!=current_thread())return 8;
    if(!bootGunyahCaptured || !ggr_report_valid(&bootGunyah))return 7;
    if(!owner && action!=1 && action!=3 && action!=5)return 7;
    if(!owner) {
        owner=(struct ggown_state *)IOMalloc(sizeof(*owner));
        if(!owner)return 7;
        bzero(owner,sizeof(*owner));
        gunyahWorkerArmed=true;
        const struct ggown_policy policy={this,gunyahPolicy,gunyahNow,gunyahDelay,gunyahNotification};
        int result=ggown_init(owner,&bootGunyah,&policy);
        if(result!=GGOWN_OK) {
            IOFree(owner,sizeof(*owner));gunyahWorkerArmed=false;
            int code=gpuGunyahResult(result);if(code==8)platformPoisoned=true;
            return code;
        }
        /* Pin cookie/service before publishing a descriptor used by HVC. The
         * native IRQ reservation and all state remain pinned until reboot. */
        retain();gunyahPinned=true;
        __atomic_store_n(&gunyahOwner,owner,__ATOMIC_RELEASE);
    }
    int result;
    if(action==1)result=ggown_probe(owner,b*1000u);
    else if(action==2)result=ggown_poll(owner,b,c*1000u);
    else if(action==3)result=ggown_bootstrap(owner,b*1000u);
    else if(action==5)result=ggown_query_self(owner,b*1000u);
    else result=ggown_invalidate(owner);
    if(action==4)return 0;
    int code=gpuGunyahResult(result);
    if(code==8)platformPoisoned=true;
    IOLog("GIULIA GPU GUNYAH: action=%u result=%d code=%d provider_ready=0 completion_verified=0\n",action,result,code);
    return code;
}
void GiuliaGPU::gunyahPeriodic()
{
    struct ggown_state *owner=__atomic_load_n(&gunyahOwner,__ATOMIC_ACQUIRE);
    if(!owner || !gunyahPolicy(this,bootGunyah.epoch))return;
    struct ggown_status status={};
    if(ggown_get_status(owner,&status)!=GGOWN_OK || status.invalid || status.stage!=GGOWN_READY)return;
    int result=ggown_poll(owner,64,5000);
    if(result!=GGOWN_IDLE && result!=GGOWN_BUDGET && result!=GGOWN_OK) {
        platformPoisoned=true;
        IOLog("GIULIA GPU GUNYAH: lifecycle_lost=1 result=%d host_io_quarantined=1 completion_verified=0\n",result);
    }
}
void GiuliaGPU::stopGunyah()
{
    __atomic_store_n(&gunyahStopLatch,1u,__ATOMIC_RELEASE);
    struct ggown_state *owner=__atomic_load_n(&gunyahOwner,__ATOMIC_ACQUIRE);
    if(owner)(void)ggown_invalidate(owner);
    /* Never free the state or release the permanent service retain. */
}
