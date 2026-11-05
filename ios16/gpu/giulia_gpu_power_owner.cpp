/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * One permanent XNU executor. Provider and consumer lifetimes are retained
 * through the whole transaction, including quarantine. The native source's
 * callbacks are copied, never borrowed from a stack or a Boot Services table.
 * This is a coordinator, NOT the missing SM8650 hardware power/clock driver.
 */
#if !defined(GG_HOST_TEST)
#include <pexpert/arm64/board_config.h>
#endif
#if defined(GG_HOST_TEST) || defined(ARM64_BOARD_CONFIG_SWEET)
#if !defined(GG_HOST_TEST)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#endif
#include "giulia_gpu_power_owner.h"

static const struct ggpo_scope powerScope={
    {{0x03d00000,0x40000},{0x03d68000,0x37000},{0x03d40000,0x10000},
     {0x03d90000,0x9800},{0x03da0000,0x40000},{0x00171000,0x1000}},
    0x23,{332,336,337},GGPO_PREREQUISITES,0
};
static struct {
    IOService *source,*consumer;
    struct ggpo_source ops;
    struct ggpo_grant grant;
    struct ggrt_state runtime;
    unsigned registered,used,attempted,taken,ready,quarantined,lost,mutation,last;
} powerOwner;
static IOLock *powerOwnerLock;

