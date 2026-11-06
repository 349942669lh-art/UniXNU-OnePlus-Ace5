/* Actual native executor + actual core/global broker algorithms. All IOKit,
 * mapping and native authority objects below are RAM-only host doubles. */
#define GG_HOST_TEST 1
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
struct IOLock { std::mutex mutex; };
static unsigned lockAllocations;
static IOLock *IOLockAlloc(){++lockAllocations;return new IOLock;}
static void IOLockFree(IOLock *p){delete p;}
static void IOLockLock(IOLock *p){p->mutex.lock();}
static void IOLockUnlock(IOLock *p){p->mutex.unlock();}
struct IOService {
    unsigned references=1;
    void retain(){++references;}
    void release(){assert(references>1);--references;}
};
static constexpr unsigned kIODirectionInOut=3,kIOMapInhibitCache=0x100;
static unsigned descCreated,descFreed,mapCreated,mapFreed,accessCalls,delays;
static unsigned mapFault;
static uint32_t registerRAM;
static void nativeAccess();
static void delayFixture(unsigned);
static void unmapFixture();
struct IOMemoryMap {
    uintptr_t getVirtualAddress(){++accessCalls;nativeAccess();
        return mapFault==3?0:(uintptr_t)&registerRAM+(mapFault==4?1:0);}
    uint64_t getLength(){return mapFault==5?2048:4096;}
    void release(){unmapFixture();++mapFreed;delete this;}
};
struct IOMemoryDescriptor {
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t base,uint64_t bytes,unsigned flags){
        assert(base==0x152000 && bytes==4096 && flags==kIODirectionInOut);
        nativeAccess();if(mapFault==1)return nullptr;++descCreated;return new IOMemoryDescriptor;
    }
    IOMemoryMap *map(unsigned flags){assert(flags==kIOMapInhibitCache);nativeAccess();
        if(mapFault==2)return nullptr;++mapCreated;return new IOMemoryMap;}
    void release(){++descFreed;delete this;}
};
static void IODelay(unsigned us){delayFixture(us);}
#define IOLog(...) ((void)0)
#include "../ios16/gpu/giulia_gpu_parents_native.cpp"
static struct gpr_broker broker;
static std::mutex brokerMutex;
#define BROKER(expr) std::lock_guard<std::mutex> guard(brokerMutex);return (expr)
extern "C" int gpr_global_initialize(){return GPR_OK;}
extern "C" int gpr_global_open_epoch(const void *o,uint64_t e){BROKER(gpr_open_epoch(&broker,o,e));}
extern "C" int gpr_global_retire_epoch(const void *o,uint64_t e){BROKER(gpr_retire_epoch(&broker,o,e));}
extern "C" int gpr_global_claim(const gpr_key *k,const void *o,uint64_t e,unsigned f,uint64_t v,gpr_token *t){BROKER(gpr_claim(&broker,k,o,e,f,v,t));}
extern "C" int gpr_global_owns(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_owns(&broker,t,o,e));}
extern "C" int gpr_global_release(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_release(&broker,t,o,e));}
extern "C" int gpr_global_quarantine(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_quarantine(&broker,t,o,e));}
#undef BROKER
struct Fixture {
    IOService source,foreign;
    int enterResult=1,quietResult=1,leaveResult=0;
    bool valid=true,held=false,block=false,entered=false,proceed=false,detachTest=false,latchInLeave=false;
    unsigned enters=0,leaves=0,lives=0,quiets=0,loseAt=0;
    std::mutex mutex;std::condition_variable condition;
    static int enter(void *p,uint64_t e,const ggcp_scope *scope){auto &f=*(Fixture *)p;++f.enters;
        assert(!f.held && e==17 && scope->page==0x152000 && scope->bytes==4096 && scope->offset==0);
        assert(scope->mask==0x18000 && scope->settle_us==10 && scope->parent_hz==600000000 && scope->div_hz==300000000);
        assert(gpr_global_owns(&parents.page,&parents,e));
        if(f.enterResult==1)f.held=true;return f.enterResult;
    }
    static int live(void *p,uint64_t e){auto &f=*(Fixture *)p;assert(f.held && e==17);++f.lives;
        if(f.lives==f.loseAt)f.valid=false;return f.valid?1:0;}
    static int quiet(void *p,uint64_t e){auto &f=*(Fixture *)p;assert(f.held && e==17);++f.quiets;return f.quietResult;}
    static int leave(void *p,uint64_t e){auto &f=*(Fixture *)p;assert(f.held && e==17);++f.leaves;f.held=false;
        if(f.latchInLeave){unsigned bound=1;assert(__atomic_compare_exchange_n(&parents.lifecycle,&bound,2u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE));}
        return f.leaveResult;
    }
    ggcp_authority authority(){return {GGCP_ABI,sizeof(ggcp_authority),this,enter,live,quiet,leave};}
    int open(){auto a=authority();int code=giulia_gpu_parents_open(&source,17,&a);a.enter=nullptr;return code;}
    ggcp_status status(){auto a=accessCalls,b=lives,c=enters,d=delays;ggcp_status out={};
        assert(giulia_gpu_parents_status(&out)==GGCP_OK && accessCalls==a && lives==b && enters==c && delays==d);return out;}
    void checkPinned(){auto s=status();assert(s.bound && s.quarantined && !s.ready_cached && source.references==2);
        assert(parents.leased && broker.leases[parents.page.slot].state==GPR_ISOLATED);
        unsigned accesses=accessCalls,callbacks=enters;uint64_t ticket=99;
        assert(giulia_gpu_parents_acquire(&source,0,&ticket)==GGCP_QUARANTINED && !ticket);
        assert(giulia_gpu_parents_close(&source)==GGCP_QUARANTINED && callbacks==enters && accesses==accessCalls);
    }
};
static Fixture *active;
static void nativeAccess(){assert(active && active->held && active->valid);
    assert(parents.guard && parents.leased && gpr_global_owns(&parents.page,&parents,17));}
