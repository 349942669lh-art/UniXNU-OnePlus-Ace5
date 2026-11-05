/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Darwin 22 A750 bring-up service. giulia_gpu=1 prepares pinned firmware and
 * host memory; =2 additionally observes gated power/SMMU registers; =3
 * consumes the explicit UEFI CX/clock preparation report.
 * Automatic startup only prepares memory. Explicit debug ABI2 operations may
 * assign isolated secure memory, configure its context, call SCM/PAS and watch
 * bounded command submission. No accelerator is automatically published.
 * giulia_gpu_hostonly=1 is an additional isolation latch (requires mode 1):
 * only status/stack planning are dispatched; device commands and periodic
 * hardware-side workers are suppressed for firmware/RAM qualification boots.
 */
#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <mach-o/loader.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
extern "C" { extern unsigned long gPhysBase, gPhysSize, gVirtBase; }
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_cache.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_dev.h"
#include "giulia_gpu_platform.h"
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_memstore_mapping.h"
#include "giulia_gpu_ipc.h"
/* giulia_gpu_ipcc_owner.h routes every host/GPU claim and ownership check
 * through the common ledger when the integrated build enables the broker.
 * This includes direct ggi calls in the mapping/fence implementation headers. */
#ifndef GIULIA_PLATFORM_BROKER_ENABLED
#define GIULIA_PLATFORM_BROKER_ENABLED 1
#endif
#include "giulia_gpu_ipcc_owner.h"
#include "giulia_gpu_fence_provider.h"
#include "giulia_gpu_fence_mapping.h"
#include "giulia_gpu_power.h"
#include "giulia_gpu_control.h"
#include "giulia_gpu_gmu_boot.h"
#include "giulia_gpu_terminal.h"
#include "../platform/giulia_platform_power.h"
#include "giulia_gpu_gunyah_boot.h"
#include "giulia_gpu_gunyah_owner.h"
#include "giulia_gpu_hfi_alloc_pool.h"
#include "giulia_gpu_hfi_pump.h"
#include "giulia_gpu_stack.h"
#include "giulia_gpu_submit.h"
#include "giulia_gpu_share_runtime.h"
#include "../vendor/ArmSmmu4K/ArmSmmu4K.h"

/* Host bookkeeping is separate from the IOService's 16 KiB allocation. These
 * records neither allocate secure backing nor grant any device ownership. */
struct gpu_secure_metadata {
    IOBufferMemoryDescriptor *descriptors[GGS_SEC_SEGMENTS];
    uint64_t physical[GGS_SEC_SEGMENTS];
};

static struct gg_terminal_mailbox gpuTerminal;
static unsigned devState;
static uint32_t devArgs[6];
static struct ggi_owner gpuIpcOwner;
static struct ggf_registration gpuFenceProvider;
static void gpuHfiBarrier(void *);
extern "C" int giulia_gpu_fence_register(const struct ggf_provider *p)
{ return ggfp_register(&gpuFenceProvider,p); }
extern "C" int giulia_gpu_fence_ipcc_claim(const void *identity,uint64_t epoch)
{
    const struct ggf_provider *p=ggfp_get(&gpuFenceProvider);
    if(!p || p->identity!=identity || p->epoch!=epoch)return 0;
    const void *gpu=__atomic_load_n(&gpuIpcOwner.owners[GGI_GPU-1],__ATOMIC_ACQUIRE);
    return ggi_pair_fence(&gpuIpcOwner,gpu,identity,epoch);
}
extern "C" int giulia_gpu_fence_ipcc_owns(const void *identity,uint64_t epoch)
{
    const struct ggf_provider *p=ggfp_get(&gpuFenceProvider);
    if(!p || p->identity!=identity || p->epoch!=epoch)return 0;
    const void *gpu=__atomic_load_n(&gpuIpcOwner.owners[GGI_GPU-1],__ATOMIC_ACQUIRE);
    return ggi_fence_owns(&gpuIpcOwner,gpu,identity,epoch);
}
extern "C" int giulia_gpu_terminal(const char *command)
{
    if(!strncmp(command,"gpu.dev ",8)) {
        uint32_t args[6]; unsigned expected=0; int debug=0;
        PE_parse_boot_argn("giulia_debug",&debug,sizeof(debug));
        if(!debug || !ggd_parse(command+8,args) ||
           __atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)!=GGT_READY ||
           !__atomic_compare_exchange_n(&devState,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            IOLog("GIULIA GPU DEV: rejected busy-or-unavailable-or-syntax\n");return 1;
        }
        memcpy(devArgs,args,sizeof(args));__atomic_store_n(&devState,2u,__ATOMIC_RELEASE);return 1;
    }
    unsigned request=gg_terminal_command(command);
    if(!request)return 0;
    if(!gg_terminal_request(&gpuTerminal,request))
        IOLog("GIULIA GPU: command=%s service=unavailable stage=%s mode=%u hardware_ready=0 acceleration=0\n",command,
            gg_terminal_stage_name(__atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)),
            __atomic_load_n(&gpuTerminal.mode,__ATOMIC_ACQUIRE));
    return 1;
}