static IOLock *ownerLock()
{ return __atomic_load_n(&powerOwnerLock,__ATOMIC_ACQUIRE); }
extern "C" __attribute__((used)) int giulia_gpu_power_owner_initialize(void)
{
    if(ownerLock())return GGPO_OK;
    IOLock *candidate=IOLockAlloc();
    if(!candidate)return GGPO_UNAVAILABLE;
    IOLock *empty=NULL;
    if(!__atomic_compare_exchange_n(&powerOwnerLock,&empty,candidate,false,
        __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))IOLockFree(candidate);
    return GGPO_OK;
}
static int ownerScope(const struct ggpo_scope *s)
{
    if(s->sid_mask!=powerScope.sid_mask || s->reserved ||
       s->prerequisites!=powerScope.prerequisites)return 0;
    for(unsigned i=0;i<GGPO_WINDOWS;i++)
        if(s->windows[i].base!=powerScope.windows[i].base ||
           s->windows[i].bytes!=powerScope.windows[i].bytes)return 0;
    for(unsigned i=0;i<GGPO_IRQS;i++)
        if(s->intids[i]!=powerScope.intids[i])return 0;
    return 1;
}
static int ownerGrant(uint64_t epoch)
{
    const struct ggpo_grant *g=&powerOwner.grant;
    return epoch && g->epoch==epoch && g->serial && g->self_vmid &&
        g->self_vmid<65535 && !g->reserved && ownerScope(&g->scope);
}
static int ownerAuthority(void *,uint64_t epoch)
{
    return powerOwner.registered && powerOwner.taken && !powerOwner.quarantined &&
        !__atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE) && ownerGrant(epoch) &&
        powerOwner.ops.live(powerOwner.ops.cookie,&powerOwner.grant)==1 &&
        !__atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE);
}
static int ownerPoison(unsigned code)
{
    powerOwner.quarantined=1;powerOwner.ready=0;powerOwner.last=code;
    giulia_gpu_runtime_quarantine(&powerOwner.runtime);
    return (int)code;
}
static IOService *ownerDetachConsumer()
{
    IOService *client=powerOwner.consumer;
    powerOwner.consumer=NULL;powerOwner.ready=0;
    return client;
}
static int ownerDrop()
{
    if(!ownerAuthority(NULL,powerOwner.grant.epoch))return ownerPoison(GGPO_LOST);
    int result=powerOwner.ops.drop(powerOwner.ops.cookie,&powerOwner.grant);
    /* A successful drop terminates the authority; do not check live after it.
     * A concurrently latched revocation still makes cleanup uncertain. */
    if(result || __atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE))
        return ownerPoison(GGPO_QUARANTINED);
    powerOwner.taken=0;
    if(giulia_gpu_runtime_cancel(&powerOwner.runtime)!=GGRT_OK)
        return ownerPoison(GGPO_QUARANTINED);
    return GGPO_OK;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_register(void *source,const struct ggpo_source *ops)
{
    if(!source || !ops || ops->abi!=GGPO_ABI || ops->bytes!=sizeof(*ops) ||
       !ops->cookie || !ops->take || !ops->live || !ops->drop ||
       !gg_power_ops_valid(&ops->power))return GGPO_ARGUMENT;
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    if(powerOwner.used) {IOLockUnlock(lock);return GGPO_USED;}
    powerOwner.ops=*ops;
    ((IOService *)source)->retain();
    __atomic_store_n(&powerOwner.source,(IOService *)source,__ATOMIC_RELEASE);
    powerOwner.registered=powerOwner.used=1;
    IOLockUnlock(lock);return GGPO_OK;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_acquire(void *consumer,uint64_t epoch)
{
    if(!consumer || !epoch)return GGPO_ARGUMENT;
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    if(!powerOwner.registered) {IOLockUnlock(lock);return GGPO_UNAVAILABLE;}
    if(powerOwner.attempted || powerOwner.consumer || powerOwner.quarantined ||
       __atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE)) {
        IOLockUnlock(lock);return GGPO_USED;
    }
    powerOwner.attempted=powerOwner.mutation=1;
    powerOwner.consumer=(IOService *)consumer;powerOwner.consumer->retain();
    int result=giulia_gpu_runtime_reserve(&powerOwner.runtime,epoch);
    if(result) {
        powerOwner.mutation=0;
        if(powerOwner.runtime.quarantined || powerOwner.runtime.held || powerOwner.runtime.epoch_open) {
            result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
        }
        IOService *client=ownerDetachConsumer();powerOwner.last=GGPO_BUSY;
        IOLockUnlock(lock);client->release();return GGPO_BUSY;
    }
    if(__atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE)) {
        powerOwner.mutation=0;
        if(giulia_gpu_runtime_cancel(&powerOwner.runtime)!=GGRT_OK) {
            result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
        }
        IOService *client=ownerDetachConsumer();powerOwner.last=GGPO_HANDOFF;
        IOLockUnlock(lock);client->release();return GGPO_HANDOFF;
    }
    /* All native handoff and power callbacks run while this executor is held.
     * The source must also serialize upstream authority removal with it. */
    result=powerOwner.ops.take(powerOwner.ops.cookie,epoch,&powerScope,&powerOwner.grant);
    unsigned dirty=0;
    const unsigned char *raw=(const unsigned char *)&powerOwner.grant;
    for(unsigned i=0;i<sizeof(powerOwner.grant);i++)dirty|=raw[i];
    if(result==GGPO_UNCHANGED && !dirty) {
        if(giulia_gpu_runtime_cancel(&powerOwner.runtime)!=GGRT_OK) {
            result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
        }
        IOService *client=ownerDetachConsumer();powerOwner.last=GGPO_HANDOFF;
        IOLockUnlock(lock);client->release();return GGPO_HANDOFF;
    }
    powerOwner.taken=1; /* Includes uncertain/malformed transfers: retain. */
    if(result!=GGPO_TAKEN || !ownerGrant(epoch)) {
        result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
    }
    if(!ownerAuthority(NULL,epoch)) {
        result=ownerPoison(GGPO_LOST);IOLockUnlock(lock);return result;
    }
    const struct ggrt_ops runtimeOps={NULL,ownerAuthority,powerOwner.ops.power};
    result=giulia_gpu_runtime_acquire(&powerOwner.runtime,&runtimeOps,epoch);
    if(result) {
        if(powerOwner.runtime.quarantined || powerOwner.runtime.power.quarantined ||
           powerOwner.runtime.power.held || !powerOwner.runtime.staged ||
           powerOwner.runtime.held!=GGRT_ALL || !powerOwner.runtime.epoch_open) {
            result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
        }
        result=ownerDrop();
        if(result) {IOLockUnlock(lock);return result;}
        IOService *client=ownerDetachConsumer();powerOwner.last=GGPO_POWER;
        IOLockUnlock(lock);client->release();return GGPO_POWER;
    }
    if(!ownerAuthority(NULL,epoch)) {
        result=ownerPoison(GGPO_LOST);IOLockUnlock(lock);return result;
    }
    powerOwner.ready=1;powerOwner.last=GGPO_OK;
    IOLockUnlock(lock);return GGPO_OK;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_release(void *consumer)
{
    if(!consumer)return GGPO_ARGUMENT;
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    if(powerOwner.consumer!=consumer) {IOLockUnlock(lock);return GGPO_ARGUMENT;}
    if(powerOwner.quarantined) {IOLockUnlock(lock);return GGPO_QUARANTINED;}
    int result=giulia_gpu_runtime_release(&powerOwner.runtime);
    if(result) {
        result=ownerPoison(GGPO_QUARANTINED);IOLockUnlock(lock);return result;
    }
    result=ownerDrop();
    if(result) {IOLockUnlock(lock);return result;}
    IOService *client=ownerDetachConsumer();powerOwner.last=GGPO_OK;
    IOLockUnlock(lock);client->release();return GGPO_OK;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_invalidate(void *source)
{
    if(!source || __atomic_load_n(&powerOwner.source,__ATOMIC_ACQUIRE)!=source)
        return GGPO_ARGUMENT;
    /* No new callback after the next boundary, even while this caller waits. */
    __atomic_store_n(&powerOwner.lost,1u,__ATOMIC_RELEASE);
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    int result=powerOwner.consumer || powerOwner.taken ?
        ownerPoison(GGPO_QUARANTINED):GGPO_OK;
    IOLockUnlock(lock);return result;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_unregister(void *source)
{
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    if(!source || powerOwner.source!=source || !powerOwner.registered) {
        IOLockUnlock(lock);return GGPO_ARGUMENT;
    }
    if(powerOwner.consumer || powerOwner.taken || powerOwner.quarantined) {
        IOLockUnlock(lock);return GGPO_BUSY;
    }
    IOService *old=powerOwner.source;
    powerOwner.registered=0;
    __atomic_store_n(&powerOwner.source,(IOService *)NULL,__ATOMIC_RELEASE);
    IOLockUnlock(lock);old->release();return GGPO_OK;
}
extern "C" __attribute__((used)) int giulia_gpu_power_owner_status(struct ggpo_status *out)
{
    if(!out)return GGPO_ARGUMENT;
    *out={};
    IOLock *lock=ownerLock();if(!lock)return GGPO_UNAVAILABLE;
    IOLockLock(lock);
    out->epoch=powerOwner.grant.epoch;out->serial=powerOwner.grant.serial;
    out->registered=powerOwner.registered;out->attempted=powerOwner.attempted;
    out->grant_held=powerOwner.taken;out->ready=powerOwner.ready;
    out->revoked=__atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE);
    out->quarantined=powerOwner.quarantined;
    out->power_held=powerOwner.runtime.power.held;
    out->mutation_attempted=powerOwner.mutation;out->last_code=powerOwner.last;
    IOLockUnlock(lock);return GGPO_OK;
}
#endif
