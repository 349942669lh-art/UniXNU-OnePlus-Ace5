/* Execute the real resident coordinator with RAM-only provider/lock doubles.
 * Fixture reset is NOT a kernel API and never demonstrates hardware reset. */
#define GG_HOST_TEST 1
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
struct IOLock { std::mutex mutex; };
static unsigned lockAllocations;
static IOLock *IOLockAlloc(){++lockAllocations;return new IOLock;}
static void IOLockFree(IOLock *p){delete p;}
static void IOLockLock(IOLock *p){p->mutex.lock();}
static void IOLockUnlock(IOLock *p){p->mutex.unlock();}
struct IOService {
    std::atomic<unsigned> references{1};
    void retain(){++references;}
    void release(){assert(references.fetch_sub(1)>1);}
};
#include "../ios16/gpu/giulia_gpu_power_owner.cpp"
static struct gpr_broker broker;
static std::mutex brokerMutex;
static bool loseDuringReservation;
#define BROKER(expr) std::lock_guard<std::mutex> guard(brokerMutex);return (expr)
extern "C" int gpr_global_initialize(){return GPR_OK;}
extern "C" int gpr_global_open_epoch(const void *o,uint64_t e){BROKER(gpr_open_epoch(&broker,o,e));}
extern "C" int gpr_global_retire_epoch(const void *o,uint64_t e){BROKER(gpr_retire_epoch(&broker,o,e));}
extern "C" int gpr_global_claim(const gpr_key *k,const void *o,uint64_t e,unsigned f,uint64_t v,gpr_token *t)
{
    if(loseDuringReservation)__atomic_store_n(&powerOwner.lost,1u,__ATOMIC_RELEASE);
    BROKER(gpr_claim(&broker,k,o,e,f,v,t));
}
extern "C" int gpr_global_owns(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_owns(&broker,t,o,e));}
extern "C" int gpr_global_release(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_release(&broker,t,o,e));}
extern "C" int gpr_global_quarantine(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_quarantine(&broker,t,o,e));}
extern "C" int gpr_global_mark_dma(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_mark_dma(&broker,t,o,e));}
extern "C" int gpr_global_quiesce(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_quiesce(&broker,t,o,e));}
#undef BROKER