class GiuliaGPU : public IOService {
    OSDeclareDefaultStructors(GiuliaGPU)
    struct ggr_report bootGunyah;
    bool bootGunyahCaptured;
    void readGunyahHandoff();
    struct ggown_state *gunyahOwner=nullptr;
    thread_t gunyahWorker=nullptr;
    unsigned gunyahStopLatch=0;
    bool gunyahPinned=false,gunyahWorkerArmed=false;
    int gunyahCommand(uint32_t,uint32_t,uint32_t,uint32_t);
    void gunyahPeriodic();
    void stopGunyah();
    static int gunyahPolicy(void *,uint64_t);
    static uint64_t gunyahNow(void *);
    static int gunyahDelay(void *,unsigned);
    static int gunyahNotification(void *,uint32_t,const void *,size_t);
    IOMemoryDescriptor *firmwareMemory;
    IOMemoryMap *firmwareMap;
    IOBufferMemoryDescriptor *tableMemory;
    IOBufferMemoryDescriptor *devMemory[GGD_CHUNKS],*gpuTableMemory;
    struct ggd_pool devPool;
    uint32_t devPacket[255];bool devWritten[255];
    uint64_t gmuRoot,gpuRoot;
    IOBufferMemoryDescriptor *memstoreMemory;
    uint32_t memstoreFlags;
    bool memstoreMapped;
    bool ipcMapped;
    uint32_t ipcFlags;
    bool fenceMapped;
    uint32_t fenceFlags;
    const struct ggf_provider *fenceProvider;
    int fenceAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int fenceStatus();
    int auditFenceMappings();
    int ipcAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int auditIpcMappings();
    bool prepareInitialMappings();
    int memstoreAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int memstoreRead(uint32_t);
    int auditMemstoreMappings();
    IOBufferMemoryDescriptor **secureData;
    struct gpu_secure_metadata *secureMetadata;
    bool allocateSecureMetadata();
    void releaseSecureMetadata();
    IOBufferMemoryDescriptor *secureTables,*scmMemory,*zapMetadata;
    IOMemoryDescriptor *platformMemory[4],*zapMemory;
    IOMemoryMap *platformMaps[4],*zapMap;
    uint64_t *securePA,secureRoot,scmPA,zapMetadataPA;
    struct ggs_secure_slot secureSlots[GGS_SEC_CHUNKS];
    struct ggs_state scmState;
    bool platformPrepared,platformOwned,platformPoisoned,secureContextActive,zapAuthenticated;
    bool secureOwnershipReady;
    bool devMappingsManaged;
    bool devUcode[GGD_MAX];
    int configureDevMappings();
    int mapDevAllocation(unsigned,bool clearOnly=false);
    int syncDevMappings();
    int auditDevMappings();
    int syncSecureMappings();
    int auditSecureMappings();
    unsigned platformEpoch,platformSnapshotSequence;
    bool platformPrepare();
    bool platformPower();
    bool platformIdle();
    uint32_t platformRead(uint64_t);
    void platformWrite(uint64_t,uint32_t);
    bool platformPoll(uint64_t,uint32_t,uint32_t);
    int platformCall(uint32_t,uint32_t,const uint64_t *,bool);
    int platformAvailable(uint32_t);
    int platformAssign(uint64_t,uint64_t,bool);
    int platformAssignBatch(unsigned,unsigned);
    int platformCommand(unsigned);
    int secureAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int platformWatch(unsigned,unsigned);
    int platformSnapshot(unsigned);
    int platformZap();
    static int platformScmCall(void *,struct ggs_regs *);
    static int platformScmDelay(void *,unsigned);
    static int platformScmPrerequisites(void *,unsigned);
    bool prepareDevMemory(ArmSmmu4KRange *,size_t *);
    void devCommand(const uint32_t *);
    uint8_t *devPointer(uint32_t);
    void devAllocation(unsigned);
    IOBufferMemoryDescriptor *allocations[GG_FW_MAX_BLOCKS];
    IOMemoryDescriptor *registerMemory[GGP_WINDOWS];
    IOMemoryMap *registerMaps[GGP_WINDOWS];
    IOLock *runtimeLock;
    bool workerRunning, stopping, terminalOwner;
    unsigned mode, reservationCount, tablePages, leafPages, observations;
    bool hostOnly;
    bool firmwareVerified, hostPrepared;
    bool manualMemoryPinned;
    unsigned hfiIndex,hfiSequence,hfiLastHeader;
    /* F2H_MSG_MEM_ALLOC: the responder runs over this service's own devPool,
     * and the pump only moves while the HFI memory is pinned and the platform
     * is healthy. Nothing here owns memory the pool did not allocate. */
    struct gga_pool hfiAllocPool;
    struct gga_ops hfiAllocOps;
    struct ggpp_state hfiPump;
    uint32_t hfiPumpScratch[64];
    bool hfiAllocReady;
    void hfiAllocInit();
    int hfiPumpOnce(unsigned);
    void hfiAllocReport();
    static uint64_t hfiPoolAddress(void *,unsigned,unsigned);
    static int hfiPoolMap(void *,unsigned,unsigned);
    static int hfiPoolUnmap(void *,unsigned,unsigned);
    static int hfiPoolPublish(void *,unsigned);
    static int hfiPoolBarrier(void *);
    /* Stage machine. Only the stages this service can actually observe are
     * registered; every later stage stays unbound so the plan reports where the
     * chain stops instead of skipping it. */
    struct ggsk stackState;
    struct ggsk_ops stackOps[GGSK_STAGES];
    bool stackReady;
    void stackInit();
    int stackPlan();
    void stackReport();
    static int stackProbeBoot(void *,uint64_t *);
    static int stackProbeFirmware(void *,uint64_t *);
    static int stackProbeMemory(void *,uint64_t *);
    static int stackEnterNoop(void *);
    static int stackLeaveNoop(void *);
    /* Primary share transaction: gated by the platform authority backend, which
     * does not exist yet, so this path can only refuse today. The control page
     * is an independent 16 KiB Normal NC allocation outside the carveout. */
    struct ggsr_state shareRuntime;
    struct ggsh_owner shareOwner;
    struct ggsr_gate shareGate;
    IOBufferMemoryDescriptor *shareControl;
    uint8_t *shareControlCPU;
    uint64_t shareControlPA;
    bool shareControlReady;
    bool shareControlAllocate();
    int shareCommand();
    void shareReport();
    static int shareLive(void *,uint64_t);
    static int shareIdentity(void *,uint64_t,uint16_t *,uint16_t *);
    static int shareAuthorize(void *,uint64_t,uint16_t,uint16_t,uint64_t,uint32_t);
    static int sharePublish(void *,const void *,size_t);
    /* GMU boot adapter: the pinned gen7 sequence over the owned GMU window.
     * preconditions require the UEFI power handoff and a live GPU SMMU. */
    struct gg_gmu_state gmuState;
    IOMemoryDescriptor *gmuMemory;
    IOMemoryMap *gmuMap;
    bool gmuWindowMapped,gmuActive,gmuFaulted,smmuActive;
    unsigned gmuLoadCalls,gmuStartCalls,smmuAsid,smmuBank,smmuSlot;
    bool gmuEnsureWindow();
    struct ggb_config gmuConfigValues();
    struct gg_gmu_ops gmuOps();
    int gmuCommand(unsigned);
    void gmuReport();
    static int gmuReadWord(void *,uint32_t,uint32_t *);
    static int gmuWriteWord(void *,uint32_t,uint32_t);
    static int gmuBarrierFn(void *);
    static int gmuWaitFn(void *,unsigned);
    static int gmuPreconditions(void *);
    static int gmuRamMatches(void *,uint32_t,const uint8_t *,size_t);
    static int gmuConfigureFn(void *);
    static int gmuHfiFn(void *);
    static int gmuHealthyFn(void *);
    struct ggp_report powerObservation;
    struct gpp_report firmwarePower;
    unsigned firmwarePowerState;
    bool loadFirmware(struct gg_pack *pack);
    bool prepareMemory(const struct gg_pack *pack);
    void readPowerHandoff();
    void observePower();
    void reportStatus();
    void hfiCommand(unsigned);
    void closeTerminal(unsigned);
    void releaseResources();
    static int readRegister(void *, unsigned, uint32_t, uint32_t *);
    static void worker(void *,wait_result_t);
public:
    bool init(OSDictionary *dictionary=0) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
};
OSDefineMetaClassAndStructors(GiuliaGPU, IOService)

bool GiuliaGPU::allocateSecureMetadata()
{
    if(secureMetadata || secureData || securePA)return false;
    struct gpu_secure_metadata *records=(struct gpu_secure_metadata *)IOMalloc(sizeof(*records));
    if(!records)return false;
    bzero(records,sizeof(*records));
    secureMetadata=records;secureData=records->descriptors;securePA=records->physical;
    return true;
}

void GiuliaGPU::releaseSecureMetadata()
{
    /* Match releaseResources: uncertain device ownership retains bookkeeping
     * as well as the underlying mappings until reset. */
    if(!secureMetadata || manualMemoryPinned)return;
    IOFree(secureMetadata,sizeof(*secureMetadata));
    secureMetadata=NULL;secureData=NULL;securePA=NULL;
}

