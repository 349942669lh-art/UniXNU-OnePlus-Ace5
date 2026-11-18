/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Native XNU fixed-page mapping/executor for the GCC GPU parent branch subset.
 * No production caller is registered; no DT or terminal command enables this.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#if !defined(GG_HOST_TEST)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#endif
#include "giulia_gpu_parents.h"
#include "../platform/giulia_platform_resources.h"

static const struct ggcp_scope parentScope={GGCP_PAGE,600000000,300000000,GGCP_BYTES,0,GGCP_MASK,GGCP_SETTLE_US};
static const struct gpr_key parentPage={GPR_MMIO,0,0,0,GGCP_PAGE,GGCP_BYTES};
static struct {
    IOLock *lock;
    IOService *source;
    IOMemoryDescriptor *memory;
    IOMemoryMap *map;
    struct ggcp_authority authority;
    struct ggcp_state core;
    struct gpr_token page;
    uint64_t epoch;
    /* lifecycle: 0=unbound, 1=bound, 2=revoked, 3=clean detach. */
    unsigned used,opened,leased,guard,lifecycle,quarantined,last_code;
} parents;

extern "C" __attribute__((used)) int giulia_gpu_parents_initialize(void)
{
    if(__atomic_load_n(&parents.lock,__ATOMIC_ACQUIRE))return GGCP_OK;
    IOLock *candidate=IOLockAlloc();if(!candidate)return GGCP_UNAVAILABLE;
    IOLock *empty=NULL;
    if(!__atomic_compare_exchange_n(&parents.lock,&empty,candidate,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(candidate);
    return GGCP_OK;
}
static int parentPoison(int code)
{
    parents.quarantined=1;parents.last_code=code;ggcp_quarantine(&parents.core);
    if(parents.leased)(void)gpr_global_quarantine(&parents.page,&parents,parents.epoch);
    return code;
}
static int parentLive(void *)
{
    return parents.guard && !parents.quarantined &&
        !(__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)==2) && parents.leased &&
        gpr_global_owns(&parents.page,&parents,parents.epoch) &&
        parents.authority.live(parents.authority.cookie,parents.epoch)==1 &&
        !(__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)==2);
}
static int parentEnter()
{
    if(parents.quarantined || (__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)==2) ||
       !parents.leased || !gpr_global_owns(&parents.page,&parents,parents.epoch))
        return parentPoison(GGCP_LOST);
    int e=parents.authority.enter(parents.authority.cookie,parents.epoch,&parentScope);
    if(e==0)return GGCP_BUSY; /* Guard denied, no backing changed or I/O. */
    if(e!=1)return parentPoison(GGCP_LOST);
    parents.guard=1;
    if(!parentLive(NULL))return parentPoison(GGCP_LOST);
    return GGCP_OK;
}
static int parentLeave(int code)
{
    /* This is CPU guard unlock, NOT a clock/access lease drop or rollback. */
    if(parents.guard) {
        int e=parents.authority.leave(parents.authority.cookie,parents.epoch);
        parents.guard=0;
        if(e)return parentPoison(GGCP_QUARANTINED);
    }
    if((__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)==2) || parents.core.quarantined)
        return parentPoison(GGCP_QUARANTINED);
    return code;
}
static int parentRead(void *,uint32_t *value)
{
    if(!parents.map)return -1;
    *value=*(volatile uint32_t *)parents.map->getVirtualAddress();
#if defined(GG_HOST_TEST)
    __sync_synchronize();
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    return 0;
}
static int parentWrite(void *,uint32_t value)
{
    if(!parents.map)return -1;
    *(volatile uint32_t *)parents.map->getVirtualAddress()=value;
#if defined(GG_HOST_TEST)
    __sync_synchronize();
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    return 0;
}
static int parentSettle(void *,unsigned us)
{ if(us!=GGCP_SETTLE_US)return -1;IODelay(us);return 0; }
static int parentQuiet(void *)
{ return parents.authority.quiescent(parents.authority.cookie,parents.epoch); }
static IOService *parentCleanup()
{
    /* CPU-only teardown, only on a clean result and with no native votes.
     * Keep software exclusion until all aliases have been unmapped. */
    /* Compete with invalidate BEFORE dropping any backing. If revoke wins,
     * keep map/source/exclusion pinned. If detach wins, invalidate is stale. */
    unsigned bound=1;
    if(!__atomic_compare_exchange_n(&parents.lifecycle,&bound,3u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        (void)parentPoison(GGCP_QUARANTINED);return NULL;
    }
    if(parents.map){parents.map->release();parents.map=NULL;}
    if(parents.memory){parents.memory->release();parents.memory=NULL;}
    if(parents.leased) {
        if(gpr_global_release(&parents.page,&parents,parents.epoch)) {
            (void)parentPoison(GGCP_QUARANTINED);return NULL;
        }
        parents.leased=0;
    }
    if(parents.opened && gpr_global_retire_epoch(&parents,parents.epoch)) {
        (void)parentPoison(GGCP_QUARANTINED);return NULL;
    }
    parents.opened=0;
    IOService *old=parents.source;
    __atomic_store_n(&parents.source,(IOService *)NULL,__ATOMIC_RELEASE);
    return old;
}
extern "C" __attribute__((used)) int giulia_gpu_parents_open(void *source,uint64_t epoch,const struct ggcp_authority *a)
{
    if(!source || !epoch || !a || a->abi!=GGCP_ABI || a->bytes!=sizeof(*a) ||
       !a->cookie || !a->enter || !a->live || !a->quiescent || !a->leave)return GGCP_ARGUMENT;
    IOLock *lock=__atomic_load_n(&parents.lock,__ATOMIC_ACQUIRE);if(!lock)return GGCP_UNAVAILABLE;
    IOLockLock(lock);
    if(parents.used){IOLockUnlock(lock);return GGCP_USED;}
    parents.used=1;parents.epoch=epoch;parents.authority=*a;
    ((IOService *)source)->retain();
    __atomic_store_n(&parents.source,(IOService *)source,__ATOMIC_RELEASE);
    __atomic_store_n(&parents.lifecycle,1u,__ATOMIC_RELEASE);
    int code=GGCP_BUSY;
    if(!gpr_global_initialize() && !gpr_global_open_epoch(&parents,epoch)) {
        parents.opened=1;
        if(!gpr_global_claim(&parentPage,&parents,epoch,0,0,&parents.page)) {
            parents.leased=1;code=parentEnter();
            if(!code) {
                parents.memory=IOMemoryDescriptor::withPhysicalAddress(GGCP_PAGE,GGCP_BYTES,kIODirectionInOut);
                if(parents.memory)parents.map=parents.memory->map(kIOMapInhibitCache);
                if(!parents.map)code=GGCP_UNAVAILABLE;
                else if(!parents.map->getVirtualAddress() ||
                        (parents.map->getVirtualAddress()&3u) ||
                        parents.map->getLength()!=GGCP_BYTES)code=GGCP_UNAVAILABLE;
                else {
                    const struct ggcp_io io={NULL,parentLive,parentRead,parentWrite,parentSettle,parentQuiet};
                    code=ggcp_init(&parents.core,&io);
                }
            }
            code=parentLeave(code);
        }
    }
    IOService *old=NULL;
    if(code && !parents.quarantined)old=parentCleanup();
    if(parents.quarantined)code=GGCP_QUARANTINED;
    parents.last_code=code;
    IOLog("GIULIA GPU PARENT: open code=%d page=0x152000 subset_only=1 acceleration=0\n",code);
    IOLockUnlock(lock);if(old)old->release();return code;
}
static int parentOperation(void *source,unsigned operation,unsigned clock,uint64_t *ticket)
{
    IOLock *lock=__atomic_load_n(&parents.lock,__ATOMIC_ACQUIRE);if(!lock)return GGCP_UNAVAILABLE;
    IOLockLock(lock);
    if(!source || source!=parents.source){IOLockUnlock(lock);return GGCP_ARGUMENT;}
    int code=parentEnter();
    if(!code)code=operation==0?ggcp_acquire(&parents.core,clock,ticket):
        operation==1?ggcp_release(&parents.core,*ticket):ggcp_close(&parents.core);
    code=parentLeave(code);
    IOService *old=NULL;
    if(operation==2 && !code)old=parentCleanup();
    if(parents.quarantined)code=GGCP_QUARANTINED;
    parents.last_code=code;
    IOLockUnlock(lock);if(old)old->release();return code;
}
extern "C" __attribute__((used)) int giulia_gpu_parents_acquire(void *s,unsigned clock,uint64_t *ticket)
{
    if(!ticket)return GGCP_ARGUMENT;
    *ticket=0;
    if(clock>=GGCP_CLOCKS)return GGCP_ARGUMENT;
    return parentOperation(s,0,clock,ticket);
}
extern "C" __attribute__((used)) int giulia_gpu_parents_release(void *s,uint64_t ticket)
{ if(!ticket)return GGCP_ARGUMENT;return parentOperation(s,1,0,&ticket); }
extern "C" __attribute__((used)) int giulia_gpu_parents_close(void *s)
{ return parentOperation(s,2,0,NULL); }
extern "C" __attribute__((used)) int giulia_gpu_parents_invalidate(void *s)
{
    if(!s || __atomic_load_n(&parents.source,__ATOMIC_ACQUIRE)!=s)return GGCP_ARGUMENT;
    unsigned bound=1;
    if(!__atomic_compare_exchange_n(&parents.lifecycle,&bound,2u,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE) && bound!=2)return GGCP_ARGUMENT;
    IOLock *lock=__atomic_load_n(&parents.lock,__ATOMIC_ACQUIRE);if(!lock)return GGCP_UNAVAILABLE;
    IOLockLock(lock);int code=parentPoison(GGCP_QUARANTINED);IOLockUnlock(lock);return code;
}
extern "C" __attribute__((used)) int giulia_gpu_parents_status(struct ggcp_status *out)
{
    if(!out)return GGCP_ARGUMENT;
    *out={};IOLock *lock=__atomic_load_n(&parents.lock,__ATOMIC_ACQUIRE);if(!lock)return GGCP_UNAVAILABLE;
    IOLockLock(lock);
    out->epoch=parents.epoch;out->bound=parents.source!=NULL;
    out->ready_cached=parents.core.ready;out->quarantined=parents.quarantined;
    out->revoked=(__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)==2);
    out->enabled_cached=parents.core.enabled;out->main_refs=parents.core.refs[0];out->div_refs=parents.core.refs[1];
    out->write_attempted=parents.core.write_attempted;out->last_code=parents.last_code;
    IOLockUnlock(lock);return GGCP_OK;
}
#endif
