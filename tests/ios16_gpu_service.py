"""Run the production terminal worker/stop methods against host lock fixtures.

This verifies CPU lifetime/order only. The observation stub cannot validate
register accessibility, cache behavior, bus errors or device power timing.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'ios16/gpu/GiuliaGPU.cpp').read_text()


def method(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]


fixture=r'''
#include <atomic>
#include <cstring>
#include "giulia_gpu_dev.h"
static unsigned devState;static uint32_t devArgs[6];
static void PE_parse_boot_argn(const char *,int *v,size_t){*v=1;}
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "giulia_gpu_terminal.h"
#include "giulia_gpu_power.h"
#include "giulia_gpu_power_owner.h"
#include "giulia_gpu_gunyah_boot.h"
#include "giulia_platform_power.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_hfi_alloc_pool.h"
#include "giulia_gpu_hfi_pump.h"
#include "giulia_gpu_stack.h"
#include "giulia_gpu_share_runtime.h"
#include "giulia_scm.h"
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_cache.h"
#include <strings.h>
static void gpuHfiBarrier(void *){__sync_synchronize();}
int giulia_platform_qmp_available(void){return 1;}
extern "C" int giulia_gpu_cache_publish(const void *,size_t){return 1;}
static std::mutex logsLock;
static std::vector<std::string> logs;
static std::atomic<unsigned> statuses{0};
static void IOLog(const char *format,...) {
    char line[2048];va_list args;va_start(args,format);
    vsnprintf(line,sizeof(line),format,args);va_end(args);
    std::lock_guard<std::mutex> guard(logsLock);logs.emplace_back(line);
    if(logs.back().find("command=gpu.status stage=")!=std::string::npos)++statuses;
}
struct IOLock { std::mutex lock;std::condition_variable wake; };
static void IOLockLock(IOLock *p){p->lock.lock();}
static void IOLockUnlock(IOLock *p){p->lock.unlock();}
static void IOLockWakeup(IOLock *p,void *,bool){p->wake.notify_all();}
static void IOLockSleep(IOLock *p,void *,int) {
    std::unique_lock<std::mutex> lock(p->lock,std::adopt_lock);
    p->wake.wait(lock);lock.release();
}
static void IOSleep(unsigned){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
#define THREAD_UNINT 0
using wait_result_t=int;
static void *current_thread(){return nullptr;}
static void thread_terminate(void *){}
struct IOService {void stop(IOService *){}};
/* Only the worker-to-controller boundary is doubled here. The separate
 * power_owner fixture executes the actual resident controller and runtime. */
static ggpo_status workerPowerStatus={};
static unsigned powerStarts=0,powerStops=0;
static int powerStartCode=GGPO_UNAVAILABLE;
extern "C" int giulia_gpu_power_owner_acquire(void *,uint64_t epoch) {
    assert(epoch==17);++powerStarts;return powerStartCode;
}
extern "C" int giulia_gpu_power_owner_release(void *) {
    ++powerStops;return workerPowerStatus.quarantined?GGPO_QUARANTINED:GGPO_OK;
}
extern "C" int giulia_gpu_power_owner_status(ggpo_status *out) {
    *out=workerPowerStatus;return GGPO_OK;
}
static struct gg_terminal_mailbox gpuTerminal;
/* Share fixtures: an aligned control page, a live owner snapshot (5/9) and a
 * channel that reports itself as this epoch's exclusive owner. */
static uint8_t sharePage[16384] __attribute__((aligned(16384)));
struct ggown_state { uint64_t epoch; };
static struct ggrm_state shareChannelState;
static uint16_t shareSelf=5,sharePeer=9;
static int share_rm_valid(void *,uint64_t){return 1;}
static int share_rm_send(void *,const void *,size_t,unsigned){return 0;}
static int share_rm_receive(void *,void *,size_t,size_t *){return 1;}
static uint64_t share_rm_now(void *){return 0;}
static int share_rm_delay(void *,unsigned){return 0;}
static int share_rm_notify(void *,uint32_t,const void *,size_t){return 1;}
extern "C" int ggown_get_channel(struct ggown_state *owner,struct ggrm_io *io,
    struct ggrm_state **channel)
{
    if(!owner || !io || !channel) return 1;
    io->cookie=owner;
    io->valid=share_rm_valid;
    io->send=share_rm_send;
    io->receive=share_rm_receive;
    io->now_us=share_rm_now;
    io->delay_us=share_rm_delay;
    io->notification=share_rm_notify;
    shareChannelState.epoch=owner->epoch;
    shareChannelState.next_sequence=7;
    *channel=&shareChannelState;
    return 0;
}
extern "C" int ggown_copy_snapshot(struct ggown_state *owner,struct gg_rq_snapshot *out)
{
    if(!owner || !out) return 1;
    memset(out,0,sizeof(*out));
    out->epoch=owner->epoch;
    out->self_vmid=shareSelf;
    out->peer_vmid=sharePeer;
    return 0;
}
class GiuliaGPU:public IOService {
public:
    IOLock lock;
    IOLock *runtimeLock=&lock;
    bool workerRunning=true,stopping=false,terminalOwner=true;
    unsigned mode=1,reservationCount=5,tablePages=6,leafPages=104,observations=0;
    bool hostOnly=false;
    bool powerOnlyTest=false,powerOnlyAttached=false;unsigned powerOnlyCode=GGPO_UNAVAILABLE;
    void powerOnlyStart();void powerOnlyStop();void powerOnlyReport();
    std::atomic<unsigned> devCalls{0},periodicCalls{0},gmuCalls{0};
    bool firmwareVerified=true,hostPrepared=true;
    bool bootGunyahCaptured=false;
    struct ggr_report bootGunyah={};
    bool manualMemoryPinned=false,platformPoisoned=false,fenceMapped=false;
    void *gunyahWorker=nullptr;
    int auditFenceMappings(){platformPoisoned=true;return 8;}
    unsigned hfiIndex=0,hfiSequence=0,hfiLastHeader=0;
    /* HFI allocation glue: the real implementation header is compiled against
     * this stub, so the pool adapter, the pump and the report are type-checked
     * here. Only the empty/uninitialised cases run: the fixture owns no device
     * mappings. */
    struct gga_pool hfiAllocPool{};
    struct gga_ops hfiAllocOps{};
    struct ggpp_state hfiPump{};
    uint32_t hfiPumpScratch[64]={};
    bool hfiAllocReady=false;
    struct ggd_pool devPool{};
    bool devUcode[GGD_MAX]={};
    bool devMappingsManaged=true;
    bool gmuFaulted=false;
    int mapDevAllocation(unsigned){return 8;}
    uint8_t *devPointer(uint32_t){return nullptr;}
    void hfiAllocInit();
    int hfiPumpOnce(unsigned);
    void hfiAllocReport();
    struct ggsk stackState{};
    struct ggsk_ops stackOps[GGSK_STAGES]{};
    bool stackReady=false;
    void stackInit();
    int stackPlan();
    void stackReport();
    static int stackProbeBoot(void *,uint64_t *);
    static int stackProbeFirmware(void *,uint64_t *);
    static int stackProbeMemory(void *,uint64_t *);
    static int stackEnterNoop(void *);
    static int stackLeaveNoop(void *);
    /* Share transaction glue: the fixture provides a live owner and channel, so
     * the gate runs up to the authority refusal. */
    struct ggsr_state shareRuntime{};
    struct ggsh_owner shareOwner{};
    struct ggsr_gate shareGate{};
    uint8_t *shareControlCPU=nullptr;
    uint64_t shareControlPA=0;
    bool shareControlReady=false;
    struct ggown_state *gunyahOwner=nullptr;
    bool gunyahPinned=false;
    bool shareControlAllocate()
    {
        shareControlCPU=(uint8_t *)sharePage;
        shareControlPA=0x10000000u;
        shareControlReady=true;
        return true;
    }
    int shareCommand();
    void shareReport();
    static int shareLive(void *,uint64_t);
    static int shareIdentity(void *,uint64_t,uint16_t *,uint16_t *);
    static int shareAuthorize(void *,uint64_t,uint16_t,uint16_t,uint64_t,uint32_t);
    static int sharePublish(void *,const void *,size_t);
    static int platformScmCall(void *,struct ggs_regs *){return 1;}
    static int platformScmDelay(void *,unsigned){return 0;}
    static int platformScmPrerequisites(void *,unsigned){return 0;}
    static uint64_t hfiPoolAddress(void *,unsigned,unsigned);
    static int hfiPoolMap(void *,unsigned,unsigned);
    static int hfiPoolUnmap(void *,unsigned,unsigned);
    static int hfiPoolPublish(void *,unsigned);
    static int hfiPoolBarrier(void *);
    struct Memory {uint32_t words[GGH_BYTES/4]={};void *getBytesNoCopy(){return words;}} memory,logMemory;
    Memory *allocations[3]={&memory,&logMemory,&logMemory};
    ggp_report powerObservation={};
    gpp_report firmwarePower={};unsigned firmwarePowerState=GGPH_INVALID;
    std::atomic<unsigned> references{2},inObservation{0},released{0};
    std::atomic<bool> entered{false},allowObservation{false};
    void retain(){++references;}
    void release(){assert(references.fetch_sub(1)>=2);}
    void observePower() {
        assert(!released);++inObservation;entered=true;
        while(!allowObservation.load())std::this_thread::yield();
        powerObservation={};powerObservation.stage=GGP_CFG_OFF;
        powerObservation.valid_fields=7;++observations;--inObservation;
    }
    void releaseResources(){assert(!inObservation);if(!manualMemoryPinned)++released;}
    void devCommand(const uint32_t *){++devCalls;}
    void gunyahPeriodic(){++periodicCalls;}
    void stopGunyah(){}
    void hfiCommand(unsigned);
    int gmuCommand(unsigned){++gmuCalls;return 1;} /* IOKit + SID5 prerequisites absent */
    void reportStatus();void closeTerminal(unsigned);void stop(IOService *);
    static void worker(void *,wait_result_t);
};
#include "giulia_gpu_hfi_alloc_impl.h"
#include "giulia_gpu_stack_impl.h"
#include "giulia_gpu_share_impl.h"
#include "giulia_gpu_power_owner_impl.h"
template<class F> static void until(F condition) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!condition()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::yield();
    }
}
'''
for signature in ('extern "C" int giulia_gpu_terminal(',
                  'void GiuliaGPU::reportStatus(',
                  'void GiuliaGPU::hfiCommand(',
                  'void GiuliaGPU::worker(',
                  'void GiuliaGPU::closeTerminal(',
                  'void GiuliaGPU::stop('):
    fixture+='\n'+method(signature)+'\n'
fixture+=r'''
int main() {
    assert(giulia_gpu_terminal("gpu.start")==0);
    assert(giulia_gpu_terminal("gpu.status")==1);
    GiuliaGPU first;
    assert(gg_terminal_open(&gpuTerminal));
    std::thread worker(GiuliaGPU::worker,&first,0);
    until([]{return __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_READY;});
    assert(giulia_gpu_terminal("gpu.status")==1);
    until([]{return statuses.load()>=1;});
    assert(!first.entered); /* mode 1/status have no observation side effect. */
    assert(giulia_gpu_terminal("gpu.observe")==1);
    until([&]{return first.entered.load();});
    std::atomic<bool> stopped{false};
    std::thread stopper([&]{first.stop(nullptr);stopped=true;});
    for(unsigned i=0;i<100;++i)assert(giulia_gpu_terminal("gpu.status")==1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(!stopped && !first.released && first.references==2);
    first.allowObservation=true;
    stopper.join();worker.join();
    assert(first.released==1 && first.references==1 && !first.workerRunning);
    assert(first.observations==1 && first.powerObservation.stage==GGP_CFG_OFF);
    assert(!gg_terminal_take(&gpuTerminal));
    assert(__atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)==GGT_STOPPED);
    assert(giulia_gpu_terminal("gpu.observe")==1);
    assert(!gg_terminal_take(&gpuTerminal));
    GiuliaGPU second;second.mode=2;second.allowObservation=true;
    assert(gg_terminal_open(&gpuTerminal));
    std::thread next(GiuliaGPU::worker,&second,0);
    until([&]{return second.entered.load();});
    assert(giulia_gpu_terminal("gpu.status")==1);
    second.stop(nullptr);next.join();
    assert(second.observations==1 && second.released==1 && second.references==1);
    GiuliaGPU third;third.mode=3;third.workerRunning=false;
    assert(!ggh_init(third.memory.words,GGH_BYTES,GGH_IOVA));
    third.hfiCommand(GGT_HFI_QUERY);assert(!third.hfiLastHeader && !third.memory.words[17]);
    third.hfiCommand(GGT_HFI_PIN);third.hfiCommand(GGT_HFI_PIN);
    assert(third.references==3 && third.manualMemoryPinned);
    third.hfiCommand(GGT_HFI_QUERY|GGT_HFI_STATUS|GGT_HFI_LOG);
    assert(third.hfiLastHeader==0x10030c && third.memory.words[17]==4);
    uint32_t *rx=third.memory.words+GGH_RING_OFFSET/4+1024;
    rx[0]=0x1040c;rx[1]=third.hfiLastHeader;rx[2]=1;rx[3]=0;
    third.memory.words[6+12+11]=4;
    third.hfiCommand(GGT_HFI_POLL);assert(third.memory.words[6+12+10]==4);
    assert(!third.hfiLastHeader);
    // A matching value with error status, or a legacy reply with no status,
    // proves receipt only. Neither may be reported as an accepted command.
    for(unsigned words=3;words<=4;++words) {
        third.hfiCommand(GGT_HFI_QUERY);
        unsigned offset=third.memory.words[6+12+11];
        rx[offset]=(1u<<16)|(words<<8)|12;
        rx[offset+1]=third.hfiLastHeader;rx[offset+2]=1;rx[offset+3]=8;
        third.memory.words[6+12+11]=offset+4;
        third.hfiCommand(GGT_HFI_POLL);assert(!third.hfiLastHeader);
    }
    std::vector<uint32_t> saved(third.memory.words,third.memory.words+GGH_BYTES/4);
    unsigned sequence=third.hfiSequence;third.fenceMapped=true;
    third.hfiCommand(GGT_HFI_PIN|GGT_HFI_QUERY|GGT_HFI_POLL);
    assert(third.platformPoisoned && third.hfiSequence==sequence && third.references==3 &&
        !memcmp(saved.data(),third.memory.words,GGH_BYTES));
    assert(gg_terminal_open(&gpuTerminal));third.stop(nullptr);third.release();
    assert(!third.released && third.references==2); // Manual DMA ownership retains RAM until reset.
    bool sawStatus=false,sawUnavailable=false,sawInactive=false,sawAck=false;
    unsigned rejectedAcks=0;
    // Exercise the recorded failure and worst-width values under the target's
    // actual IOLog bound, including readiness records after the status line.
    second.firmwarePowerState=GGPH_RECORDED;
    second.firmwarePower.flags=GPP_EXPECTED_VALID|GPP_OBSERVED_VALID|GPP_MASK_VALID;
    second.firmwarePower.expected=second.firmwarePower.observed=second.firmwarePower.mask=~0ULL;
    second.firmwarePower.firmware_status=second.firmwarePower.protocol_status=~0ULL;
    second.firmwarePower.valid_reads=0xffff;
    for(unsigned i=0;i<16;++i)second.firmwarePower.values[i]=~0u;
    second.reportStatus();
    for(const auto &line:logs) {
        assert(line.size()<256 && line.back()=='\n');
        if(line.find("command=gpu.status stage=")!=std::string::npos) {
            sawStatus=true;
        }
        if(line.find("power_acquired=0 smmu_active=0 gmu_started=0")!=std::string::npos) {
            assert(line.find("hardware_ready=0 acceleration=0 device_writes=0")!=std::string::npos);
            sawInactive=true;
        }
        if(line.find("service=unavailable")!=std::string::npos)sawUnavailable=true;
        if(line.find("query_ack=1 header=0x10030c value=0x1 status_present=1 status=0 accepted=1")!=std::string::npos)sawAck=true;
        if(line.find("query_ack=1")!=std::string::npos && line.find("accepted=0")!=std::string::npos)++rejectedAcks;
    }
    assert(sawStatus && sawUnavailable && sawInactive && sawAck && rejectedAcks==2);
    GiuliaGPU fourth;
    fourth.hfiAllocInit();
    /* The adapter owns the bump cursors: init must leave an empty, valid pool. */
    assert(fourth.hfiAllocReady && fourth.devPool.count==0 && fourth.devPool.next==GGD_CACHE_BYTES);
    assert(fourth.hfiPumpOnce(2)==GGPP_ARGUMENT); /* nothing is pinned yet */
    fourth.manualMemoryPinned=true;
    /* The fixture queue table is all zeroes: the pump must quarantine on the
     * corrupt table instead of reading it as an empty queue. */
    assert(fourth.hfiPumpOnce(2)==GGPP_RECEIVE && fourth.hfiPump.quarantined==1);
    fourth.hfiAllocReport();
    /* Stage machine: only the three observable stages are registered, so the
     * plan must stop at power-lease without entering any device stage. */
    GiuliaGPU fifth;
    fifth.mode=3;
    fifth.bootGunyahCaptured=true;
    fifth.firmwareVerified=true;
    fifth.hostPrepared=true;
    fifth.firmwarePowerState=GGPH_PREPARED;
    fifth.stackInit();
    assert(fifth.stackReady);
    assert(fifth.stackPlan()==GGSK_NO_PROVIDER);
    assert(fifth.stackState.blocked==GGSK_POWER);
    assert(fifth.stackState.writes==0 && fifth.stackState.quarantined==0);
    assert(fifth.stackState.entered_mask==
        (GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_FIRMWARE)|GGSK_BIT(GGSK_HOST_MEMORY)));
    fifth.stackReport();
    /* Mode 1 has no host-mutation grant: even the page-table stage stays out. */
    GiuliaGPU sixth;
    sixth.bootGunyahCaptured=true;
    sixth.firmwareVerified=true;
    sixth.hostPrepared=true;
    sixth.stackInit();
    assert(sixth.stackPlan()==GGSK_POLICY);
    assert(sixth.stackState.blocked==GGSK_HOST_MEMORY);
    assert(sixth.stackState.entered_mask==(GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_FIRMWARE)));
    /* Share gate: with a live owner and channel the transaction stops at the
     * missing authority proof, and the share state stays zero. */
    GiuliaGPU seventh;
    seventh.mode=3;
    seventh.manualMemoryPinned=true;
    seventh.gunyahPinned=true;
    static ggown_state ownerState{};
    ownerState.epoch=0x1000;
    seventh.gunyahOwner=&ownerState;
    assert(seventh.shareCommand()==GGSR_AUTHORITY);
    assert(seventh.shareRuntime.prepared==0 && seventh.shareRuntime.share.initialized==0);
    assert(seventh.shareRuntime.authority_checks==1 && seventh.shareRuntime.refused==1);
    assert(seventh.shareRuntime.snapshot.taken==0);
    seventh.shareReport();
    bool sawAuthority=false,sawPrepare=false;
    for(const auto &line:logs) {
        if(line.find("GIULIA GPU SHARE: authority=unavailable")!=std::string::npos)sawAuthority=true;
        if(line.find("GIULIA GPU SHARE: prepare code=no-authority")!=std::string::npos)sawPrepare=true;
    }
    assert(sawAuthority && sawPrepare);
    /* Without the pin the command refuses before touching the channel at all. */
    GiuliaGPU eighth;
    eighth.mode=3;
    assert(eighth.shareCommand()==GGSR_ARGUMENT);
    assert(eighth.shareRuntime.live_checks==0 && eighth.shareRuntime.authority_checks==0);
    /* Host-only qualification must reject every hardware-side terminal bit,
     * including observe (a register read can fault), and the raw DEV mailbox.
     * Actual worker source is executed; no real MMIO is used in this fixture. */
    GiuliaGPU isolated;
    isolated.hostOnly=true;
    isolated.allowObservation=true;
    isolated.bootGunyahCaptured=true;
    isolated.stackInit();
    assert(gg_terminal_open(&gpuTerminal));
    unsigned before=statuses.load();
    for(unsigned bit=1;bit<=GGT_SHARE;bit<<=1)
        if(bit&GGT_COMMANDS)assert(gg_terminal_request(&gpuTerminal,bit));
    devArgs[0]=123;
    __atomic_store_n(&devState,2u,__ATOMIC_RELEASE);
    std::thread qualification(GiuliaGPU::worker,&isolated,0);
    until([&]{return statuses.load()>before;});
    IOLockLock(isolated.runtimeLock);
    assert(isolated.stackState.writes==0 && isolated.stackState.blocked==GGSK_HOST_MEMORY);
    assert(isolated.stackState.entered_mask==(GGSK_BIT(GGSK_BOOT)|GGSK_BIT(GGSK_FIRMWARE)));
    IOLockUnlock(isolated.runtimeLock);
    isolated.stop(nullptr);qualification.join();
    assert(isolated.devCalls==0 && isolated.gmuCalls==0 && isolated.periodicCalls==0);
    assert(!isolated.entered && isolated.observations==0 && !isolated.manualMemoryPinned);
    assert(isolated.hfiSequence==0 && isolated.hfiPump.received==0);
    assert(isolated.shareRuntime.prepared==0 && isolated.shareRuntime.authority_checks==0);
    assert(isolated.stackState.writes==0 && isolated.stackState.entered_mask==0); // stop unwinds RO stages
    assert(__atomic_load_n(&devState,__ATOMIC_ACQUIRE)==4u);
    bool sawDevRejected=false,sawRequestsRejected=false;
    for(const auto &line:logs) {
        if(line.find("hostonly=1 power_only=0 rejected_dev_token=123 device_dispatch=0")!=std::string::npos)sawDevRejected=true;
        if(line.find("hostonly=1 power_only=0 rejected_requests=")!=std::string::npos)sawRequestsRejected=true;
    }
    assert(sawDevRejected && sawRequestsRejected);
    /* Opt-in power test must fail closed with no native source, while
     * suppressing every legacy device path even though hostOnly is false. */
    GiuliaGPU powerTest;powerTest.mode=3;powerTest.powerOnlyTest=true;
    powerTest.allowObservation=true;powerTest.bootGunyahCaptured=true;powerTest.bootGunyah.epoch=17;
    powerTest.firmwarePowerState=GGPH_PREPARED;
    assert(gg_terminal_open(&gpuTerminal));before=statuses.load();
    for(unsigned bit=1;bit<=GGT_SHARE;bit<<=1)
        if(bit&GGT_COMMANDS)assert(gg_terminal_request(&gpuTerminal,bit));
    devArgs[0]=456;__atomic_store_n(&devState,2u,__ATOMIC_RELEASE);
    std::thread powerWorker(GiuliaGPU::worker,&powerTest,0);
    until([&]{return statuses.load()>before;});
    powerTest.stop(nullptr);powerWorker.join();
    assert(powerStarts==1 && powerStops==0 && powerTest.powerOnlyCode==GGPO_UNAVAILABLE);
    assert(!powerTest.entered && !powerTest.devCalls && !powerTest.gmuCalls && !powerTest.periodicCalls);
    assert(!powerTest.hfiPump.received && !powerTest.manualMemoryPinned);
    /* Uncertain native power cannot let stop free memory behind the source. */
    workerPowerStatus.registered=workerPowerStatus.grant_held=workerPowerStatus.quarantined=1;
    powerStartCode=GGPO_QUARANTINED;
    GiuliaGPU uncertain;uncertain.powerOnlyTest=true;uncertain.mode=3;
    uncertain.bootGunyahCaptured=true;uncertain.bootGunyah.epoch=17;
    assert(gg_terminal_open(&gpuTerminal));before=statuses.load();
    assert(gg_terminal_request(&gpuTerminal,GGT_STATUS));
    std::thread uncertainWorker(GiuliaGPU::worker,&uncertain,0);
    until([&]{return statuses.load()>before;});uncertain.stop(nullptr);uncertainWorker.join();
    assert(powerStarts==2 && powerStops==1 && uncertain.manualMemoryPinned && !uncertain.released);
    workerPowerStatus={};
    for(const auto &line:logs)assert(line.size()<256 && line.back()=='\n');
    puts("Power-only worker: no-source refusal, legacy-path exclusion and uncertain-stop memory retention passed (CPU fixtures only)");
    puts("Host-only isolation: raw DEV and every non-status/stack command blocked; no periodic GPU worker dispatched (host fixtures only)");
    puts("Production GPU worker/status/stop: mode gating, queued requests, observation-stop ordering and retained lifetime passed");
}
'''
with tempfile.TemporaryDirectory() as folder:
    directory=Path(folder)
    test=directory/'service.cpp';test.write_text(fixture)
    binary=directory/'service'
    subprocess.run([os.environ.get('CXX','c++'),'-x','c++','-std=c++17','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-pthread','-DGG_HOST_TEST',
                    '-DGG_GUNYAH_SHARE_STANDALONE',*include_flags(ROOT),
                    str(test),str(ROOT / 'ios16/gpu/giulia_gpu_stack.c'),
                    str(ROOT / 'ios16/gpu/giulia_gpu_gunyah_share.c'),
                    '-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