bool GiuliaGPU::init(OSDictionary *dictionary)
{
    hostOnly=false;
    firmwareMemory=NULL; firmwareMap=NULL; tableMemory=NULL;
    gpuTableMemory=NULL;gmuRoot=gpuRoot=0;
    memstoreMemory=NULL;memstoreFlags=0;memstoreMapped=false;
    ipcMapped=false;ipcFlags=0;
    fenceMapped=false;fenceFlags=0;fenceProvider=NULL;
    secureMetadata=NULL;secureData=NULL;securePA=NULL;bzero(secureSlots,sizeof(secureSlots));
    bzero(platformMemory,sizeof(platformMemory));bzero(platformMaps,sizeof(platformMaps));bzero(&scmState,sizeof(scmState));
    secureTables=scmMemory=zapMetadata=NULL;zapMemory=NULL;zapMap=NULL;
    secureRoot=scmPA=zapMetadataPA=0;platformEpoch=platformSnapshotSequence=0;
    platformPrepared=platformOwned=platformPoisoned=secureContextActive=zapAuthenticated=secureOwnershipReady=false;
    devMappingsManaged=false;bzero(devUcode,sizeof(devUcode));
    bzero(devMemory,sizeof(devMemory));bzero(&devPool,sizeof(devPool));devPool.next=GGD_CACHE_BYTES;
    bzero(devPacket,sizeof(devPacket));bzero(devWritten,sizeof(devWritten));
    runtimeLock=NULL;workerRunning=stopping=terminalOwner=false;
    mode=reservationCount=tablePages=leafPages=observations=0;
    firmwareVerified=hostPrepared=false;bzero(&powerObservation,sizeof(powerObservation));
    manualMemoryPinned=false;hfiIndex=hfiSequence=hfiLastHeader=0;
    bzero(&hfiAllocPool,sizeof(hfiAllocPool));bzero(&hfiAllocOps,sizeof(hfiAllocOps));
    bzero(&hfiPump,sizeof(hfiPump));bzero(hfiPumpScratch,sizeof(hfiPumpScratch));
    hfiAllocReady=false;
    bzero(&stackState,sizeof(stackState));bzero(stackOps,sizeof(stackOps));stackReady=false;
    bzero(&shareRuntime,sizeof(shareRuntime));bzero(&shareOwner,sizeof(shareOwner));
    bzero(&shareGate,sizeof(shareGate));
    shareControl=NULL;shareControlCPU=NULL;shareControlPA=0;shareControlReady=false;
    bzero(&gmuState,sizeof(gmuState));
    gmuMemory=NULL;gmuMap=NULL;
    gmuWindowMapped=gmuActive=gmuFaulted=smmuActive=false;
    gmuLoadCalls=gmuStartCalls=smmuAsid=smmuBank=smmuSlot=0;
    firmwarePowerState=GGPH_INVALID;bzero(&firmwarePower,sizeof(firmwarePower));
    bzero(allocations,sizeof(allocations));
    bzero(registerMemory,sizeof(registerMemory)); bzero(registerMaps,sizeof(registerMaps));
    if(!IOService::init(dictionary))return false;
#if GIULIA_PLATFORM_BROKER_ENABLED
    /* Process-context bootstrap; IRQ/fence ownership queries never allocate. */
    if(gpr_global_initialize()!=GPR_OK)return false;
#endif
    runtimeLock=IOLockAlloc();return runtimeLock && allocateSecureMetadata();
}

static bool gpuFirmwareError(unsigned code,uint64_t at,const char *reason,uint64_t expected,uint64_t observed)
{
    IOLog("GIULIA GPU: subsystem=firmware phase=validate result=fail code=%u location=%#llx check=%s expected=%#llx observed=%#llx device_writes=0 cause=unconfirmed\n",
        code,(unsigned long long)at,reason,(unsigned long long)expected,(unsigned long long)observed);
    return false;
}

bool GiuliaGPU::loadFirmware(struct gg_pack *pack)
{
    const struct mach_header_64 *h=&_mh_execute_header;
    struct segment_command_64 segment={}; bool found=false;
    if (h->magic!=MH_MAGIC_64) return gpuFirmwareError(1,0,"mach-magic",MH_MAGIC_64,h->magic);
    if (!h->ncmds || h->ncmds>128) return gpuFirmwareError(2,16,"command-count-range-1-to-128",128,h->ncmds);
    if (h->sizeofcmds>0x10000) return gpuFirmwareError(3,20,"command-table-size-limit",0x10000,h->sizeofcmds);
    const uint8_t *p=(const uint8_t *)(h+1); size_t left=h->sizeofcmds;
    for (unsigned i=0;i<h->ncmds;++i) {
        struct load_command c;
        if (left<sizeof(c)) return gpuFirmwareError(4,p-(const uint8_t *)h,"truncated-command",sizeof(c),left);
        memcpy(&c,p,sizeof(c));
        if (c.cmdsize<8 || c.cmdsize>left) return gpuFirmwareError(5,p-(const uint8_t *)h,"command-size-out-of-range",left,c.cmdsize);
        if (c.cmdsize&7) return gpuFirmwareError(6,p-(const uint8_t *)h,"command-size-alignment-remainder",0,c.cmdsize&7);
        if (c.cmd==LC_SEGMENT_64) {
            struct segment_command_64 s;
            if (c.cmdsize<sizeof(s)) return gpuFirmwareError(7,p-(const uint8_t *)h,"truncated-segment",sizeof(s),c.cmdsize);
            memcpy(&s,p,sizeof(s));
            if (!strncmp(s.segname,"__GPU_FW",16)) {
                if (found) return gpuFirmwareError(8,p-(const uint8_t *)h,"duplicate-GPU-segment",1,2);
                if (c.cmdsize!=sizeof(s) || s.nsects || s.flags) {
                    IOLog("GIULIA GPU: subsystem=firmware phase=segment-shape cmdsize=%u expected_cmdsize=%lu sections=%u flags=%#x\n",c.cmdsize,(unsigned long)sizeof(s),s.nsects,s.flags);
                    return gpuFirmwareError(9,p-(const uint8_t *)h,"GPU-segment-shape",sizeof(s),c.cmdsize);
                }
                if(s.initprot!=VM_PROT_READ || s.maxprot!=VM_PROT_READ) {
                    IOLog("GIULIA GPU: subsystem=firmware phase=segment-permissions initial=%#x maximum=%#x expected=1\n",s.initprot,s.maxprot);
                    return gpuFirmwareError(10,p-(const uint8_t *)h,"GPU-segment-not-read-only",1,s.initprot);
                }
                segment=s; found=true;
            }
        }
        p+=c.cmdsize; left-=c.cmdsize;
    }
    if (left) return gpuFirmwareError(11,0,"unconsumed-load-command-bytes",0,left);
    if (!found) return gpuFirmwareError(12,0,"GPU-firmware-segment-missing",1,0);
    if (segment.filesize!=GG_FIRMWARE_PACK_BYTES) return gpuFirmwareError(13,segment.vmaddr,"pack-size",GG_FIRMWARE_PACK_BYTES,segment.filesize);
    if (segment.vmsize<segment.filesize) return gpuFirmwareError(14,segment.vmaddr,"virtual-size-smaller-than-file",segment.filesize,segment.vmsize);
    if (segment.vmaddr<gVirtBase) return gpuFirmwareError(15,segment.vmaddr,"segment-below-kernel-window",gVirtBase,segment.vmaddr);
    if (segment.vmaddr-gVirtBase>gPhysSize) return gpuFirmwareError(16,segment.vmaddr,"segment-start-outside-window",gPhysSize,segment.vmaddr-gVirtBase);
    if (segment.vmsize>gPhysSize-(segment.vmaddr-gVirtBase)) return gpuFirmwareError(17,segment.vmaddr,"segment-end-outside-window",gPhysSize-(segment.vmaddr-gVirtBase),segment.vmsize);
    if (gPhysBase>UINT64_MAX-(segment.vmaddr-gVirtBase)) return gpuFirmwareError(18,segment.vmaddr,"physical-address-overflow",UINT64_MAX-(segment.vmaddr-gVirtBase),gPhysBase);
    uint64_t pa=gPhysBase+segment.vmaddr-gVirtBase;
    firmwareMemory=IOMemoryDescriptor::withPhysicalAddress(pa,segment.filesize,kIODirectionIn);
    if (!firmwareMemory) return gpuFirmwareError(19,pa,"physical-memory-descriptor-null",segment.filesize,0);
    firmwareMap=firmwareMemory->map(kIOMapReadOnly|kIOMapCopybackCache);
    if (!firmwareMap) return gpuFirmwareError(20,pa,"read-only-RAM-map-null",segment.filesize,0);
    if (!gg_pack_open((const void *)firmwareMap->getVirtualAddress(),segment.filesize,pack)) {
        unsigned char digest[32];char actual[65],expected[65];
        gwh_sha256((const void *)firmwareMap->getVirtualAddress(),segment.filesize,digest);
        for(unsigned i=0;i<32;++i) {snprintf(actual+2*i,3,"%02x",digest[i]);snprintf(expected+2*i,3,"%02x",gg_firmware_pack_sha256[i]);}
        IOLog("GIULIA GPU: subsystem=firmware phase=pack-validation result=fail expected_sha256=%s observed_sha256=%s digest_match=%u\n",expected,actual,gwh_equal(digest,gg_firmware_pack_sha256));
        return gpuFirmwareError(21,pa,"container-validation-failed",GG_FIRMWARE_PACK_BYTES,segment.filesize);
    }
    IOLog("GIULIA GPU: firmware pack mapped read-only and digest verified\n");
    firmwareVerified=true;
    return true;
}