static void delayFixture(unsigned us){assert(us==10);nativeAccess();++delays;
    auto &f=*active;if(f.block){std::unique_lock<std::mutex> lock(f.mutex);f.entered=true;f.condition.notify_all();
        assert(f.condition.wait_for(lock,std::chrono::seconds(5),[&]{return f.proceed;}));}}
static void unmapFixture(){assert(parents.leased && gpr_token_valid(&broker,&parents.page,&parents,17));
    if(active->detachTest)assert(giulia_gpu_parents_invalidate(&active->source)==GGCP_ARGUMENT);}
static void resetRAMOnly(Fixture &f){
    /* Deliberately test-only destruction of quarantined RAM; never a target API. */
    f.detachTest=false;
    if(parents.map){parents.map->release();parents.map=nullptr;}
    if(parents.memory){parents.memory->release();parents.memory=nullptr;}
    if(parents.source)parents.source->release();
    IOLockFree(parents.lock);parents={};broker={};active=nullptr;
    assert(f.source.references==1 && f.foreign.references==1 && descCreated==descFreed && mapCreated==mapFreed);
    registerRAM=mapFault=accessCalls=delays=0;
}
static void setup(Fixture &f){active=&f;assert(giulia_gpu_parents_initialize()==GGCP_OK);}
static unsigned cases;
int main(){
    ggcp_status s={};IOService unrelated;uint64_t a=0,b=0;
    assert(giulia_gpu_parents_status(&s)==GGCP_UNAVAILABLE && !lockAllocations);
    assert(giulia_gpu_parents_acquire(&unrelated,0,&a)==GGCP_UNAVAILABLE && !lockAllocations);++cases;
    for(unsigned fault=0;fault<9;fault++){
        Fixture f;setup(f);auto ops=f.authority();
        switch(fault){case 0:ops.abi++;break;case 1:ops.bytes--;break;case 2:ops.cookie=nullptr;break;
            case 3:ops.enter=nullptr;break;case 4:ops.live=nullptr;break;case 5:ops.quiescent=nullptr;break;
            case 6:ops.leave=nullptr;break;default:break;}
        assert(giulia_gpu_parents_open(fault==7?nullptr:&f.source,fault==8?0:17,&ops)==GGCP_ARGUMENT);
        assert(!parents.used && !accessCalls && !f.enters && f.source.references==1);
        resetRAMOnly(f);++cases;
    }
    {
        Fixture f;setup(f);unsigned locks=lockAllocations;
        assert(giulia_gpu_parents_initialize()==GGCP_OK && lockAllocations==locks);
        assert(f.open()==GGCP_OK && f.source.references==2 && parents.memory && parents.map);
        auto out=f.status();assert(out.ready_cached && !out.write_attempted && out.epoch==17);
        assert(f.open()==GGCP_USED);
        assert(giulia_gpu_parents_acquire(&f.foreign,0,&a)==GGCP_ARGUMENT && !a);
        assert(giulia_gpu_parents_acquire(&f.source,2,&a)==GGCP_ARGUMENT);
        assert(giulia_gpu_parents_acquire(&f.source,0,nullptr)==GGCP_ARGUMENT);
        assert(giulia_gpu_parents_invalidate(&f.foreign)==GGCP_ARGUMENT);
        assert(giulia_gpu_parents_acquire(&f.source,0,&a)==GGCP_OK && a);
        assert(giulia_gpu_parents_acquire(&f.source,1,&b)==GGCP_OK && b!=a && registerRAM==GGCP_MASK);
        assert(giulia_gpu_parents_close(&f.source)==GGCP_BUSY);
        f.quietResult=0;assert(giulia_gpu_parents_release(&f.source,a)==GGCP_BUSY && f.status().main_refs==1);
        f.quietResult=1;assert(giulia_gpu_parents_release(&f.source,a)==GGCP_OK);
        f.enterResult=0;unsigned accesses=accessCalls;
        assert(giulia_gpu_parents_release(&f.source,b)==GGCP_BUSY && accesses==accessCalls && !f.status().quarantined);
        f.enterResult=1;assert(giulia_gpu_parents_release(&f.source,b)==GGCP_OK && registerRAM==0);
        f.detachTest=true;assert(giulia_gpu_parents_close(&f.source)==GGCP_OK);
        assert(!f.status().bound && !parents.leased && !parents.map && f.source.references==1);
        assert(f.enters==f.leaves+1 && !f.held); /* One unchanged denied guard. */
        assert(giulia_gpu_parents_invalidate(&f.source)==GGCP_ARGUMENT);
        resetRAMOnly(f);++cases;
    }
    {
        Fixture f;setup(f);gpr_token t={};auto key=parentPage;
        assert(gpr_global_open_epoch(&f.foreign,9)==GPR_OK);
        assert(gpr_global_claim(&key,&f.foreign,9,0,0,&t)==GPR_OK);
        assert(f.open()==GGCP_BUSY && !accessCalls && !f.enters && !parents.source);
        assert(gpr_global_owns(&t,&f.foreign,9));resetRAMOnly(f);++cases;
    }
    for(unsigned fault=1;fault<=5;fault++){
        Fixture f;setup(f);mapFault=fault;
        assert(f.open()==GGCP_UNAVAILABLE && !f.status().bound && !parents.leased && !parents.map);
        assert(f.enters==1 && f.leaves==1 && !f.status().write_attempted);
        resetRAMOnly(f);++cases;
    }
    for(unsigned bits=1;bits<4;bits++){
        Fixture f;setup(f);registerRAM=bits<<15;
        assert(f.open()==GGCP_INHERITED && !f.status().bound && !parents.leased && registerRAM==(bits<<15));
        assert(!delays && !f.status().write_attempted);resetRAMOnly(f);++cases;
    }
    for(int result: {0,-1,2}){
        Fixture f;setup(f);f.enterResult=result;
        assert(f.open()==(result?GGCP_QUARANTINED:GGCP_BUSY));
        assert(!accessCalls && !f.leaves && !parents.map);
        if(result)f.checkPinned();else assert(f.source.references==1 && !parents.leased);
        resetRAMOnly(f);++cases;
    }
    for(unsigned operation=0;operation<3;operation++){
        Fixture baseline;setup(baseline);if(operation)assert(baseline.open()==GGCP_OK);
        if(operation==2)assert(giulia_gpu_parents_acquire(&baseline.source,0,&a)==GGCP_OK);
        unsigned before=baseline.lives;
        assert((!operation?baseline.open():operation==1?giulia_gpu_parents_acquire(&baseline.source,0,&a):
            giulia_gpu_parents_release(&baseline.source,a))==GGCP_OK);
        unsigned total=baseline.lives-before;resetRAMOnly(baseline);
        for(unsigned boundary=1;boundary<=total;boundary++){
            Fixture f;setup(f);if(operation)assert(f.open()==GGCP_OK);
            if(operation==2)assert(giulia_gpu_parents_acquire(&f.source,0,&a)==GGCP_OK);
            f.loseAt=f.lives+boundary;
            assert((!operation?f.open():operation==1?giulia_gpu_parents_acquire(&f.source,0,&a):
                giulia_gpu_parents_release(&f.source,a))==GGCP_QUARANTINED);
            assert(!f.valid && !f.held && f.enters==f.leaves);f.checkPinned();resetRAMOnly(f);++cases;
        }
    }
    {
        Fixture f;setup(f);assert(f.open()==GGCP_OK);
        /* Internal test of the exact detach/revoke linearization point,
         * with revoke winning after a hypothetical successful close. */
        IOLockLock(parents.lock);assert(parentEnter()==GGCP_OK);
        assert(ggcp_close(&parents.core)==GGCP_OK && parentLeave(GGCP_OK)==GGCP_OK);
        IOLockUnlock(parents.lock);
        __atomic_store_n(&parents.lifecycle,2u,__ATOMIC_RELEASE);
        unsigned unmapped=mapFreed;IOLockLock(parents.lock);
        assert(parentCleanup()==nullptr);IOLockUnlock(parents.lock);
        assert(mapFreed==unmapped && parents.map && parents.memory);
        f.checkPinned();resetRAMOnly(f);++cases;
    }
    {
        Fixture f;setup(f);assert(f.open()==GGCP_OK);
        assert(gpr_global_quarantine(&parents.page,&parents,17)==GPR_OK);
        unsigned accesses=accessCalls;
        assert(giulia_gpu_parents_acquire(&f.source,0,&a)==GGCP_QUARANTINED && !a);
        assert(accesses==accessCalls);f.checkPinned();resetRAMOnly(f);++cases;
    }
    for(unsigned fault=0;fault<4;fault++){
        Fixture f;setup(f);assert(f.open()==GGCP_OK);
        if(fault==0)f.leaveResult=1;
        if(fault==1)f.latchInLeave=true;
        if(fault>=2){assert(giulia_gpu_parents_acquire(&f.source,0,&a)==GGCP_OK);
            if(fault==2)f.quietResult=-1;else registerRAM=0;
            assert(giulia_gpu_parents_release(&f.source,a)==GGCP_QUARANTINED);
        }else assert(giulia_gpu_parents_acquire(&f.source,0,&a)==GGCP_QUARANTINED);
        f.checkPinned();assert(parents.map && parents.memory);resetRAMOnly(f);++cases;
    }
    {
        Fixture f;setup(f);assert(f.open()==GGCP_OK);f.block=true;
        int acquireCode=-1,invalidateCode=-1;std::atomic<bool> finished{false};
        std::thread worker([&]{acquireCode=giulia_gpu_parents_acquire(&f.source,0,&a);});
        {std::unique_lock<std::mutex> lock(f.mutex);
            assert(f.condition.wait_for(lock,std::chrono::seconds(5),[&]{return f.entered;}));}
        unsigned accessesBeforeLoss=accessCalls;
        std::thread invalidator([&]{invalidateCode=giulia_gpu_parents_invalidate(&f.source);finished=true;});
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(__atomic_load_n(&parents.lifecycle,__ATOMIC_ACQUIRE)!=2){
            assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
        assert(!finished.load()); /* Invalidate waits for the executing callback. */
        {std::lock_guard<std::mutex> lock(f.mutex);f.proceed=true;f.condition.notify_all();}
        worker.join();invalidator.join();
        assert(acquireCode==GGCP_QUARANTINED && invalidateCode==GGCP_QUARANTINED && a);
        assert(accessCalls==accessesBeforeLoss); /* No readback after latched loss. */
        assert(f.status().revoked && f.status().main_refs==1 && registerRAM==(1u<<15));
        f.checkPinned();resetRAMOnly(f);++cases;
    }
    printf("GCC GPU parents native: %u RAM-only cases passed; hardware_verified=0\n",cases);
}