struct Fixture {
    IOService provider,consumer,foreign;
    bool valid=true,dropFailure=false,resolveFailure=false;
    int takeResult=GGPO_TAKEN,badGrant=-1,failResource=-1,uncertainResource=-1;
    int releaseFailure=-1,liveLostAt=-1;
    unsigned takes=0,drops=0,lives=0,acquires=0,releases=0,observes=0;
    std::vector<unsigned> order;
    std::mutex mutex;std::condition_variable condition;
    bool block=false,entered=false,proceed=false;
    static int take(void *p,uint64_t epoch,const ggpo_scope *scope,ggpo_grant *out) {
        auto &f=*(Fixture *)p;++f.takes;
        assert(scope && ownerScope(scope) && epoch==17);
        assert(powerOwner.runtime.staged && powerOwner.runtime.held==GGRT_ALL);
        for(unsigned i=0;i<GGRT_LEASES;i++)
            assert(gpr_global_owns(&powerOwner.runtime.leases[i],&powerOwner.runtime,epoch));
        if(f.takeResult==GGPO_UNCHANGED && f.badGrant<0)return GGPO_UNCHANGED;
        *out={};out->epoch=epoch;out->serial=39;out->self_vmid=5;out->scope=*scope;
        switch(f.badGrant) {
        case 0:out->epoch++;break;
        case 1:out->serial=0;break;
        case 2:out->self_vmid=0;break;
        case 3:out->self_vmid=65535;break;
        case 4:out->reserved=1;break;
        case 5:out->scope.reserved=1;break;
        case 6:out->scope.sid_mask|=4;break; // Never accept secure SID 2.
        case 7:out->scope.prerequisites^=1;break;
        case 8:out->scope.prerequisites|=0x40;break;
        default:
            if(f.badGrant>=9 && f.badGrant<12)out->scope.intids[f.badGrant-9]++;
            if(f.badGrant>=12 && f.badGrant<18)out->scope.windows[f.badGrant-12].base+=4;
            if(f.badGrant>=18 && f.badGrant<24)out->scope.windows[f.badGrant-18].bytes+=4;
        }
        return f.takeResult;
    }
    static int live(void *p,const ggpo_grant *g) {
        auto &f=*(Fixture *)p;++f.lives;assert(g->serial==39);
        if((int)f.lives==f.liveLostAt)f.valid=false;
        return f.valid?1:0;
    }
    static int drop(void *p,const ggpo_grant *g) {
        auto &f=*(Fixture *)p;++f.drops;
        assert(g->epoch==17 && f.releases==f.acquires);
        // Exclusion must span DROP too, not just the power transaction.
        assert(powerOwner.runtime.staged && powerOwner.runtime.held==GGRT_ALL);
        for(unsigned i=0;i<GGRT_LEASES;i++)
            assert(gpr_global_owns(&powerOwner.runtime.leases[i],&powerOwner.runtime,g->epoch));
        return f.dropFailure?1:0;
    }
    static int resolve(void *p){return ((Fixture *)p)->resolveFailure?-1:0;}
    static int quiet(void *){return 1;}
    static gg_resource_result acquire(void *p,unsigned id,uint32_t value,uint64_t *token) {
        auto &f=*(Fixture *)p;
        assert(value==(id==GGR_GMU_RATE?625000000u:id==GGR_HUB_RATE?200000000u:1u));
        if((int)id==f.failResource)return GGRC_UNCHANGED_ERROR;
        ++f.acquires;f.order.push_back(id);*token=100+id;
        if(f.block) {
            std::unique_lock<std::mutex> lock(f.mutex);f.entered=true;f.condition.notify_all();
            assert(f.condition.wait_for(lock,std::chrono::seconds(3),[&]{return f.proceed;}));
        }
        return (int)id==f.uncertainResource?GGRC_UNCERTAIN:GGRC_OK;
    }
    static int release(void *p,unsigned id,uint64_t token) {
        auto &f=*(Fixture *)p;
        assert(token==100+id && !f.order.empty() && id==f.order.back());
        if((int)id==f.releaseFailure)return -1;
        f.order.pop_back();++f.releases;return 0;
    }
    static int observe(void *p){++((Fixture *)p)->observes;return 1;}
    static int wait(void *,unsigned){return 0;}
    ggpo_source ops() {
        return {GGPO_ABI,sizeof(ggpo_source),this,take,live,drop,
            {this,resolve,quiet,acquire,release,observe,wait,nullptr}};
    }
    void bind() {
        auto source=ops();assert(giulia_gpu_power_owner_register(&provider,&source)==GGPO_OK);
        assert(provider.references==2 && consumer.references==1);
        source.take=nullptr; // Production must have copied the caller's table.
    }
    ggpo_status status() {
        ggpo_status out={};unsigned calls=lives;
        assert(giulia_gpu_power_owner_status(&out)==GGPO_OK);
        assert(lives==calls);return out; // CPU-only status must not call live.
    }
    void resetRAMOnly() {
        // No threads remain here. This is not an available production reset.
        if(powerOwner.consumer)powerOwner.consumer->release();
        if(powerOwner.source)powerOwner.source->release();
        powerOwner={};broker={};loseDuringReservation=false;
        assert(provider.references==1 && consumer.references==1 && foreign.references==1);
    }
};
static unsigned cases;
int main() {
    ggpo_status empty={};IOService temporary;
    assert(giulia_gpu_power_owner_status(&empty)==GGPO_UNAVAILABLE);
    assert(!lockAllocations);
    assert(giulia_gpu_power_owner_acquire(&temporary,17)==GGPO_UNAVAILABLE);
    assert(giulia_gpu_power_owner_initialize()==GGPO_OK);
    assert(giulia_gpu_power_owner_initialize()==GGPO_OK && lockAllocations==1);
    assert(giulia_gpu_power_owner_acquire(&temporary,17)==GGPO_UNAVAILABLE);++cases;
    {
        struct ggrt_state state={};
        assert(giulia_gpu_runtime_reserve(&state,17)==GGRT_OK && state.staged);
        assert(!giulia_gpu_runtime_live(&state) && !state.power.attempted);
        assert(giulia_gpu_runtime_cancel(&state)==GGRT_OK && !state.held && !state.epoch_open);
        broker={};++cases;
    }
    {
        Fixture f;f.bind();
        const struct gpr_key key={GPR_MMIO,0,0,0,0x3d00000,0x40000};
        struct gpr_token token={};
        assert(gpr_global_open_epoch(&f.foreign,9)==GPR_OK);
        assert(gpr_global_claim(&key,&f.foreign,9,0,0,&token)==GPR_OK);
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_BUSY);
        assert(!f.takes && !f.acquires && !f.status().mutation_attempted);
        assert(f.consumer.references==1 && gpr_global_owns(&token,&f.foreign,9));
        assert(gpr_global_release(&token,&f.foreign,9)==GPR_OK);
        f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.bind();loseDuringReservation=true;
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_HANDOFF);
        assert(!f.takes && !f.acquires && !f.status().mutation_attempted);
        assert(f.status().revoked && f.consumer.references==1 && !powerOwner.runtime.held);
        f.resetRAMOnly();++cases;
    }
    for(unsigned error=0;error<10;error++) {
        Fixture f;auto ops=f.ops();
        switch(error) {
        case 0:ops.abi++;break;case 1:ops.bytes--;break;case 2:ops.cookie=nullptr;break;
        case 3:ops.take=nullptr;break;case 4:ops.live=nullptr;break;case 5:ops.drop=nullptr;break;
        case 6:ops.power.acquire=nullptr;break;case 7:ops.power.release=nullptr;break;
        case 8:ops.power.quiescent=nullptr;break;case 9:ops.power.observe=nullptr;break;
        }
        assert(giulia_gpu_power_owner_register(&f.provider,&ops)==GGPO_ARGUMENT);
        assert(!f.takes && f.provider.references==1);f.resetRAMOnly();++cases;
    }
    for(int bad=0;bad<24;bad++) {
        Fixture f;f.badGrant=bad;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_QUARANTINED);
        auto s=f.status();assert(s.quarantined && !s.ready && s.grant_held);
        assert(!f.acquires && !f.drops && f.consumer.references==2);
        assert(giulia_gpu_power_owner_unregister(&f.provider)==GGPO_BUSY);
        assert(giulia_gpu_power_owner_release(&f.consumer)==GGPO_QUARANTINED);
        f.resetRAMOnly();++cases;
    }
    for(int result:{(int)GGPO_UNCHANGED,(int)GGPO_UNCERTAIN,-1}) {
        Fixture f;f.takeResult=result;f.bind();
        int code=giulia_gpu_power_owner_acquire(&f.consumer,17);
        if(result==GGPO_UNCHANGED) {
            assert(code==GGPO_HANDOFF && f.consumer.references==1);
            assert(giulia_gpu_power_owner_unregister(&f.provider)==GGPO_OK);
        } else assert(code==GGPO_QUARANTINED && f.consumer.references==2);
        assert(!f.acquires && !f.drops);f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.takeResult=GGPO_UNCHANGED;f.badGrant=4;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_QUARANTINED);
        assert(!f.drops && f.consumer.references==2);f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.bind();
        assert(giulia_gpu_power_owner_acquire(nullptr,17)==GGPO_ARGUMENT);
        assert(giulia_gpu_power_owner_acquire(&f.consumer,0)==GGPO_ARGUMENT);
        assert(giulia_gpu_power_owner_invalidate(&f.foreign)==GGPO_ARGUMENT);
        assert(!f.status().revoked && !f.takes);
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_OK);
        auto s=f.status();assert(s.ready && s.grant_held && s.power_held==((1u<<GGR_COUNT)-1));
        assert(f.acquires==GGR_COUNT && f.observes==1);
        assert(giulia_gpu_runtime_cancel(&powerOwner.runtime)==GGRT_BUSY);
        assert(giulia_gpu_power_owner_unregister(&f.provider)==GGPO_BUSY);
        assert(giulia_gpu_power_owner_release(&f.foreign)==GGPO_ARGUMENT);
        assert(giulia_gpu_power_owner_release(&f.consumer)==GGPO_OK);
        s=f.status();assert(!s.ready && !s.grant_held && !s.power_held);
        assert(f.releases==GGR_COUNT && f.drops==1 && f.consumer.references==1);
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_USED);
        assert(giulia_gpu_power_owner_unregister(&f.provider)==GGPO_OK);
        auto ops=f.ops();assert(giulia_gpu_power_owner_register(&f.provider,&ops)==GGPO_USED);
        f.resetRAMOnly();++cases;
    }
    for(int id=0;id<GGR_COUNT;id++) {
        Fixture f;f.failResource=id;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_POWER);
        assert(f.acquires==(unsigned)id && f.releases==(unsigned)id && f.drops==1);
        assert(!f.status().quarantined && f.consumer.references==1);
        f.resetRAMOnly();++cases;
    }
    for(int id=0;id<GGR_COUNT;id++) {
        Fixture f;f.uncertainResource=id;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_QUARANTINED);
        assert(f.acquires==(unsigned)id+1 && !f.releases && !f.drops);
        assert(f.status().power_held==((1u<<(id+1))-1));
        f.resetRAMOnly();++cases;
    }
    for(int id=0;id<GGR_COUNT;id++) {
        Fixture f;f.releaseFailure=id;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_OK);
        assert(giulia_gpu_power_owner_release(&f.consumer)==GGPO_QUARANTINED);
        assert(f.releases==GGR_COUNT-1-(unsigned)id && !f.drops && f.consumer.references==2);
        f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.resolveFailure=true;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_POWER);
        assert(!f.acquires && f.drops==1);f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.dropFailure=true;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_OK);
        assert(giulia_gpu_power_owner_release(&f.consumer)==GGPO_QUARANTINED);
        assert(f.releases==GGR_COUNT && f.status().grant_held && f.consumer.references==2);
        f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.liveLostAt=1;f.bind();
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_LOST);
        assert(!f.acquires && !f.drops && f.status().quarantined);
        f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.bind();assert(giulia_gpu_power_owner_invalidate(&f.provider)==GGPO_OK);
        assert(giulia_gpu_power_owner_acquire(&f.consumer,17)==GGPO_USED && !f.takes);
        assert(giulia_gpu_power_owner_unregister(&f.provider)==GGPO_OK);
        f.resetRAMOnly();++cases;
    }
    {
        Fixture f;f.block=true;f.bind();
        std::atomic<bool> acknowledged{false};
        int startCode=-1,revokeCode=-1;
        std::thread worker([&]{startCode=giulia_gpu_power_owner_acquire(&f.consumer,17);});
        {
            std::unique_lock<std::mutex> lock(f.mutex);
            assert(f.condition.wait_for(lock,std::chrono::seconds(3),[&]{return f.entered;}));
        }
        std::thread revoker([&]{revokeCode=giulia_gpu_power_owner_invalidate(&f.provider);acknowledged=true;});
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!__atomic_load_n(&powerOwner.lost,__ATOMIC_ACQUIRE)) {
            assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();
        }
        assert(!acknowledged); // Revocation must wait for the active callback.
        {std::lock_guard<std::mutex> lock(f.mutex);f.proceed=true;f.condition.notify_all();}
        worker.join();revoker.join();
        assert(startCode==GGPO_QUARANTINED && revokeCode==GGPO_QUARANTINED);
        assert(f.acquires==1 && !f.releases && !f.drops && acknowledged);
        auto s=f.status();assert(s.quarantined && s.revoked && !s.ready && s.power_held==1);
        assert(f.provider.references==2 && f.consumer.references==2);
        f.resetRAMOnly();++cases;
    }
    assert(lockAllocations==1);IOLockFree(powerOwnerLock);powerOwnerLock=nullptr;
    std::printf("Resident power owner: %u RAM-only cases passed; real coordinator, lock/provider doubles, no hardware grant or clock tested\n",cases);
}