static IOBufferMemoryDescriptor *gpuAllocateHostMemory(size_t bytes,bool shared=false)
{
    /* InhibitCache is Device memory on arm64: bzero's DC ZVA faults even
     * at a cache-line-aligned address. GMU RAM and tables are Normal RAM.
     * Bypass the system mapper: this service owns the GPU SMMU separately. */
    return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|
        (shared?kIOMapWriteCombineCache:kIOMapCopybackCache),bytes,0x4000);
}

static bool gpuHostRangeValid(const void *cpu,uint64_t pa,uint64_t contiguous,size_t bytes)
{
    const uint64_t limit=1ULL<<36;
    return cpu && !((uintptr_t)cpu&0x3fff) && bytes && !(bytes&0x3fff) &&
        contiguous>=bytes && !(pa&0x3fff) && pa>=gPhysBase &&
        bytes<=gPhysSize && pa-gPhysBase<=gPhysSize-bytes &&
        pa<limit && bytes<=limit-pa;
}

bool GiuliaGPU::prepareMemory(const struct gg_pack *pack)
{
    /* Keep large parsing scratch off the kernel stack. */
    struct gg_fw_block *blocks=(struct gg_fw_block *)IOMalloc(sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    struct gg_reservation *plan=(struct gg_reservation *)IOMalloc(sizeof(*plan)*GG_FW_MAX_BLOCKS);
    ArmSmmu4KRange *ranges=(ArmSmmu4KRange *)IOMalloc(sizeof(*ranges)*GG_FW_MAX_BLOCKS);
    size_t count=0, reservations=0,firmwareReservations=0; bool ok=false;
    int prepared=0;
    if (!blocks || !plan || !ranges) {
        IOLog("GIULIA GPU: subsystem=memory phase=allocate-plan-scratch result=fail blocks_allocated=%u reservations_allocated=%u ranges_allocated=%u capacity=%u cause=unconfirmed\n",
            blocks!=NULL,plan!=NULL,ranges!=NULL,GG_FW_MAX_BLOCKS);
    } else {
        enum gg_fw_status status=gg_fw_plan(pack->asset[0].data,pack->asset[0].size,blocks,GG_FW_MAX_BLOCKS,&count);
        IOLog("GIULIA GPU: subsystem=gmu phase=parse-firmware code=%u firmware_bytes=%lu blocks=%lu capacity=%u\n",
            (unsigned)status,(unsigned long)pack->asset[0].size,(unsigned long)count,GG_FW_MAX_BLOCKS);
        if(status==GG_FW_OK) {
            status=gg_memory_plan(blocks,count,plan,GG_FW_MAX_BLOCKS,&reservations);
            IOLog("GIULIA GPU: subsystem=memory phase=plan-reservations code=%u blocks=%lu reservations=%lu capacity=%u\n",
                (unsigned)status,(unsigned long)count,(unsigned long)reservations,GG_FW_MAX_BLOCKS);
            prepared=status==GG_FW_OK;
            if(prepared) {
                firmwareReservations=reservations;
                if(reservations>GG_FW_MAX_BLOCKS-3 ||
                   (uint64_t)plan[reservations-1].iova+plan[reservations-1].bytes>GGH_IOVA)
                    prepared=0;
                else {
                    plan[reservations++]={GGH_IOVA,GGH_BYTES,GG_KERNEL,0};
                    plan[reservations++]={GGH_LOG_IOVA,GGH_LOG_BYTES,GG_KERNEL,0};
                    plan[reservations++]={GGH_VRB_IOVA,GGH_VRB_BYTES,GG_KERNEL,0};
                }
            }
        }
    }
    if (prepared) {
        ok=true;

        for (size_t i=0;i<reservations;++i) {
            size_t bytes=(plan[i].bytes+0x3fff)&~(size_t)0x3fff;
            bool shared=i>=firmwareReservations;
            allocations[i]=gpuAllocateHostMemory(bytes,shared);
            if (!allocations[i]) { IOLog("GIULIA GPU: subsystem=memory phase=allocate result=fail reservation=%lu iova=%#x allocation_bytes=%lu reason=descriptor-null\n",(unsigned long)i,plan[i].iova,(unsigned long)bytes); ok=false; break; }
            IOByteCount contiguous=0;
            uint64_t pa=allocations[i]->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
            void *cpu=allocations[i]->getBytesNoCopy();
            if (!gpuHostRangeValid(cpu,pa,contiguous,bytes)) {
                IOLog("GIULIA GPU: subsystem=memory phase=validate result=fail reservation=%lu pa=%#llx requested=%lu contiguous=%llu cpu_mapped=%u reason=host-window-or-gpu-36bit-range-or-alignment\n",(unsigned long)i,(unsigned long long)pa,(unsigned long)bytes,(unsigned long long)contiguous,cpu!=NULL);
                ok=false; break; }
            ranges[i].iova=plan[i].iova; ranges[i].physical=pa; ranges[i].bytes=plan[i].bytes;
            ranges[i].permissions=ArmSmmu4KRead|ArmSmmu4KWrite|ArmSmmu4KPrivileged|
                (plan[i].region==GG_ICACHE?ArmSmmu4KExecute:0);
            ranges[i].writeback=0; ranges[i].shareability=2;
            IOLog("GIULIA GPU: subsystem=memory phase=initialize reservation=%lu iova=%#x pa=%#llx bytes=%lu cpu_cache=%s dma_active=0\n",
                (unsigned long)i,plan[i].iova,(unsigned long long)pa,(unsigned long)bytes,
                shared?"normal-nc":"normal-copyback");
            bzero(cpu,bytes);
            /* Prepare only ordinary host RAM. TCM stays untouched. */
            for (size_t j=0;j<count;++j) {
                const struct gg_fw_block *b=blocks+j;
                if (b->size && b->address>=plan[i].iova && b->size<=plan[i].bytes &&
                    b->address-plan[i].iova<=plan[i].bytes-b->size)
                    memcpy((uint8_t *)cpu+b->address-plan[i].iova,
                        pack->asset[0].data+b->payload_offset,b->size);
            }
            if(i==firmwareReservations && ggh_init(cpu,bytes,GGH_IOVA)!=GGH_OK) {
                ok=false;break;
            }
            if (!giulia_gpu_cache_publish(cpu,bytes)) {
                IOLog("GIULIA GPU: subsystem=memory phase=cache-publish result=fail reservation=%lu\n",(unsigned long)i);
                ok=false;break;
            }
            IOLog("GIULIA GPU: GMU RAM iova=%#x bytes=%#x persistent=%u; not mapped to GPU\n",
                plan[i].iova,plan[i].bytes,plan[i].persistent);
        }
        if (ok) {
            hfiIndex=(unsigned)firmwareReservations;
            size_t mapCount=reservations;
            ok=prepareDevMemory(ranges,&mapCount);
            const size_t capacity=128, bytes=capacity*4096;
            tableMemory=gpuAllocateHostMemory(bytes);
            IOByteCount contiguous=0;
            uint64_t pa=tableMemory?tableMemory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
            void *cpu=tableMemory?tableMemory->getBytesNoCopy():NULL;
            if (!gpuHostRangeValid(cpu,pa,contiguous,bytes)) {
                IOLog("GIULIA GPU: subsystem=smmu phase=table-allocation result=fail pa=%#llx requested=%lu contiguous=%llu reason=invalid-owned-allocation\n",(unsigned long long)pa,(unsigned long)bytes,(unsigned long long)contiguous);
                ok=false;
            } else {
                /* Clear the entire owned pool, including unused table pages. */
                IOLog("GIULIA GPU: subsystem=smmu phase=initialize-table-pool pa=%#llx bytes=%lu cpu_cache=normal-copyback active=0\n",(unsigned long long)pa,(unsigned long)bytes);
                bzero(cpu,bytes);
                ArmSmmu4KConfig config={32,36,pa,65536}; ArmSmmu4KResult result;
                ArmSmmu4KStatus status=ArmSmmu4KBuild(&config,ranges,mapCount,
                    (uint64_t *)tableMemory->getBytesNoCopy(),capacity,&result);
                IOLog("GIULIA GPU: subsystem=smmu phase=build-inactive-tables result=%s code=%u root_pa=%#llx capacity_pages=%lu table_pages=%lu leaf_pages=%lu input_bits=32 output_bits=36 active=0\n",
                    status==ArmSmmu4KSuccess?"ok":"fail",(unsigned)status,(unsigned long long)pa,
                    (unsigned long)capacity,(unsigned long)result.table_pages,(unsigned long)result.leaf_pages);
                ok=ok && status==ArmSmmu4KSuccess;gmuRoot=pa;
                if(ok && !prepareInitialMappings()) {
                    IOLog("GIULIA GPU: subsystem=smmu phase=cache-publish result=fail\n");
                    ok=false;
                }
                if (ok) {
                    tablePages=(unsigned)result.table_pages+1;leafPages=(unsigned)result.leaf_pages;
                    setProperty("InactiveSmmuTablePages",(unsigned long long)tablePages,32);
                }
            }
        }
        if (ok) {
            reservationCount=(unsigned)reservations;hostPrepared=true;
            setProperty("HostMemoryCachePolicy","normal-copyback-explicit-clean");
            IOLog("GIULIA GPU: host RAM cache publication complete; buffers=%lu tables=1 dma_active=0\n",(unsigned long)reservations);
            setProperty("GMUReservationCount",(unsigned long long)reservations,32);
            IOLog("GIULIA GPU: host preparation complete; blocks=%lu reservations=%lu; hardware inactive\n",
                (unsigned long)count,(unsigned long)reservations);
        }
    }
    if (blocks) IOFree(blocks,sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    if (plan) IOFree(plan,sizeof(*plan)*GG_FW_MAX_BLOCKS);
    if (ranges) IOFree(ranges,sizeof(*ranges)*GG_FW_MAX_BLOCKS);
    return ok;
}

int GiuliaGPU::readRegister(void *cookie,unsigned window,uint32_t offset,uint32_t *value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    const uint64_t bases[GGP_WINDOWS]={0x171000,0x3d90000,0x3da0000};
    const size_t sizes[GGP_WINDOWS]={0x1000,0x9800,0x1000};
    if (!value || window>=GGP_WINDOWS || (offset&3) || offset>sizes[window]-4) {
        IOLog("GIULIA GPU: subsystem=mmio phase=validate-read result=fail native_code=1 window=%u offset=%#x output_present=%u check=range-or-alignment\n",window,offset,value!=NULL);
        return 1;
    }
    /* Map lazily: a failed prerequisite cannot even map the later window. */
    if (!self->registerMaps[window]) {
        if(!self->registerMemory[window])
            self->registerMemory[window]=IOMemoryDescriptor::withPhysicalAddress(bases[window],sizes[window],kIODirectionIn);
        if (!self->registerMemory[window]) {
            IOLog("GIULIA GPU: subsystem=mmio phase=create-descriptor result=fail native_code=2 base=%#llx bytes=%lu window=%u cause=unconfirmed\n",
                (unsigned long long)bases[window],(unsigned long)sizes[window],window);
            return 2;
        }
        self->registerMaps[window]=self->registerMemory[window]->map(kIOMapReadOnly|kIOMapInhibitCache);
        if (!self->registerMaps[window]) {
            IOLog("GIULIA GPU: subsystem=mmio phase=map-read-only result=fail native_code=3 base=%#llx bytes=%lu window=%u cause=unconfirmed\n",
                (unsigned long long)bases[window],(unsigned long)sizes[window],window);
            return 3;
        }
    }
    *value=*(volatile uint32_t *)(self->registerMaps[window]->getVirtualAddress()+offset);
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

/* Independent 16 KiB Normal NC control page for the share transaction. It must
 * live outside the fence carveout, outside the development pool and inside the
 * 36-bit GPU address window; the transaction pins it until reset once
 * ggsh_init() accepted it. */
bool GiuliaGPU::shareControlAllocate()
{
    IOByteCount contiguous=0;
    uint64_t pa=0;
    void *cpu=NULL;
    if(shareControlReady)return true;
    if(shareControl)return false;
    shareControl=gpuAllocateHostMemory(GGS_CONTROL_BYTES,true);
    if(!shareControl)return false;
    pa=shareControl->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone);
    cpu=shareControl->getBytesNoCopy();
    if(!gpuHostRangeValid(cpu,pa,contiguous,GGS_CONTROL_BYTES)) {
        OSSafeReleaseNULL(shareControl);
        return false;
    }
    if(pa<GGF_CARVEOUT_PHYSICAL+GGF_CARVEOUT_BYTES &&
       GGF_CARVEOUT_PHYSICAL<pa+GGS_CONTROL_BYTES) {
        IOLog("GIULIA GPU SHARE: control rejected reason=overlaps-carveout pa=%#llx\n",
            (unsigned long long)pa);
        OSSafeReleaseNULL(shareControl);
        return false;
    }
    bzero(cpu,GGS_CONTROL_BYTES);
    if(!giulia_gpu_cache_publish(cpu,GGS_CONTROL_BYTES)) {
        OSSafeReleaseNULL(shareControl);
        return false;
    }
    gpuHfiBarrier(NULL);
    shareControlCPU=(uint8_t *)cpu;
    shareControlPA=pa;
    shareControlReady=true;
    IOLog("GIULIA GPU SHARE: control bytes=%u pa=%#llx aligned=1 outside_carveout=1 published=1\n",
        (unsigned)GGS_CONTROL_BYTES,(unsigned long long)pa);
    return true;
}

bool GiuliaGPU::start(IOService *provider)
{
    int requested=0;
    if (!PE_parse_boot_argn("giulia_gpu",&requested,sizeof(requested)) || requested<1 || requested>3) return false;
    int isolation=0;
    PE_parse_boot_argn("giulia_gpu_hostonly",&isolation,sizeof(isolation));
    if(!gg_terminal_hostonly_valid(requested,isolation))return false;
    hostOnly=isolation==1;
    IORegistryEntry *root=IORegistryEntry::fromPath("/",gIODTPlane);
    if (!root) return false;
    OSData *model=OSDynamicCast(OSData,root->getProperty("physical-model"));
    const char expected[]="OnePlus Ace 5";
    bool board=model && model->getLength()==sizeof(expected) && !memcmp(model->getBytesNoCopy(),expected,sizeof(expected));
    root->release();
    if (!board || !IOService::start(provider)) return false;
    if(!gg_terminal_open(&gpuTerminal)){IOService::stop(provider);return false;}
    terminalOwner=true;__atomic_store_n(&devState,0u,__ATOMIC_RELEASE);mode=(unsigned)requested;
    __atomic_store_n(&gpuTerminal.mode,mode,__ATOMIC_RELEASE);
    __atomic_store_n(&gpuTerminal.stage,(unsigned)GGT_PREPARING,__ATOMIC_RELEASE);
    /* Preserve firmware diagnostics even if subsequent RAM preparation fails. */
    readGunyahHandoff();
    if(mode==3)readPowerHandoff();
    struct gg_pack pack={};
    if (!loadFirmware(&pack) || !prepareMemory(&pack)) {
        IOLog("GIULIA GPU: preparation failed; no device writes performed\n");
        closeTerminal(GGT_FAILED);
        releaseResources();gg_terminal_finish(&gpuTerminal);terminalOwner=false;
        IOService::stop(provider);return false;
    }
    setProperty("GPUChipID",(unsigned long long)0x43051401,32);
    setProperty("GPUModel","Adreno750v2"); setProperty("HardwareReady",false);
    setProperty("HostOnlyIsolation",hostOnly);
    IOLog("GIULIA GPU: hostonly=%u mode=%u acceleration=0\n",hostOnly,mode);
    setProperty("BringupStage","host-memory-prepared");
    /* Pool adapter only: it establishes no mapping and performs no device
     * write until the worker is pinned and the platform is healthy. */
    hfiAllocInit();
    stackInit();
    /* The worker retains this service and owns every post-start observation.
     * A USB command cannot map registers or race stop/free through a pointer. */
    workerRunning=true;retain();thread_t thread;
    kern_return_t error=kernel_thread_start(worker,this,&thread);
    if(error!=KERN_SUCCESS) {
        workerRunning=false;release();closeTerminal(GGT_FAILED);
        releaseResources();gg_terminal_finish(&gpuTerminal);terminalOwner=false;
        IOService::stop(provider);return false;
    }
    thread_deallocate(thread);
    registerService();
    return true;
}

void GiuliaGPU::readGunyahHandoff()
{
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *data=chosen?OSDynamicCast(OSData,chosen->getProperty("giulia-gpu-gunyah")):NULL;
    bootGunyahCaptured=ggr_boot_decode(data?data->getBytesNoCopy():NULL,
        data?data->getLength():0,&bootGunyah)!=0;
    if(chosen)chosen->release();
    setProperty("BootGunyahResourcesCaptured",bootGunyahCaptured);
    if(bootGunyahCaptured)setProperty("BootGunyahResourceReport",&bootGunyah,sizeof(bootGunyah));
    IOLog("GIULIA GPU: boot Gunyah resources captured=%u epoch=%#llx fdt_bytes=%u irq_lease=0 hypervisor_verified=0 provider_ready=0\n",
        bootGunyahCaptured,(unsigned long long)bootGunyah.epoch,bootGunyah.fdt_bytes);
}

void GiuliaGPU::readPowerHandoff()
{
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *data=chosen?OSDynamicCast(OSData,chosen->getProperty("giulia-gpu-power")):NULL;
    firmwarePowerState=ggph_decode(data?data->getBytesNoCopy():NULL,
        data?data->getLength():0,&firmwarePower);
    if(chosen)chosen->release();
    setProperty("FirmwarePowerPrepared",firmwarePowerState==GGPH_PREPARED);
    setProperty("UEFIPowerReportVersion",1ULL,32);
    if(firmwarePowerState)setProperty("UEFIPowerReport",&firmwarePower,sizeof(firmwarePower));
    IOLog("GIULIA GPU: UEFI power handoff valid=%u prepared=%u code=%u phase=%u attempted=%#x confirmed=%#x valid_reads=%#x retained=%u flags=%#x native_status=%#llx protocol_status=%#llx runtime_lease=0 smmu_active=0 gmu_started=0 acceleration=0\n",
        firmwarePowerState!=GGPH_INVALID,firmwarePowerState==GGPH_PREPARED,
        firmwarePower.code,firmwarePower.phase,firmwarePower.attempted,firmwarePower.confirmed,
        firmwarePower.valid_reads,firmwarePower.retained,firmwarePower.flags,
        (unsigned long long)firmwarePower.firmware_status,(unsigned long long)firmwarePower.protocol_status);
}

void GiuliaGPU::observePower()
{
        struct ggp_report &report=powerObservation;
        struct ggp_io io={this,readRegister};
        ggp_observe(&io,&report);
        ++observations;
        setProperty("PowerObservation",&report,sizeof(report));
        setProperty("PowerObservationVersion",3ULL,32);
        setProperty("PowerObservationCount",(unsigned long long)observations,32);
        IOLog("GIULIA GPU: subsystem=power-observation stage=%u valid_fields=%#x hardware_ready=0\n",report.stage,report.valid_fields);
        const char *const names[13]={"gcc_cfg_ahb","gcc_memnoc","gcc_dvm","cx_gdscr","cx_hw_ctrl","gpu_ahb",
            "smmu_vote_clock","gmu_clock","smmu_scr0","smmu_id0","smmu_id1","smmu_id2","smmu_fault"};
        const uint64_t addresses[13]={0x171004,0x171010,0x171018,0x3d99108,0x3d99168,0x3d9911c,
            0x3d97000,0x3d9913c,0x3da0000,0x3da0020,0x3da0024,0x3da0028,0x3da0048};
        const uint32_t values[13]={report.cfg,report.memnoc,report.dvm,report.cx,report.cx_hw,report.ahb,
            report.smmu_clock,report.gmu_clock,report.scr0,report.id0,report.id1,report.id2,report.fault};
        for(unsigned i=0;i<13;++i) {
            if(report.valid_fields&(1u<<i)) IOLog("GIULIA GPU: subsystem=power-observation register=%s address=%#llx observed=%#x valid=1\n",names[i],(unsigned long long)addresses[i],values[i]);
            else IOLog("GIULIA GPU: subsystem=power-observation register=%s address=%#llx observed=unavailable valid=0\n",names[i],(unsigned long long)addresses[i]);
        }
        if (report.compared) {
            const uint64_t bases[GGP_WINDOWS]={0x171000,0x3d90000,0x3da0000};
            IOLog("GIULIA GPU: subsystem=power-observation result=blocked stage=%u address=%#llx expected=%#x observed=%#x mask=%#x later_windows_skipped=1 cause=unconfirmed\n",
                report.stage,(unsigned long long)(bases[report.window]+report.offset),report.expected,report.observed,report.mask);
        } else if (report.stage==GGP_IO_ERROR) {
            IOLog("GIULIA GPU: subsystem=power-observation result=fail stage=%u window=%u offset=%#x native_code=%d observed=%#x observed_valid=%u cause=unconfirmed\n",
                report.stage,report.window,report.offset,report.io_error,report.observed,report.io_error==0);
        }
}

void GiuliaGPU::reportStatus()
{
    /* The target IOLog formatter has a 256-byte buffer. Keep each record
     * independently bounded so a truncated line cannot erase readiness. */
    IOLog("GIULIA GPU: command=gpu.status stage=%s mode=%u firmware_verified=%u host_prepared=%u reservations=%u table_pages=%u leaf_pages=%u\n",
        gg_terminal_stage_name(__atomic_load_n(&gpuTerminal.stage,__ATOMIC_ACQUIRE)),mode,
        firmwareVerified,hostPrepared,reservationCount,tablePages,leafPages);
    IOLog("GIULIA GPU: power_acquired=0 smmu_active=0 gmu_started=0 hardware_ready=0 acceleration=0 device_writes=0\n");
    /* Transactions are linked, not bound to an authoritative runtime-power,
     * context or fence backend. Do not report them as active hardware. */
    IOLog("GIULIA GPU: runtime_lease_abi=1 native_submit_abi=1 runtime_provider_bound=0 submit_provider_bound=0\n");
    IOLog("GIULIA GPU: observations=%u observation_stage=%u valid_fields=%#x\n",
        observations,powerObservation.stage,powerObservation.valid_fields);
    IOLog("GIULIA GPU: host_cache_published=%u cpu_cache=normal-copyback gpu_memory_attribute=normal-nc runtime_cache_ownership=unassigned\n",hostPrepared);
    hfiAllocReport();
    stackReport();
    shareReport();
    IOLog("GIULIA GPU: firmware_power_valid=%u firmware_power_prepared=%u phase=%u code=%u attempted=%#x confirmed=%#x retained=%u flags=%#x qmp_available=%d\n",
        firmwarePowerState!=GGPH_INVALID,firmwarePowerState==GGPH_PREPARED,
        firmwarePower.phase,firmwarePower.code,firmwarePower.attempted,
        firmwarePower.confirmed,firmwarePower.retained,firmwarePower.flags,giulia_platform_qmp_available());
    if(firmwarePowerState!=GGPH_INVALID) {
        IOLog("GIULIA GPU: firmware_failure phase=%u op=%u resource=%u last_read=%u failed_read=%u native_status=%#llx protocol_status=%#llx\n",
            firmwarePower.failure_phase,firmwarePower.failure_operation,firmwarePower.failure_resource,
            firmwarePower.last_read,firmwarePower.failed_read,
            (unsigned long long)firmwarePower.firmware_status,(unsigned long long)firmwarePower.protocol_status);
        IOLog("GIULIA GPU: firmware_comparison expected=%#llx expected_valid=%u observed=%#llx observed_valid=%u mask=%#llx mask_valid=%u\n",
            (unsigned long long)firmwarePower.expected,!!(firmwarePower.flags&GPP_EXPECTED_VALID),
            (unsigned long long)firmwarePower.observed,!!(firmwarePower.flags&GPP_OBSERVED_VALID),
            (unsigned long long)firmwarePower.mask,!!(firmwarePower.flags&GPP_MASK_VALID));
        for(unsigned i=0;i<16;++i) {
            if(firmwarePower.valid_reads&(1u<<i))
                IOLog("GIULIA GPU: firmware_value index=%u observed=%#x valid=1\n",i,firmwarePower.values[i]);
        }
    }
}

static void gpuHfiBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); }
void GiuliaGPU::hfiCommand(unsigned requests)
{
    if(!hostPrepared || mode!=3)return;
    if(platformPoisoned || (fenceMapped && auditFenceMappings())) {
        IOLog("GIULIA GPU HFI: rejected reason=platform-quarantined\n");return;
    }
    if(requests&GGT_HFI_PIN) {
        /* Explicit manual hardware experiment: pin this retained service and
         * all its RAM until reboot. stop cannot free tables behind raw MMIO.
         * A native runtime adapter will replace this with confirmed teardown. */
        if(!manualMemoryPinned){manualMemoryPinned=true;retain();}
        IOLog("GIULIA GPU HFI: memory_pinned_until_reboot=1 runtime_lease=0\n");
    }
    struct ggh_io io={(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy(),GGH_BYTES,GGH_IOVA,NULL,gpuHfiBarrier};
    if(requests&GGT_HFI_STATUS) {
        IOLog("GIULIA GPU HFI: table_iova=%#x log_iova=%#x vrb_iova=%#x cpu_cache=normal-nc pinned=%u\n",GGH_IOVA,GGH_LOG_IOVA,GGH_VRB_IOVA,manualMemoryPinned);
        for(unsigned q=0;q<GGH_QUEUES;++q) {
            volatile uint32_t *h,*ring;unsigned r=0,w=0;
            int e=ggh_queue(&io,q,&h,&ring,&r,&w);
            IOLog("GIULIA GPU HFI: queue=%u code=%d read=%u write=%u\n",q,e,r,w);
        }
    }
    if((requests&(GGT_HFI_QUERY|GGT_HFI_POLL)) && !manualMemoryPinned) {
        IOLog("GIULIA GPU HFI: rejected reason=pin-memory-before-hardware-use\n");return;
    }
    if(requests&GGT_HFI_QUERY) {
        hfiSequence=(hfiSequence+1)&4095;if(!hfiSequence)hfiSequence=1;
        uint32_t msg[3]={(hfiSequence<<20)|(3u<<8)|12u,125u,0u};
        int e=ggh_send(&io,msg,3);
        if(!e)hfiLastHeader=msg[0];
        IOLog("GIULIA GPU HFI: query code=%d header=%#x property=125 doorbell_pending=%u\n",e,msg[0],!e);
    }
    if(requests&GGT_HFI_POLL) {
        uint32_t msg[256];unsigned n=0;
        for(unsigned q=1;q<=2;++q)for(unsigned packet=0;packet<8;++packet) {
            int e=ggh_receive(&io,q,msg,256,&n);
            if(e){IOLog("GIULIA GPU HFI: poll queue=%u code=%d\n",q,e);break;}
            IOLog("GIULIA GPU HFI: received queue=%u words=%u header=%#x\n",q,n,msg[0]);
            for(unsigned i=0;i<n;++i)IOLog("GIULIA GPU HFI: word=%u value=%#x\n",i,msg[i]);
            if(q==1 && n>=3 && (msg[0]&255)==12 && ((msg[0]>>16)&15)==1 && hfiLastHeader &&
               (msg[1]&~(1u<<19))==hfiLastHeader) {
                /* HFI v5+ reports an independent status in word 3. A
                 * plausible value alone cannot prove command success.
                 * Legacy replies remain receipt-only until version validation. */
                IOLog("GIULIA GPU HFI: query_ack=1 header=%#x value=%#x status_present=%u status=%#x accepted=%u\n",
                    msg[1],msg[2],n>=4,n>=4?msg[3]:0xffffffffu,
                    n>=4 && msg[3]==0 && msg[2]!=0xffffffffu);
                hfiLastHeader=0;
            }
        }
    }
    if(requests&GGT_HFI_LOG) {
        volatile uint32_t *p=(volatile uint32_t *)allocations[hfiIndex+1]->getBytesNoCopy();
        gpuHfiBarrier(NULL);
        for(unsigned i=0;i<64;++i)IOLog("GIULIA GPU HFI: log_word=%u value=%#x\n",i,p[i]);
    }
}

#include "giulia_gpu_dev_impl.h"
#include "giulia_gpu_platform_impl.h"
#include "giulia_gpu_mapping_impl.h"
#include "giulia_gpu_fence_impl.h"
#include "giulia_gpu_gunyah_impl.h"
#include "giulia_gpu_gmu_impl.h"
#include "giulia_gpu_hfi_alloc_impl.h"
#include "giulia_gpu_stack_impl.h"
#include "giulia_gpu_share_impl.h"
void GiuliaGPU::worker(void *cookie,wait_result_t)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    IOLockLock(self->runtimeLock);
    self->gunyahWorker=current_thread();
    if(!self->stopping) {
        __atomic_store_n(&gpuTerminal.stage,(unsigned)GGT_READY,__ATOMIC_RELEASE);
        if(!self->hostOnly && (self->mode==2 || (self->mode==3 && self->firmwarePowerState==GGPH_PREPARED)))self->observePower();
    }
    while(!self->stopping) {
        if(__atomic_load_n(&devState,__ATOMIC_ACQUIRE)==2) {
            uint32_t args[6];memcpy(args,devArgs,sizeof(args));
            if(self->hostOnly)
                IOLog("GIULIA GPU: hostonly=1 rejected_dev_token=%u device_dispatch=0\n",args[0]);
            else self->devCommand(args);
            __atomic_store_n(&devState,0u,__ATOMIC_RELEASE);
        }
        unsigned requests=gg_terminal_take(&gpuTerminal);
        if(self->hostOnly) {
            unsigned rejected=requests&~(GGT_STATUS|GGT_STACK);
            if(rejected)IOLog("GIULIA GPU: hostonly=1 rejected_requests=%#x device_dispatch=0\n",rejected);
            requests&=GGT_STATUS|GGT_STACK;
        }
        if(requests&GGT_GMU_LOAD)self->gmuCommand(1);
        if(requests&GGT_GMU_START)self->gmuCommand(2);
        if(requests&GGT_GMU_STATUS)self->gmuCommand(0);
        if(requests&GGT_STACK)self->stackPlan();
        if(requests&GGT_SHARE)self->shareCommand();
        if(!self->hostOnly && (requests&(GGT_COMMANDS&~(GGT_STATUS|GGT_OBSERVE|GGT_GMU_COMMANDS))))self->hfiCommand(requests);
        if(requests&GGT_OBSERVE) {
            IOLog("GIULIA GPU: command=gpu.observe phase=begin device_writes=0\n");
            self->observePower();
            IOLog("GIULIA GPU: command=gpu.observe phase=end observation=%u stage=%u valid_fields=%#x hardware_ready=0\n",
                self->observations,self->powerObservation.stage,self->powerObservation.valid_fields);
        }
        if(requests&(GGT_STATUS|GGT_OBSERVE))self->reportStatus();
        if(!self->hostOnly)self->gunyahPeriodic();
        /* Answer firmware memory requests only while pinned and healthy. The
         * pump is bounded and never retries a refused or unsent reply. */
        if(!self->hostOnly)self->hfiPumpOnce(2);
        IOLockUnlock(self->runtimeLock);IOSleep(25);IOLockLock(self->runtimeLock);
    }
    self->workerRunning=false;
    IOLockWakeup(self->runtimeLock,&self->workerRunning,false);
    IOLockUnlock(self->runtimeLock);
    self->release();thread_terminate(current_thread());
}

