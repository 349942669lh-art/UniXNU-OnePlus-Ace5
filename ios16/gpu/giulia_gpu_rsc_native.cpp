/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native IOKit mapping, bounds, barriers and lifetime for restricted ACTIVE
 * TCS transactions. Source authorization is mandatory; no production caller.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#if !defined(GG_HOST_TEST)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#endif
#include "giulia_gpu_rsc_native.h"
#include "../platform/giulia_platform_resources.h"

/* Static source candidate only. A successful source take must bind every
 * field to THIS boot. Neither these constants nor readback grants access. */
static const struct gpr_key nativeRscPage={GPR_MMIO,0,0,0,0x17a20000,0x10000};
static struct {
    IOLock *lock;
    IOService *source;
    IOMemoryDescriptor *memory;
    IOMemoryMap *map;
    struct ggrn_authority authority;
    struct ggrn_scope scope;
    struct ggrsc_state core;
    struct gpr_token page;
    uint64_t grant;
    /* lifecycle: 0 unused, 1 bound, 2 revoked, 3 detaching/cleanly detached. */
    unsigned used,lifecycle,epoch_open,leased,taken,quarantined,last,last_core;
} nativeRsc;

extern "C" __attribute__((used)) const char *giulia_gpu_rsc_native_identity(void)
{ return GGRN_IDENTITY; }
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_initialize(void)
{
    if(__atomic_load_n(&nativeRsc.lock,__ATOMIC_ACQUIRE))return GGRN_OK;
    IOLock *candidate=IOLockAlloc();if(!candidate)return GGRN_UNAVAILABLE;
    IOLock *empty=NULL;
    if(!__atomic_compare_exchange_n(&nativeRsc.lock,&empty,candidate,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(candidate);
    return GGRN_OK;
}
static int nativeRscPoison()
{
    nativeRsc.quarantined=1;nativeRsc.last=GGRN_QUARANTINED;
    ggrsc_invalidate(&nativeRsc.core);
    if(nativeRsc.leased)(void)gpr_global_quarantine(&nativeRsc.page,&nativeRsc,
        nativeRsc.scope.transport.epoch);
    return GGRN_QUARANTINED;
}
static int nativeRscLive(void *,uint64_t epoch)
{
    return nativeRsc.taken && nativeRsc.grant && !nativeRsc.quarantined &&
        __atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)==1 &&
        nativeRsc.leased && epoch==nativeRsc.scope.transport.epoch &&
        gpr_global_owns(&nativeRsc.page,&nativeRsc,epoch) &&
        nativeRsc.authority.live(nativeRsc.authority.cookie,epoch,nativeRsc.grant)==1 &&
        __atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)==1;
}
static void nativeRscBarrier()
{
#if defined(GG_HOST_TEST)
    __sync_synchronize();
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
/* Mapping is not re-created or remapped during submit/poll. Exact extent
 * checked at open; each access is word aligned, bounds checked and ordered. */
static int nativeRscReadDrv(uint32_t offset,uint32_t *out)
{
    if(!out || !nativeRsc.map || (offset&3u) || offset>nativeRsc.scope.drv_bytes-4u ||
       !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    uintptr_t base=nativeRsc.map->getVirtualAddress();
    if(!base || (base&3u) || !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    nativeRscBarrier();
    *out=*(volatile uint32_t *)(base+offset);
    nativeRscBarrier();return 0;
}
static int nativeRscOffset(uint32_t offset,int write)
{
    const struct ggrsc_state *s=&nativeRsc.core;
    if((offset&3u) || offset>nativeRsc.scope.transport.mapped_bytes-4u)return 0;
    if(write)return offset==8u || offset==s->control || offset==s->wait ||
        offset==s->enable || offset==s->msg || offset==s->addr || offset==s->data;
    return offset==0u || offset==4u || offset==s->control || offset==s->wait ||
        offset==s->enable || offset==s->status || offset==s->msg ||
        offset==s->addr || offset==s->data || offset==s->cmd_status;
}
static int nativeRscRead(void *,uint32_t offset,uint32_t *out)
{
    if(!nativeRscOffset(offset,0))return -1;
    return nativeRscReadDrv(nativeRsc.scope.tcs_offset+offset,out);
}
static int nativeRscWrite(void *,uint32_t offset,uint32_t value)
{
    if(!nativeRscOffset(offset,1) || !nativeRsc.map ||
       !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    const struct ggrsc_state *s=&nativeRsc.core;
    if((offset==8u && value!=(1u<<s->config.tcs_id)) ||
       (offset==s->control && value!=0u && value!=(1u<<16) && value!=((1u<<16)|(1u<<24))) ||
       ((offset==s->wait || offset==s->enable) && value>1u) ||
       (offset==s->msg && value!=(8u|(1u<<8)|(1u<<16))) ||
       (offset==s->addr && value!=s->config.arc_address) ||
       (offset==s->data && (value!=s->selector || value>=16u ||
         !(s->config.selector_mask&(1u<<value)))))return -1;
    uintptr_t base=nativeRsc.map->getVirtualAddress();
    if(!base || (base&3u) || !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    nativeRscBarrier();
    *(volatile uint32_t *)(base+nativeRsc.scope.tcs_offset+offset)=value;
    nativeRscBarrier();return 0;
}
static int nativeRscNow(void *,uint64_t *out)
{
    if(!out || !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);
    *out=ns/1000u;return 0;
}
static int nativeRscDelay(void *,unsigned us)
{
    if(us!=1u || !nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return -1;
    IODelay(us);return 0;
}
static int nativeRscConfig(const struct ggrsc_config *c)
{
    return c && c->abi==GGRSC_ABI && (c->version==GGRSC_V27 || c->version==GGRSC_V30) &&
        c->epoch && !c->reserved && c->tcs_id<3u && c->mapped_bytes==0xf300u &&
        c->arc_address && !(c->arc_address&0xfff00003u) &&
        ((c->arc_address>>16)&15u)==3u && c->selector_mask &&
        !(c->selector_mask&0xffff0000u) && c->timeout_us && c->timeout_us<=1000000u;
}
static int nativeRscMetadata()
{
    uint32_t id=0,solver=0,config=0;
    if(nativeRscReadDrv(0,&id) || nativeRscReadDrv(4,&solver) || nativeRscReadDrv(12,&config))
        return nativeRscPoison();
    const unsigned major=(id>>16)&255u,minor=(id>>8)&255u;
    const unsigned expected=nativeRsc.scope.transport.version;
    const unsigned count=(config>>(6u*nativeRsc.scope.drv_id))&63u,commands=config>>27;
    if(!nativeRscLive(NULL,nativeRsc.scope.transport.epoch))return nativeRscPoison();
    if(id==0xffffffffu || solver==0xffffffffu || config==0xffffffffu ||
       !((expected==27u && major==2u && minor==7u) ||
         (expected==30u && major==3u && minor==0u)) ||
       (solver&(1u<<24)) || count<8u || count>32u || !commands || commands>16u)
        return GGRN_PROTOCOL;
    return GGRN_OK;
}
/* Only clean/unchanged paths reach this. Revocation wins BEFORE any unmap.
 * Exclusion stays held until alias removal AND confirmed source drop. */
static IOService *nativeRscDetach()
{
    if(nativeRsc.taken && !nativeRscLive(NULL,nativeRsc.scope.transport.epoch)) {
        (void)nativeRscPoison();return NULL;
    }
    unsigned bound=1;
    if(!__atomic_compare_exchange_n(&nativeRsc.lifecycle,&bound,3u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        (void)nativeRscPoison();return NULL;
    }
    if(nativeRsc.map){nativeRsc.map->release();nativeRsc.map=NULL;}
    if(nativeRsc.memory){nativeRsc.memory->release();nativeRsc.memory=NULL;}
    if(nativeRsc.taken) {
        if(nativeRsc.authority.drop(nativeRsc.authority.cookie,&nativeRsc.scope,nativeRsc.grant)) {
            (void)nativeRscPoison();return NULL;
        }
        nativeRsc.taken=0;
    }
    if(nativeRsc.leased) {
        if(gpr_global_release(&nativeRsc.page,&nativeRsc,nativeRsc.scope.transport.epoch)) {
            (void)nativeRscPoison();return NULL;
        }
        nativeRsc.leased=0;
    }
    if(nativeRsc.epoch_open && gpr_global_retire_epoch(&nativeRsc,nativeRsc.scope.transport.epoch)) {
        (void)nativeRscPoison();return NULL;
    }
    nativeRsc.epoch_open=0;
    IOService *old=nativeRsc.source;
    __atomic_store_n(&nativeRsc.source,(IOService *)NULL,__ATOMIC_RELEASE);
    return old;
}
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_open(void *source,
    const struct ggrsc_config *config,const struct ggrn_authority *a)
{
    if(!source || !nativeRscConfig(config) || !a || a->abi!=GGRN_ABI || a->bytes!=sizeof(*a) ||
       !a->cookie || !a->take || !a->live || !a->approve || !a->settled || !a->drop)return GGRN_ARGUMENT;
    IOLock *lock=__atomic_load_n(&nativeRsc.lock,__ATOMIC_ACQUIRE);if(!lock)return GGRN_UNAVAILABLE;
    IOLockLock(lock);
    if(nativeRsc.used){IOLockUnlock(lock);return GGRN_USED;}
    nativeRsc.used=1;nativeRsc.authority=*a;
    nativeRsc.scope={0x17a20000,0x10000,2,0xd00,37,7,0,*config};
    const uint64_t epoch=nativeRsc.scope.transport.epoch;
    ((IOService *)source)->retain();
    __atomic_store_n(&nativeRsc.source,(IOService *)source,__ATOMIC_RELEASE);
    __atomic_store_n(&nativeRsc.lifecycle,1u,__ATOMIC_RELEASE);
    int code=GGRN_BUSY;
    if(!gpr_global_initialize() && !gpr_global_open_epoch(&nativeRsc,epoch)) {
        nativeRsc.epoch_open=1;
        if(!gpr_global_claim(&nativeRscPage,&nativeRsc,epoch,0,0,&nativeRsc.page)) {
            nativeRsc.leased=1;
            if(__atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)!=1)code=nativeRscPoison();
            else {
                int result=nativeRsc.authority.take(nativeRsc.authority.cookie,&nativeRsc.scope,&nativeRsc.grant);
                if(!result && !nativeRsc.grant)code=GGRN_BUSY;
                else {
                    nativeRsc.taken=1;
                    if(result!=1 || !nativeRsc.grant || !nativeRscLive(NULL,epoch))code=nativeRscPoison();
                    else {
                        nativeRsc.memory=IOMemoryDescriptor::withPhysicalAddress(
                            nativeRscPage.base,nativeRscPage.bytes,kIODirectionInOut);
                        code=GGRN_UNAVAILABLE;
                        if(!nativeRscLive(NULL,epoch))code=nativeRscPoison();
                        else if(nativeRsc.memory)nativeRsc.map=nativeRsc.memory->map(kIOMapInhibitCache);
                        if(!nativeRscLive(NULL,epoch))code=nativeRscPoison();
                        uintptr_t address=0;
                        if(!nativeRsc.quarantined && nativeRsc.map &&
                           nativeRsc.map->getLength()==nativeRscPage.bytes)
                            address=nativeRsc.map->getVirtualAddress();
                        if(!nativeRscLive(NULL,epoch))code=nativeRscPoison();
                        if(!nativeRsc.quarantined && address && !(address&3u)) {
                            code=nativeRscMetadata();
                            if(!code) {
                                const struct ggrsc_io io={NULL,nativeRscLive,nativeRscRead,
                                    nativeRscWrite,nativeRscNow,nativeRscDelay};
                                nativeRsc.last_core=ggrsc_open(&nativeRsc.core,&nativeRsc.scope.transport,&io);
                                code=nativeRsc.last_core ? nativeRscPoison() : GGRN_OK;
                            }
                        }
                    }
                }
            }
        }
    }
    IOService *old=NULL;
    if(code && !nativeRsc.quarantined)old=nativeRscDetach();
    if(nativeRsc.quarantined)code=GGRN_QUARANTINED;
    nativeRsc.last=code;
    IOLog("GIULIA GPU RSC NATIVE: open code=%d provider_required=1 acceleration=0\n",code);
    IOLockUnlock(lock);if(old)old->release();return code;
}
static int nativeRscOperation(void *source,unsigned operation,uint32_t selector,uint64_t *serial)
{
    IOLock *lock=__atomic_load_n(&nativeRsc.lock,__ATOMIC_ACQUIRE);if(!lock)return GGRN_UNAVAILABLE;
    IOLockLock(lock);
    if(!source || source!=nativeRsc.source){IOLockUnlock(lock);return GGRN_ARGUMENT;}
    int code=GGRN_OK;
    if(nativeRsc.quarantined)code=GGRN_QUARANTINED;
    else if(!nativeRscLive(NULL,nativeRsc.scope.transport.epoch))code=nativeRscPoison();
    else if(operation==0) {
        if(!(nativeRsc.scope.transport.selector_mask&(1u<<selector)))code=GGRN_ARGUMENT;
        else if(nativeRsc.core.inflight)code=GGRN_BUSY;
        else {
            int approved=nativeRsc.authority.approve(nativeRsc.authority.cookie,
                &nativeRsc.scope,nativeRsc.grant,selector);
            if(!nativeRscLive(NULL,nativeRsc.scope.transport.epoch))code=nativeRscPoison();
            else if(!approved)code=GGRN_BUSY;
            else if(approved!=1)code=nativeRscPoison();
            else nativeRsc.last_core=ggrsc_submit(&nativeRsc.core,selector,serial);
        }
    } else if(operation==1)nativeRsc.last_core=ggrsc_poll(&nativeRsc.core,*serial);
    else if(nativeRsc.core.inflight)code=GGRN_BUSY;
    else {
        int settled=nativeRsc.authority.settled(nativeRsc.authority.cookie,&nativeRsc.scope,
            nativeRsc.grant,nativeRsc.core.completed,nativeRsc.core.selector);
        if(!nativeRscLive(NULL,nativeRsc.scope.transport.epoch))code=nativeRscPoison();
        else if(!settled)code=GGRN_BUSY;
        else if(settled!=1)code=nativeRscPoison();
        else nativeRsc.last_core=ggrsc_close(&nativeRsc.core);
    }
    if(!code) {
        if(nativeRsc.core.quarantined)code=nativeRscPoison();
        else if(nativeRsc.last_core==GGRSC_PENDING)code=GGRN_PENDING;
        else if(nativeRsc.last_core==GGRSC_BUSY)code=GGRN_BUSY;
        else if(nativeRsc.last_core==GGRSC_ARGUMENT)code=GGRN_ARGUMENT;
        else if(nativeRsc.last_core!=GGRSC_OK)code=nativeRscPoison();
    }
    IOService *old=NULL;
    if(operation==2 && !code)old=nativeRscDetach();
    if(nativeRsc.quarantined)code=GGRN_QUARANTINED;
    nativeRsc.last=code;IOLockUnlock(lock);if(old)old->release();return code;
}
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_submit(void *s,uint32_t selector,uint64_t *serial)
{
    if(!serial)return GGRN_ARGUMENT;
    *serial=0;
    if(selector>=16)return GGRN_ARGUMENT;
    return nativeRscOperation(s,0,selector,serial);
}
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_poll(void *s,uint64_t serial)
{ if(!serial)return GGRN_ARGUMENT;return nativeRscOperation(s,1,0,&serial); }
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_close(void *s)
{ return nativeRscOperation(s,2,0,NULL); }
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_invalidate(void *s)
{
    if(!s || __atomic_load_n(&nativeRsc.source,__ATOMIC_ACQUIRE)!=s)return GGRN_ARGUMENT;
    unsigned bound=1;
    if(!__atomic_compare_exchange_n(&nativeRsc.lifecycle,&bound,2u,false,
       __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && bound!=2)return GGRN_BUSY;
    IOLock *lock=__atomic_load_n(&nativeRsc.lock,__ATOMIC_ACQUIRE);if(!lock)return GGRN_UNAVAILABLE;
    IOLockLock(lock);int code=nativeRscPoison();IOLockUnlock(lock);return code;
}
extern "C" __attribute__((used)) int giulia_gpu_rsc_native_status(struct ggrn_status *out)
{
    if(!out)return GGRN_ARGUMENT;
    *out={};IOLock *lock=__atomic_load_n(&nativeRsc.lock,__ATOMIC_ACQUIRE);if(!lock)return GGRN_UNAVAILABLE;
    IOLockLock(lock);
    out->epoch=nativeRsc.scope.transport.epoch;out->grant=nativeRsc.grant;
    out->serial=nativeRsc.core.serial;out->completed=nativeRsc.core.completed;
    out->bound=nativeRsc.source!=NULL;
    out->ready_cached=nativeRsc.core.ready && !nativeRsc.quarantined &&
        __atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)==1;
    out->revoked=__atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)==2;
    out->quarantined=nativeRsc.quarantined;out->mapped=nativeRsc.map!=NULL;out->leased=nativeRsc.leased;
    out->selector=nativeRsc.core.selector;out->inflight=nativeRsc.core.inflight;
    out->mutation_attempted=nativeRsc.core.mutation_attempted;out->trigger_attempted=nativeRsc.core.trigger_attempted;
    out->last_code=nativeRsc.last;out->last_transport_code=nativeRsc.last_core;
    IOLockUnlock(lock);return GGRN_OK;
}
#endif
