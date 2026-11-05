"""Exercise actual GiuliaGPU Gunyah methods and worker/stop glue on the host.

Only the separately tested ggown C boundary is stubbed. No HVC, GIC register,
physical mapping or live sharing proof is installed. Published test heap state
is reclaimed explicitly after each isolated fixture, never by production stop.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'ios16/gpu/GiuliaGPU.cpp').read_text()
device = (ROOT / 'ios16/gpu/giulia_gpu_dev_impl.h').read_text()


def method(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


# Keep dispatch on the actual OP, and reject accidentally moving stop behind
# the lock/join or periodic servicing outside the real worker loop.
assert device.count('case 22:code=gunyahCommand(a,b,c,d);break;') == 1
worker = method('void GiuliaGPU::worker(')
stop = method('void GiuliaGPU::stop(')
assert worker.index('gunyahWorker=current_thread()') < worker.index('while(!self->stopping)')
assert worker.count('self->gunyahPeriodic();') == 1
assert stop.index('stopGunyah();') < stop.index('IOLockLock(runtimeLock)') < stop.index('while(workerRunning)')
assert method('void GiuliaGPU::free(').index('stopGunyah();') < method('void GiuliaGPU::free(').index('releaseResources();')

fixture = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "giulia_gpu_gunyah_owner.h"
#include "giulia_gpu_terminal.h"
#include "giulia_gpu_hfi_alloc_pool.h"
#include "giulia_gpu_hfi_pump.h"
#include "giulia_gpu_stack.h"
#include "giulia_gpu_share_runtime.h"

using thread_t=void *;
using wait_result_t=int;
static thread_local unsigned char threadIdentity;
static thread_t current_thread(){return &threadIdentity;}
static void thread_terminate(thread_t){}
static std::atomic<unsigned> allocs{0},frees{0},inits{0},probes{0},polls{0},bootstraps{0},
    statuses{0},invalidates{0},delays{0},lastTimeout{0},lastBudget{0},devResults{99};
static bool allocFailure=false;
static int initResult=GGOWN_OK,probeResult=GGOWN_OK,pollResult=GGOWN_IDLE,
    bootstrapResult=GGOWN_OK,statusResult=GGOWN_OK;
static std::atomic<bool> blockProbe{false},probeEntered{false},allowProbeReturn{false};
static std::atomic<uint64_t> ticks{123456789};
static unsigned *delayLatch=nullptr;
static std::mutex logsLock;
static std::vector<std::string> logs;
static std::vector<void *> testHeap;
static void IOLog(const char *format,...) {
    char line[2048];va_list args;va_start(args,format);
    int n=vsnprintf(line,sizeof(line),format,args);va_end(args);
    assert(n>0 && (size_t)n<sizeof(line));
    std::lock_guard<std::mutex> lock(logsLock);logs.emplace_back(line);
}
static uint64_t mach_absolute_time(){return ticks.load();}
static void absolutetime_to_nanoseconds(uint64_t at,uint64_t *out){*out=at;}
static void IODelay(unsigned us) {
    ++delays;ticks.fetch_add((uint64_t)us*1000);
    if(delayLatch)__atomic_store_n(delayLatch,1u,__ATOMIC_RELEASE);
}
struct IOLock {std::mutex lock;std::condition_variable wake;};
static void IOLockLock(IOLock *p){p->lock.lock();}
static void IOLockUnlock(IOLock *p){p->lock.unlock();}
static void IOLockWakeup(IOLock *p,void *,bool){p->wake.notify_all();}
static void IOLockSleep(IOLock *p,void *,int) {
    std::unique_lock<std::mutex> lock(p->lock,std::adopt_lock);
    p->wake.wait(lock);lock.release();
}
static void IOSleep(unsigned){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
#define THREAD_UNINT 0
struct IOService {void stop(IOService *){}};
static unsigned devState;
static uint32_t devArgs[6];
static struct gg_terminal_mailbox gpuTerminal;

class GiuliaGPU:public IOService {
public:
    struct ggr_report bootGunyah={};
    bool bootGunyahCaptured=false;
    struct ggown_state *gunyahOwner=nullptr;
    thread_t gunyahWorker=nullptr;
    unsigned gunyahStopLatch=0;
    bool gunyahPinned=false,gunyahWorkerArmed=false;
    bool manualMemoryPinned=true,hostPrepared=true,platformPoisoned=false;
    IOLock lock;IOLock *runtimeLock=&lock;
    bool workerRunning=false,stopping=false,terminalOwner=false;
    unsigned mode=1,firmwarePowerState=0,observations=0;
    bool hostOnly=false;
    struct {unsigned stage=0,valid_fields=0;} powerObservation;
    std::atomic<unsigned> references{1},releaseCalls{0},resourceAttempts{0},resourcesFreed{0};
    void retain(){assert(!gunyahOwner && !gunyahPinned);++references;}
    void release(){assert(references.fetch_sub(1)>1);++releaseCalls;}
    void releaseResources(){++resourceAttempts;if(!manualMemoryPinned)++resourcesFreed;}
    void observePower(){++observations;}
    void reportStatus(){}
    void hfiCommand(unsigned){}
    void devCommand(const uint32_t *args) {
        assert(args[1]==22);
        devResults=(unsigned)gunyahCommand(args[2],args[3],args[4],args[5]);
    }
    static int gunyahPolicy(void *,uint64_t);
    static uint64_t gunyahNow(void *);
    static int gunyahDelay(void *,unsigned);
    static int gunyahNotification(void *,uint32_t,const void *,size_t);
    int gmuCommand(unsigned){return 1;} /* production adapter needs IOKit + SID5 */
    /* HFI allocation glue compiled in the worker/stop methods: this fixture has
     * no pool or mappings, so the pump stays unavailable. */
    struct gga_pool hfiAllocPool{};
    struct gga_ops hfiAllocOps{};
    struct ggpp_state hfiPump{};
    uint32_t hfiPumpScratch[64]={};
    bool hfiAllocReady=false;
    int hfiPumpOnce(unsigned){return GGPP_ARGUMENT;}
    void hfiAllocReport(){}
    /* Stage machine compiled inside worker/stop: this fixture registers no
     * providers, so the plan stays unavailable and retreat is a no-op. */
    struct ggsk stackState{};
    struct ggsk_ops stackOps[GGSK_STAGES]{};
    bool stackReady=false;
    void stackInit(){}
    int stackPlan(){return GGSK_ARGUMENT;}
    void stackReport(){}
    /* Share command compiled inside the worker: no owner/channel here. */
    int shareCommand(){return GGSR_ARGUMENT;}
    void shareReport(){}
    int gunyahCommand(uint32_t,uint32_t,uint32_t,uint32_t);
    void gunyahPeriodic();void stopGunyah();
    void closeTerminal(unsigned);void stop(IOService *);
    static void worker(void *,wait_result_t);
};
#define GGPH_PREPARED 2
static void *IOMalloc(size_t bytes) {
    assert(bytes==sizeof(struct ggown_state));++allocs;
    if(allocFailure)return nullptr;
    void *p=malloc(bytes);assert(p);memset(p,0xa5,bytes);testHeap.push_back(p);return p;
}
static void IOFree(void *p,size_t bytes) {
    assert(bytes==sizeof(struct ggown_state));auto *owner=(struct ggown_state *)p;
    auto *s=(GiuliaGPU *)owner->policy.cookie;
    assert(s && !s->gunyahOwner && !s->gunyahPinned && s->references==(s->workerRunning?2u:1u));
    bool found=false;for(auto &slot:testHeap)if(slot==p){slot=nullptr;found=true;break;}
    assert(found);free(p);++frees;
}
static GiuliaGPU *published(struct ggown_state *owner) {
    auto *s=(GiuliaGPU *)owner->policy.cookie;
    assert(s && s->gunyahOwner==owner && s->gunyahPinned && s->references>=2);
    return s;
}
extern "C" int ggown_init(struct ggown_state *owner,const struct ggr_report *report,const struct ggown_policy *policy) {
    ++inits;assert(ggr_report_valid(report));
    for(unsigned i=0;i<sizeof(*owner);i++)assert(!((unsigned char *)owner)[i]);
    auto *s=(GiuliaGPU *)policy->cookie;
    assert(s && !s->gunyahOwner && !s->gunyahPinned && s->references==(s->workerRunning?2u:1u));
    assert(s->gunyahWorker==current_thread() && s->gunyahWorkerArmed && policy->live(s,report->epoch)==1);
    owner->policy=*policy;owner->epoch=report->epoch;owner->initialized=1;owner->stage=GGOWN_CONFIGURED;
    return initResult;
}
extern "C" int ggown_probe(struct ggown_state *owner,unsigned timeout) {
    ++probes;lastTimeout=timeout;GiuliaGPU *s=published(owner);
    if(!owner->policy.live(s,owner->epoch))return GGOWN_LOST;
    if(blockProbe) {
        probeEntered=true;
        while(!allowProbeReturn.load())std::this_thread::yield();
        if(!owner->policy.live(s,owner->epoch))return GGOWN_LOST;
    }
    if(probeResult==GGOWN_OK){owner->stage=GGOWN_READY;owner->bootstrapped=1;}
    return probeResult;
}
extern "C" int ggown_bootstrap(struct ggown_state *owner,unsigned timeout) {
    ++bootstraps;lastTimeout=timeout;GiuliaGPU *s=published(owner);
    if(!owner->policy.live(s,owner->epoch))return GGOWN_LOST;
    if(bootstrapResult==GGOWN_OK){owner->stage=GGOWN_CHANNEL;owner->bootstrapped=1;}
    return bootstrapResult;
}
extern "C" int ggown_poll(struct ggown_state *owner,unsigned budget,unsigned timeout) {
    ++polls;lastBudget=budget;lastTimeout=timeout;GiuliaGPU *s=published(owner);
    return owner->policy.live(s,owner->epoch)?pollResult:GGOWN_LOST;
}
extern "C" int ggown_get_status(struct ggown_state *owner,struct ggown_status *out) {
    ++statuses;(void)published(owner);
    if(statusResult!=GGOWN_OK)return statusResult;
    *out={};out->initialized=owner->initialized;out->epoch=owner->epoch;
    out->stage=owner->stage;out->invalid=__atomic_load_n(&owner->loss,__ATOMIC_ACQUIRE);
    out->bootstrapped=owner->bootstrapped;return GGOWN_OK;
}
extern "C" int ggown_invalidate(struct ggown_state *owner) {
    ++invalidates;(void)published(owner);
    __atomic_store_n(&owner->loss,1u,__ATOMIC_RELEASE);return GGOWN_LOST;
}
'''
fixture += '\n#include "giulia_gpu_gunyah_impl.h"\n'
for signature in ('void GiuliaGPU::worker(', 'void GiuliaGPU::closeTerminal(', 'void GiuliaGPU::stop('):
    fixture += '\n' + method(signature) + '\n'
fixture += r'''
static void reset() {
    // Test teardown only: actual methods must leave all published owners pinned.
    for(void *p:testHeap)free(p);
    testHeap.clear();
    allocs=0;frees=0;inits=0;probes=0;polls=0;bootstraps=0;statuses=0;invalidates=0;delays=0;
    lastTimeout=0;lastBudget=0;devResults=99;allocFailure=false;
    initResult=GGOWN_OK;probeResult=GGOWN_OK;pollResult=GGOWN_IDLE;
    bootstrapResult=GGOWN_OK;statusResult=GGOWN_OK;delayLatch=nullptr;
    blockProbe=false;probeEntered=false;allowProbeReturn=false;ticks=123456789;
    __atomic_store_n(&devState,0u,__ATOMIC_RELEASE);memset(devArgs,0,sizeof(devArgs));
}
static void setup(GiuliaGPU &s,bool worker=true) {
    ggr_report_init(&s.bootGunyah);s.bootGunyah.source=GGR_SOURCE_BOOT_FDT;
    s.bootGunyah.fdt_bytes=4096;s.bootGunyah.epoch=37;s.bootGunyah.tx_cap=0;s.bootGunyah.rx_cap=1;
    s.bootGunyah.rm_node=64;s.bootGunyah.gic_node=128;s.bootGunyah.interrupt_parent=1;s.bootGunyah.interrupt_cells=3;
    s.bootGunyah.tx_irq[1]=928;s.bootGunyah.tx_irq[2]=1;s.bootGunyah.rx_irq[1]=929;s.bootGunyah.rx_irq[2]=1;
    ggr_report_seal(&s.bootGunyah);assert(ggr_report_valid(&s.bootGunyah));s.bootGunyahCaptured=true;
    if(worker)s.gunyahWorker=current_thread();
}
static unsigned nativeCalls(){return probes+polls+bootstraps;}
static void policy_and_callbacks() {
    reset();GiuliaGPU s;setup(s,false);
    assert(!GiuliaGPU::gunyahPolicy(&s,37));
    assert(s.gunyahCommand(0,0,0,0)==0 && !allocs && !inits && !nativeCalls());
    assert(s.gunyahCommand(1,10,0,0)==8 && !allocs); // A boot report cannot arm a caller.
    s.gunyahWorker=current_thread();assert(!GiuliaGPU::gunyahPolicy(&s,37));
    s.gunyahWorkerArmed=true;assert(GiuliaGPU::gunyahPolicy(&s,37));
    assert(!GiuliaGPU::gunyahPolicy(&s,38));
    s.manualMemoryPinned=false;assert(!GiuliaGPU::gunyahPolicy(&s,37));s.manualMemoryPinned=true;
    s.hostPrepared=false;assert(!GiuliaGPU::gunyahPolicy(&s,37));s.hostPrepared=true;
    s.platformPoisoned=true;assert(!GiuliaGPU::gunyahPolicy(&s,37));s.platformPoisoned=false;
    s.bootGunyahCaptured=false;assert(!GiuliaGPU::gunyahPolicy(&s,37));s.bootGunyahCaptured=true;
    std::thread wrong([&]{assert(!GiuliaGPU::gunyahPolicy(&s,37));assert(s.gunyahCommand(1,10,0,0)==8);});wrong.join();
    assert(!nativeCalls() && !allocs && !inits);
    assert(GiuliaGPU::gunyahNow(&s)==123456);
    assert(GiuliaGPU::gunyahDelay(&s,17)==0 && delays==1);
    assert(GiuliaGPU::gunyahDelay(&s,1000001)==-1 && delays==1);
    delayLatch=&s.gunyahStopLatch;assert(GiuliaGPU::gunyahDelay(&s,1)==-1 && delays==2);
    __atomic_store_n(&s.gunyahStopLatch,0u,__ATOMIC_RELEASE);delayLatch=nullptr;
    uint8_t event[]={0x73,0,0,0,2,0,0,0};
    assert(GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8)==1);
    assert(!GiuliaGPU::gunyahNotification(&s,1,event,8));
    assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,7));
    assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,nullptr,8));
    event[2]=1;assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8));event[2]=0;
    event[4]=7;assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8));event[4]=2;
    event[0]=0;assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8));event[0]=0x73;
    event[5]=5;assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8));event[5]=0;
    s.gunyahWorkerArmed=false;assert(!GiuliaGPU::gunyahNotification(&s,GGRQ_VM_STATUS,event,8));
    assert(!nativeCalls() && !allocs);
}
static void commands() {
    reset();GiuliaGPU s;setup(s);
    const uint32_t bad[][4]={{5,0,0,0},{0,1,0,0},{0,0,1,0},{1,0,0,0},{1,3001,0,0},
        {1,1,1,0},{2,0,1,0},{2,GGRS_MAX_POLL_FRAMES+1,1,0},{2,1,0,0},{2,1,3001,0},
        {3,0,0,0},{3,3001,0,0},{3,1,1,0},{4,1,0,0},{4,0,1,0}};
    for(const auto &args:bad)assert(s.gunyahCommand(args[0],args[1],args[2],args[3])==4);
    for(unsigned action=0;action<=4;action++)assert(s.gunyahCommand(action,1,1,1)==4);
    assert(!allocs && !inits && !nativeCalls());
    assert(s.gunyahCommand(2,1,1,0)==7 && s.gunyahCommand(4,0,0,0)==7);
    assert(s.gunyahCommand(1,3000,0,0)==0 && lastTimeout==3000000 && probes==1);
    assert(allocs==1 && !frees && inits==1 && s.references==2 && s.gunyahPinned && s.gunyahOwner);
    unsigned native=nativeCalls();assert(s.gunyahCommand(0,0,0,0)==0 && statuses==1 && nativeCalls()==native);
    statusResult=GGOWN_BUSY;assert(s.gunyahCommand(0,0,0,0)==5 && nativeCalls()==native);statusResult=GGOWN_OK;
    assert(s.gunyahCommand(2,GGRS_MAX_POLL_FRAMES,3000,0)==0 && lastBudget==GGRS_MAX_POLL_FRAMES && lastTimeout==3000000);
    assert(s.gunyahCommand(3,1,0,0)==0 && bootstraps==1 && lastTimeout==1000);
    assert(s.gunyahCommand(4,0,0,0)==0 && invalidates==1 && !frees && s.references==2);
    s.stopGunyah();assert(s.gunyahStopLatch && !frees && s.references==2);
    assert(s.gunyahCommand(0,0,0,0)==0 && !frees); // Status remains historical after stop.
}
static void failures() {
    for(int result=GGOWN_OK;result<=GGOWN_BUDGET;result++) {
        reset();GiuliaGPU s;setup(s);probeResult=result;
        const int expected=result==GGOWN_OK || result==GGOWN_IDLE || result==GGOWN_BUDGET?0:
            result==GGOWN_ARGUMENT || result==GGOWN_USED?4:result==GGOWN_BUSY?5:8;
        assert(s.gunyahCommand(1,17,0,0)==expected && s.platformPoisoned==(expected==8));
        assert(s.gunyahPinned && s.references==2 && !frees && s.gunyahOwner);
        s.stopGunyah();assert(!frees && s.references==2);
    }
    for(int result:{GGOWN_ARGUMENT,GGOWN_BUSY,GGOWN_LOST}) {
        reset();GiuliaGPU s;setup(s);initResult=result;
        int expected=result==GGOWN_ARGUMENT?4:result==GGOWN_BUSY?5:8;
        assert(s.gunyahCommand(1,10,0,0)==expected);
        assert(allocs==1 && frees==1 && inits==1 && !nativeCalls());
        assert(!s.gunyahOwner && !s.gunyahPinned && !s.gunyahWorkerArmed && s.gunyahWorker==current_thread());
        assert(s.references==1 && s.platformPoisoned==(expected==8));s.stopGunyah();assert(frees==1);
    }
    reset();GiuliaGPU s;setup(s);allocFailure=true;
    assert(s.gunyahCommand(1,10,0,0)==7 && allocs==1 && !frees && !inits && !s.gunyahPinned);
    reset();GiuliaGPU noReport;setup(noReport);noReport.bootGunyahCaptured=false;
    assert(noReport.gunyahCommand(1,10,0,0)==7 && !allocs);
    noReport.bootGunyahCaptured=true;noReport.bootGunyah.crc32^=1;
    assert(noReport.gunyahCommand(1,10,0,0)==7 && !allocs);
}
static void periodic() {
    reset();GiuliaGPU s;setup(s);s.gunyahPeriodic();assert(!polls && !statuses);
    assert(s.gunyahCommand(3,17,0,0)==0);s.gunyahPeriodic();assert(!polls);
    s.gunyahOwner->stage=GGOWN_READY;
    statusResult=GGOWN_BUSY;s.gunyahPeriodic();assert(!polls);statusResult=GGOWN_OK;
    __atomic_store_n(&s.gunyahOwner->loss,1u,__ATOMIC_RELEASE);s.gunyahPeriodic();assert(!polls);
    __atomic_store_n(&s.gunyahOwner->loss,0u,__ATOMIC_RELEASE);
    std::thread wrong([&]{s.gunyahPeriodic();});wrong.join();assert(!polls && !s.platformPoisoned);
    s.hostPrepared=false;s.gunyahPeriodic();assert(!polls);s.hostPrepared=true;
    s.manualMemoryPinned=false;s.gunyahPeriodic();assert(!polls);s.manualMemoryPinned=true;
    s.gunyahWorkerArmed=false;s.gunyahPeriodic();assert(!polls);s.gunyahWorkerArmed=true;
    for(int result:{GGOWN_OK,GGOWN_IDLE,GGOWN_BUDGET}) {
        pollResult=result;s.gunyahPeriodic();assert(!s.platformPoisoned && lastBudget==64 && lastTimeout==5000);
    }
    assert(polls==3);pollResult=GGOWN_SESSION;s.gunyahPeriodic();assert(s.platformPoisoned && polls==4);
    s.gunyahPeriodic();assert(polls==4);s.stopGunyah();s.gunyahPeriodic();assert(polls==4 && !frees);
}
template<class F> static void until(F condition) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()){assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();}
}
static void worker_stop() {
    reset();GiuliaGPU s;setup(s,false);s.references=2;s.workerRunning=true;s.terminalOwner=true;
    assert(gg_terminal_open(&gpuTerminal));blockProbe=true;
    std::thread worker(GiuliaGPU::worker,&s,0);
    until([]{return __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_READY;});
    const uint32_t request[6]={42,22,1,17,0,0};memcpy(devArgs,request,sizeof(request));
    __atomic_store_n(&devState,2u,__ATOMIC_RELEASE);
    until([]{return probeEntered.load();});assert(s.gunyahWorker && s.references==3 && s.gunyahPinned);
    std::atomic<bool> stopped{false};std::thread stopper([&]{s.stop(nullptr);stopped=true;});
    until([&]{return __atomic_load_n(&s.gunyahStopLatch,__ATOMIC_ACQUIRE)==1 && invalidates.load()>0;});
    assert(!stopped && !s.resourceAttempts && !frees && s.references==3);
    allowProbeReturn=true;stopper.join();worker.join();
    assert(stopped && !s.workerRunning && s.resourceAttempts==1 && !s.resourcesFreed);
    assert(s.references==2 && s.releaseCalls==1 && !frees && s.gunyahPinned && s.platformPoisoned);
    assert(devResults==8 && !polls && __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_STOPPED);
}
static void worker_periodic() {
    reset();GiuliaGPU s;setup(s,false);s.references=2;s.workerRunning=true;s.terminalOwner=true;
    assert(gg_terminal_open(&gpuTerminal));
    std::thread worker(GiuliaGPU::worker,&s,0);
    until([]{return __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_READY;});
    const uint32_t bootstrap[6]={43,22,3,17,0,0};memcpy(devArgs,bootstrap,sizeof(bootstrap));
    __atomic_store_n(&devState,2u,__ATOMIC_RELEASE);
    until([]{return devResults.load()==0 && __atomic_load_n(&devState,__ATOMIC_ACQUIRE)==0;});
    assert(bootstraps==1 && !probes && !polls);
    // Transition to READY by an actual command on the owning worker. No
    // out-of-thread mutation of private owner state is needed to trigger it.
    devResults=99;const uint32_t probe[6]={44,22,1,17,0,0};memcpy(devArgs,probe,sizeof(probe));
    __atomic_store_n(&devState,2u,__ATOMIC_RELEASE);
    until([]{return polls.load()>0 && lastBudget.load()==64 && lastTimeout.load()==5000;});
    assert(probes==1 && bootstraps==1 && lastBudget==64 && lastTimeout==5000);
    s.stop(nullptr);worker.join();
    assert(!frees && s.references==2 && s.gunyahPinned && s.releaseCalls==1);
}
int main() {
    policy_and_callbacks();commands();failures();periodic();worker_stop();worker_periodic();
    for(const auto &line:logs) {
        assert(line.size()<256 && line.back()=='\n');
        assert(line.find("completion_verified=1")==std::string::npos);
        assert(line.find("provider_ready=1")==std::string::npos);
    }
    reset();
    puts("Production GPU Gunyah runtime: real command/worker/stop methods, live worker policy, callback dispatch, pin-before-publish, status isolation and failure quarantine passed (ggown fixtures only)");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-gunyah-runtime-') as folder:
    folder = Path(folder)
    unit = folder / 'runtime.cpp'
    unit.write_text(fixture)
    binary = folder / 'runtime'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                    *include_flags(ROOT), str(unit), '-o', str(binary)], check=True)
    environment = dict(os.environ)
    environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    # LeakSanitizer is unavailable on the macOS arm64 runner. Keep ASan/UBSan
    # enabled everywhere and explicit leak detection on supported Linux hosts.
    leak_check = '0' if _module_sys.platform == 'darwin' else '1'
    environment['ASAN_OPTIONS'] = ':'.join(filter(None,
        (environment.get('ASAN_OPTIONS', ''), 'detect_leaks=' + leak_check)))
    subprocess.run([str(binary)], check=True, env=environment)