void GiuliaGPU::closeTerminal(unsigned stage)
{
    if(!terminalOwner)return;
    while(__atomic_load_n(&devState,__ATOMIC_ACQUIRE)==1) {}
    __atomic_store_n(&devState,4u,__ATOMIC_RELEASE);
    unsigned cancelled=gg_terminal_close(&gpuTerminal);
    if(cancelled)IOLog("GIULIA GPU: terminal requests_cancelled=%#x stage=%s device_writes=0\n",cancelled,gg_terminal_stage_name(stage));
    __atomic_store_n(&gpuTerminal.stage,stage,__ATOMIC_RELEASE);
}

void GiuliaGPU::releaseResources()
{
    if(manualMemoryPinned) {
        IOLog("GIULIA GPU: manual hardware memory retained until reset; teardown not assumed\n");return;
    }
    for (unsigned i=0;i<GGP_WINDOWS;++i) {
        OSSafeReleaseNULL(registerMaps[i]); OSSafeReleaseNULL(registerMemory[i]);
    }
    OSSafeReleaseNULL(gmuMap); OSSafeReleaseNULL(gmuMemory);
    for (unsigned i=0;i<GG_FW_MAX_BLOCKS;++i) OSSafeReleaseNULL(allocations[i]);
    for(unsigned i=0;i<GGD_CHUNKS;i++)OSSafeReleaseNULL(devMemory[i]);
    /* manualMemoryPinned retains every resource before an ownership/PAS call.
     * No ambiguous secure page is ever returned to the ordinary allocator. */
    if(secureData)for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++)OSSafeReleaseNULL(secureData[i]);
    OSSafeReleaseNULL(secureTables);OSSafeReleaseNULL(scmMemory);OSSafeReleaseNULL(zapMetadata);
    OSSafeReleaseNULL(zapMap);OSSafeReleaseNULL(zapMemory);
    for(unsigned i=0;i<4;i++){OSSafeReleaseNULL(platformMaps[i]);OSSafeReleaseNULL(platformMemory[i]);}
    OSSafeReleaseNULL(gpuTableMemory);
    OSSafeReleaseNULL(memstoreMemory);
    OSSafeReleaseNULL(tableMemory);
    OSSafeReleaseNULL(firmwareMap); OSSafeReleaseNULL(firmwareMemory);
    /* A prepared transaction pinned the control page and its callbacks:
     * retain it until reset instead of freeing behind the share state. */
    if(!shareRuntime.prepared)OSSafeReleaseNULL(shareControl);
}
void GiuliaGPU::stop(IOService *provider)
{
    stopGunyah(); /* Latch before taking a lock held by the owning worker. */
    IOLockLock(runtimeLock);stopping=true;closeTerminal(GGT_STOPPING);
    while(workerRunning)IOLockSleep(runtimeLock,&workerRunning,THREAD_UNINT);
    /* The bump pool never frees: a quarantined pump keeps its allocations and
     * every mapped page table entry until reboot. Report, never "reclaim". */
    if(hfiAllocReady)
        IOLog("GIULIA GPU HFI ALLOC: stop quarantine=%u allocations=%u published=%u retained_until_reboot=1\n",
            hfiPump.quarantined,hfiAllocPool.pool?hfiAllocPool.pool->count:0,hfiAllocPool.published);
    /* Read-only stages only: unwinding them confirms nothing about hardware. */
    if(stackReady)(void)ggsk_retreat(&stackState);
    releaseResources();
    if(terminalOwner) {
        __atomic_store_n(&gpuTerminal.stage,(unsigned)GGT_STOPPED,__ATOMIC_RELEASE);
        gg_terminal_finish(&gpuTerminal);terminalOwner=false;
    }
    IOLockUnlock(runtimeLock);IOService::stop(provider);
}
void GiuliaGPU::free()
{
    stopGunyah();
    /* A running worker holds a retain. stop has joined it before releasing
     * resources; failed starts never leave a worker or an open mailbox. */
    releaseResources();
    releaseSecureMetadata();
    if(terminalOwner){gg_terminal_finish(&gpuTerminal);terminalOwner=false;}
    if(runtimeLock){IOLockFree(runtimeLock);runtimeLock=NULL;}
    IOService::free();
}
#endif
