/* Actual production executor + real C transport/broker algorithms.
 * All IOKit, authority and MMIO objects below are RAM-only host doubles.
 * No phone/physical mapping, HVC, SCM, firmware or native grant is used. */
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
static unsigned allocatedLocks;
static bool lockFail;
static IOLock *IOLockAlloc(){if(lockFail)return nullptr;++allocatedLocks;return new IOLock;}
static void IOLockFree(IOLock *p){delete p;}
static void IOLockLock(IOLock *p){p->mutex.lock();}
static void IOLockUnlock(IOLock *p){p->mutex.unlock();}
struct IOService {unsigned refs=1;void retain(){++refs;}void release(){assert(refs>1);--refs;}};
static constexpr unsigned kIODirectionInOut=3,kIOMapInhibitCache=0x100;
alignas(16) static uint32_t ram[0x10000/4];
static unsigned mapFault,descCreated,descFreed,mapCreated,mapFreed,mapAccesses;
static bool manualReset;
static void accessFixture();
static void unmapFixture();
static uint64_t hostTime;
struct IOMemoryMap {
    uintptr_t getVirtualAddress(){++mapAccesses;accessFixture();
        return mapFault==3?0:(uintptr_t)ram+(mapFault==4?1:0);}
    uint64_t getLength(){return mapFault==5?0x8000:0x10000;}
    void release(){unmapFixture();++mapFreed;delete this;}
};
struct IOMemoryDescriptor {
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t base,uint64_t bytes,unsigned flags){
        assert(base==0x17a20000 && bytes==0x10000 && flags==kIODirectionInOut);
        accessFixture();if(mapFault==1)return nullptr;++descCreated;return new IOMemoryDescriptor;}
    IOMemoryMap *map(unsigned flags){assert(flags==kIOMapInhibitCache);accessFixture();
        if(mapFault==2)return nullptr;
        ++mapCreated;return new IOMemoryMap;}
    void release(){++descFreed;delete this;}
};
static uint64_t mach_absolute_time(){return hostTime;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *out){*out=t;}
static void IODelay(unsigned us){assert(us==1);hostTime+=1000;}
#define IOLog(...) ((void)0)
#include "../ios16/gpu/giulia_gpu_rsc_native.cpp"
static gpr_broker broker;
static std::mutex brokerMutex;
static unsigned brokerFault;
#define BROKER(expr) std::lock_guard<std::mutex> guard(brokerMutex);return (expr)
extern "C" int gpr_global_initialize(){return brokerFault==1?GPR_BUSY:GPR_OK;}
extern "C" int gpr_global_open_epoch(const void *o,uint64_t e){BROKER(brokerFault==2?GPR_BUSY:gpr_open_epoch(&broker,o,e));}
extern "C" int gpr_global_retire_epoch(const void *o,uint64_t e){BROKER(brokerFault==5?GPR_BUSY:gpr_retire_epoch(&broker,o,e));}
extern "C" int gpr_global_claim(const gpr_key *k,const void *o,uint64_t e,unsigned f,uint64_t v,gpr_token *t){BROKER(brokerFault==3?GPR_BUSY:gpr_claim(&broker,k,o,e,f,v,t));}
extern "C" int gpr_global_owns(const gpr_token *t,const void *o,uint64_t e){BROKER(brokerFault==6?0:gpr_owns(&broker,t,o,e));}
extern "C" int gpr_global_release(const gpr_token *t,const void *o,uint64_t e){BROKER(brokerFault==4?GPR_BUSY:gpr_release(&broker,t,o,e));}
extern "C" int gpr_global_quarantine(const gpr_token *t,const void *o,uint64_t e){BROKER(gpr_quarantine(&broker,t,o,e));}
#undef BROKER
struct Fixture {
    IOService source,foreign;
    ggrsc_config config={GGRSC_ABI,GGRSC_V30,0xf300,0,0x30010,0x1fe,100,0,17};
    int takeResult=1,approveResult=1,settledResult=1,dropResult=0;
    uint64_t offeredGrant=61;
    bool granted=false,valid=true,complete=true,started=false,finished=false;
    bool unmapRace=false,dropRace=false,block=false,entered=false,proceed=false;
    bool alterCaller=false;
    ggrn_authority *callerOps=nullptr;
    unsigned takes=0,lives=0,approvals=0,settles=0,drops=0,starts=0,loseAt=0;
    std::mutex mutex;std::condition_variable condition;
    uint32_t &reg(unsigned offset){return ram[(0xd00+offset)/4];}
    unsigned control(){return 672*config.tcs_id+(config.version==30?0x24:0x14);}
    unsigned status(){return control()+4;}
    unsigned cmdstatus(){return 672*config.tcs_id+(config.version==30?0x40:0x3c);}
    static void scope(const ggrn_scope *s){assert(s->drv_base==0x17a20000 && s->drv_bytes==0x10000 &&
        s->drv_id==2 && s->tcs_offset==0xd00 && s->intid==37 && s->active_mask==7 && !s->reserved &&
        s->transport.epoch==17 && s->transport.mapped_bytes==0xf300);}
    static int take(void *p,const ggrn_scope *s,uint64_t *grant){auto &f=*(Fixture *)p;scope(s);++f.takes;
        assert(!f.granted && gpr_global_owns(&nativeRsc.page,&nativeRsc,17));
        *grant=f.offeredGrant;f.granted=f.takeResult!=0 || f.offeredGrant!=0;
        if(f.alterCaller){f.config.epoch=99;f.config.mapped_bytes=0;
            assert(f.callerOps);f.callerOps->live=nullptr;f.callerOps->approve=nullptr;}
        return f.takeResult;}
    static int live(void *p,uint64_t e,uint64_t grant){auto &f=*(Fixture *)p;assert(f.granted && e==17 && grant==61);
        ++f.lives;if(f.lives==f.loseAt)f.valid=false;return f.valid?1:0;}
    static int approve(void *p,const ggrn_scope *s,uint64_t grant,uint32_t selector){auto &f=*(Fixture *)p;
        scope(s);assert(f.granted && grant==61 && selector<16 && !nativeRsc.core.inflight);++f.approvals;
        return f.approveResult;}
    static int settled(void *p,const ggrn_scope *s,uint64_t grant,uint64_t completed,uint32_t selector){
        auto &f=*(Fixture *)p;scope(s);assert(f.granted && grant==61 && !nativeRsc.core.inflight &&
            nativeRsc.core.completed==completed && nativeRsc.core.selector==selector);++f.settles;return f.settledResult;}
    static int drop(void *p,const ggrn_scope *s,uint64_t grant){auto &f=*(Fixture *)p;scope(s);++f.drops;
        assert(f.granted && grant==61 && !nativeRsc.map && !nativeRsc.memory && nativeRsc.leased &&
            gpr_global_owns(&nativeRsc.page,&nativeRsc,17));
        if(f.dropRace)assert(giulia_gpu_rsc_native_invalidate(&f.source)==GGRN_BUSY);
        if(!f.dropResult)f.granted=false;
        return f.dropResult;}
    ggrn_authority authority(){return {GGRN_ABI,sizeof(ggrn_authority),this,take,live,approve,settled,drop};}
    int open(){auto a=authority();callerOps=&a;int code=giulia_gpu_rsc_native_open(&source,&config,&a);callerOps=nullptr;
        a.live=nullptr;a.approve=nullptr;return code; /* Must have copied table. */}
    int submit(uint64_t *s){return giulia_gpu_rsc_native_submit(&source,3,s);}
    int poll(uint64_t s){return giulia_gpu_rsc_native_poll(&source,s);}
    int close(){return giulia_gpu_rsc_native_close(&source);}
    ggrn_status statusSnapshot(){unsigned a=mapAccesses,b=lives,c=takes,d=approvals,e=settles;ggrn_status out={};
        assert(giulia_gpu_rsc_native_status(&out)==GGRN_OK && a==mapAccesses && b==lives && c==takes &&
            d==approvals && e==settles);return out;}
    void pinned(){auto s=statusSnapshot();assert(s.bound && s.quarantined && !s.ready_cached && source.refs==2);
        unsigned accesses=mapAccesses,callbacks=lives+approvals+settles+drops;uint64_t serial=99;
        assert(submit(&serial)==GGRN_QUARANTINED && !serial && close()==GGRN_QUARANTINED);
        assert(accesses==mapAccesses && callbacks==lives+approvals+settles+drops);}
};
static Fixture *active;
static void accessFixture(){auto &f=*active;assert(f.granted && f.valid && nativeRsc.leased &&
    gpr_global_owns(&nativeRsc.page,&nativeRsc,17));
    if(f.block){std::unique_lock<std::mutex> lock(f.mutex);f.entered=true;f.condition.notify_all();
        assert(f.condition.wait_for(lock,std::chrono::seconds(5),[&]{return f.proceed;}));}
    // Emulate W1C and asynchronous command completion in RAM. This does not
    // replace the production volatile load/store or the real core algorithm.
    if(f.reg(8)){f.reg(4)&=~f.reg(8);f.reg(8)=0;}
    if(f.reg(f.control())&(1u<<24)) {
        if(!f.started){++f.starts;f.started=true;f.finished=false;f.reg(f.status())=0;f.reg(f.cmdstatus())=0;}
        if(f.complete && !f.finished){f.reg(4)|=1u<<f.config.tcs_id;f.reg(f.status())=1;
            f.reg(f.cmdstatus())=(1u<<8)|(1u<<16);f.finished=true;}
    }else f.started=false;
}
static void unmapFixture(){if(manualReset)return;assert(nativeRsc.leased && nativeRsc.lifecycle==3 &&
    gpr_global_owns(&nativeRsc.page,&nativeRsc,17));
    if(active->unmapRace)assert(giulia_gpu_rsc_native_invalidate(&active->source)==GGRN_BUSY);}
static void setup(Fixture &f,unsigned version=30,unsigned tcs=0){active=&f;f.config.version=version;f.config.tcs_id=tcs;
    ram[0]=(version==30?3u<<16:(2u<<16)|(7u<<8));ram[3]=(16u<<27)|(8u<<12);
    f.reg(0)=(1u<<tcs)|(1u<<10);f.reg(4)=1u<<9;f.reg(f.status())=1;
    assert(giulia_gpu_rsc_native_initialize()==GGRN_OK);}
static void reset(Fixture &f){
    // Test-only destruction of quarantined RAM. No target reset/reopen API.
    manualReset=true;
    if(nativeRsc.map)nativeRsc.map->release();
    if(nativeRsc.memory)nativeRsc.memory->release();
    if(nativeRsc.source)nativeRsc.source->release();
    IOLockFree(nativeRsc.lock);nativeRsc={};broker={};active=nullptr;manualReset=false;
    assert(f.source.refs==1 && f.foreign.refs==1 && descCreated==descFreed && mapCreated==mapFreed);
    std::memset(ram,0,sizeof(ram));mapFault=brokerFault=mapAccesses=0;hostTime=0;
}
static unsigned cases;
int main(){
    ggrn_status status={};IOService none;uint64_t serial=0;
    assert(!std::strcmp(giulia_gpu_rsc_native_identity(),GGRN_IDENTITY));
    assert(giulia_gpu_rsc_native_status(&status)==GGRN_UNAVAILABLE && !allocatedLocks);
    assert(giulia_gpu_rsc_native_submit(&none,3,&serial)==GGRN_UNAVAILABLE && !allocatedLocks);
    lockFail=true;assert(giulia_gpu_rsc_native_initialize()==GGRN_UNAVAILABLE);lockFail=false;++cases;
    for(unsigned fault=0;fault<23;fault++){
        Fixture f;setup(f);auto a=f.authority();auto c=f.config;
        switch(fault){case 0:a.abi++;break;case 1:a.bytes--;break;case 2:a.cookie=nullptr;break;
          case 3:a.take=nullptr;break;case 4:a.live=nullptr;break;case 5:a.approve=nullptr;break;
          case 6:a.settled=nullptr;break;case 7:a.drop=nullptr;break;case 8:c.version=31;break;
          case 9:c.mapped_bytes=0x10000;break;case 10:c.tcs_id=3;break;case 11:c.epoch=0;break;
          case 12:c.arc_address=0x40010;break;case 13:c.arc_address|=1;break;case 14:c.selector_mask=0;break;
          case 15:c.selector_mask|=1u<<16;break;case 16:c.timeout_us=0;break;
          case 17:c.timeout_us=1000001;break;case 18:c.reserved=1;break;case 19:c.abi++;break;default:break;}
        assert(giulia_gpu_rsc_native_open(fault==20?nullptr:&f.source,fault==21?nullptr:&c,fault==22?nullptr:&a)==GGRN_ARGUMENT);
        assert(!nativeRsc.used && !f.takes && !mapAccesses && f.source.refs==1);reset(f);++cases;
    }
    for(unsigned version: {27u,30u})for(unsigned tcs=0;tcs<3;tcs++){
        Fixture f;setup(f,version,tcs);unsigned locks=allocatedLocks;
        assert(giulia_gpu_rsc_native_initialize()==GGRN_OK && locks==allocatedLocks);
        assert(f.open()==GGRN_OK && f.source.refs==2 && f.statusSnapshot().ready_cached);
        assert(f.open()==GGRN_USED);
        unsigned beforeAccess=mapAccesses;uint32_t ignored=0;
        assert(nativeRscRead(nullptr,0xffffffffu,&ignored)==-1);
        assert(nativeRscRead(nullptr,nativeRsc.scope.transport.mapped_bytes,&ignored)==-1);
        assert(nativeRscWrite(nullptr,0,0)==-1 && nativeRscWrite(nullptr,4,1)==-1);
        assert(nativeRscWrite(nullptr,8,1u<<9)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.control,0xffffffffu)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.wait,2)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.enable,2)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.msg,0)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.addr,0x30020)==-1);
        assert(nativeRscWrite(nullptr,nativeRsc.core.data,31)==-1 && beforeAccess==mapAccesses);
        assert(giulia_gpu_rsc_native_submit(&f.foreign,3,&serial)==GGRN_ARGUMENT);
        unsigned approves=f.approvals;assert(giulia_gpu_rsc_native_submit(&f.source,0,&serial)==GGRN_ARGUMENT && !serial);
        assert(approves==f.approvals && giulia_gpu_rsc_native_submit(&f.source,16,&serial)==GGRN_ARGUMENT);
        assert(giulia_gpu_rsc_native_submit(&f.source,3,nullptr)==GGRN_ARGUMENT);
        assert(giulia_gpu_rsc_native_poll(&f.source,0)==GGRN_ARGUMENT);
        assert(giulia_gpu_rsc_native_invalidate(&f.foreign)==GGRN_ARGUMENT);
        f.approveResult=0;unsigned accesses=mapAccesses;assert(f.submit(&serial)==GGRN_BUSY && !serial && accesses==mapAccesses);
        f.approveResult=1;f.complete=false;assert(f.submit(&serial)==GGRN_OK && serial==1);
        uint64_t other=0;assert(f.submit(&other)==GGRN_BUSY && !other && f.close()==GGRN_BUSY);
        assert(f.poll(serial+1)==GGRN_ARGUMENT && f.poll(serial)==GGRN_PENDING);
        f.complete=true;assert(f.poll(serial)==GGRN_OK && f.starts==1 && f.poll(serial)==GGRN_ARGUMENT);
        assert(f.reg(0)==((1u<<tcs)|(1u<<10)) && f.reg(4)==(1u<<9));
        assert(f.statusSnapshot().completed==serial);
        assert(f.submit(&serial)==GGRN_OK && serial==2 && f.poll(serial)==GGRN_OK && f.starts==2);
        f.settledResult=0;assert(f.close()==GGRN_BUSY && f.statusSnapshot().mapped && !f.drops);
        f.settledResult=1;f.unmapRace=f.dropRace=true;assert(f.close()==GGRN_OK);
        assert(f.drops==1 && !f.granted && f.source.refs==1 && !f.statusSnapshot().bound);
        assert(giulia_gpu_rsc_native_invalidate(&f.source)==GGRN_ARGUMENT);reset(f);++cases;
    }
    for(unsigned fault=1;fault<=3;fault++){
        Fixture f;setup(f);brokerFault=fault;assert(f.open()==GGRN_BUSY && !f.takes && !mapAccesses);
        assert(!f.statusSnapshot().bound);reset(f);++cases;
    }
    {
        Fixture f;setup(f);f.alterCaller=true;
        assert(f.open()==GGRN_OK && nativeRsc.scope.transport.epoch==17 &&
            nativeRsc.scope.transport.mapped_bytes==0xf300 && f.config.epoch==99);
        assert(f.submit(&serial)==GGRN_OK && f.poll(serial)==GGRN_OK && f.close()==GGRN_OK);
        reset(f);++cases;
    }
    {
        Fixture f;setup(f);gpr_token token={};assert(gpr_global_open_epoch(&f.foreign,7)==GPR_OK);
        assert(gpr_global_claim(&nativeRscPage,&f.foreign,7,0,0,&token)==GPR_OK);
        assert(f.open()==GGRN_BUSY && !f.takes && !mapAccesses && !f.statusSnapshot().bound);
        assert(gpr_global_owns(&token,&f.foreign,7));reset(f);++cases;
    }
    for(unsigned fault=1;fault<=5;fault++){
        Fixture f;setup(f);mapFault=fault;assert(f.open()==GGRN_UNAVAILABLE && f.drops==1);
        assert(!f.statusSnapshot().bound && !f.granted);reset(f);++cases;
    }
    for(unsigned fault=0;fault<8;fault++){
        Fixture f;setup(f);
        switch(fault){case 0:ram[0]=0x30100;break;case 1:ram[0]=0xffffffff;break;case 2:ram[1]=1u<<24;break;
          case 3:ram[1]=0xffffffff;break;case 4:ram[3]=(7u<<12)|(16u<<27);break;
          case 5:ram[3]=8u<<12;break;case 6:ram[3]=(8u<<12)|(17u<<27);break;case 7:ram[3]=0xffffffff;break;}
        assert(f.open()==GGRN_PROTOCOL && f.drops==1 && !f.statusSnapshot().bound);reset(f);++cases;
    }
    for(unsigned fault=0;fault<6;fault++){
        Fixture f;setup(f);switch(fault){case 0:f.takeResult=0;f.offeredGrant=0;break;
          case 1:f.takeResult=0;break;case 2:f.offeredGrant=0;break;
          case 3:f.takeResult=-1;break;case 4:f.takeResult=2;break;case 5:f.takeResult=-1;f.offeredGrant=0;break;}
        assert(f.open()==(fault?GGRN_QUARANTINED:GGRN_BUSY));
        assert(!mapAccesses && !f.drops);if(fault)f.pinned();else assert(!f.statusSnapshot().bound);
        reset(f);++cases;
    }
    for(unsigned fault=0;fault<5;fault++){
        Fixture f;setup(f);switch(fault){case 0:f.reg(0)&=~1u;break;case 1:f.reg(4)|=1;break;
          case 2:f.reg(f.control())=1u<<16;break;case 3:f.reg(f.status())=0;break;
          case 4:f.reg(0)=0xffffffff;break;}
        assert(f.open()==GGRN_QUARANTINED && !f.drops);f.pinned();reset(f);++cases;
    }
    // Sweep every CPU authority boundary of open/submit/poll/close.
    for(unsigned phase=0;phase<4;phase++){
        Fixture base;setup(base);if(phase)assert(base.open()==GGRN_OK);
        if(phase==2)assert(base.submit(&serial)==GGRN_OK);
        unsigned before=base.lives;
        assert((phase==0?base.open():phase==1?base.submit(&serial):phase==2?base.poll(serial):base.close())==GGRN_OK);
        unsigned boundaries=base.lives-before;reset(base);
        for(unsigned boundary=1;boundary<=boundaries;boundary++){
            Fixture f;setup(f);if(phase)assert(f.open()==GGRN_OK);
            if(phase==2)assert(f.submit(&serial)==GGRN_OK);
            f.loseAt=f.lives+boundary;
            assert((phase==0?f.open():phase==1?f.submit(&serial):phase==2?f.poll(serial):f.close())==GGRN_QUARANTINED);
            f.pinned();reset(f);++cases;
        }
    }
    for(unsigned fault=0;fault<8;fault++){
        Fixture f;setup(f);assert(f.open()==GGRN_OK);
        if(fault==0)f.approveResult=-1;
        if(fault==1)f.settledResult=-1;
        if(fault==2)f.dropResult=-1;
        if(fault==3)brokerFault=4;
        if(fault==4)brokerFault=5;
        if(fault==5)brokerFault=6;
        int code;
        if(fault==0)code=f.submit(&serial);
        else if(fault==6 || fault==7){f.complete=false;assert(f.submit(&serial)==GGRN_OK);
            if(fault==6)hostTime=100000;else {f.config.timeout_us=1;hostTime=100000;}
            code=f.poll(serial);assert(f.statusSnapshot().inflight && f.statusSnapshot().serial==serial);
        }else code=f.close();
        assert(code==GGRN_QUARANTINED);f.pinned();reset(f);++cases;
    }
    {
        Fixture f;setup(f);assert(f.open()==GGRN_OK);f.complete=false;assert(f.submit(&serial)==GGRN_OK);
        assert(giulia_gpu_rsc_native_invalidate(&f.source)==GGRN_QUARANTINED);
        assert(f.statusSnapshot().revoked && f.statusSnapshot().inflight);f.pinned();reset(f);++cases;
    }
    {
        Fixture f;setup(f);assert(f.open()==GGRN_OK);f.block=true;
        int submitCode=-1,invalidateCode=-1;std::atomic<bool> done{false};
        std::thread worker([&]{submitCode=f.submit(&serial);});
        {std::unique_lock<std::mutex> lock(f.mutex);
            assert(f.condition.wait_for(lock,std::chrono::seconds(5),[&]{return f.entered;}));}
        std::thread invalidator([&]{invalidateCode=giulia_gpu_rsc_native_invalidate(&f.source);done=true;});
        auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(__atomic_load_n(&nativeRsc.lifecycle,__ATOMIC_ACQUIRE)!=2){
            assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
        assert(!done.load());
        {std::lock_guard<std::mutex> lock(f.mutex);f.proceed=true;f.condition.notify_all();}
        worker.join();invalidator.join();assert(submitCode==GGRN_QUARANTINED && invalidateCode==GGRN_QUARANTINED);
        assert(!f.drops && f.statusSnapshot().mapped);f.pinned();reset(f);++cases;
    }
    std::printf("GPU native RSC: %u RAM-only cases passed; no hardware/provider registration\n",cases);
}
