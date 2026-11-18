"""Exercise actual platform methods with owned-memory/SCM doubles; no device."""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'ios16/gpu/GiuliaGPU.cpp').read_text()
members=source[source.index('    IOBufferMemoryDescriptor **secureData'):source.index('    bool prepareDevMemory')]
# The mapping fixture owns ordinary RAM records; allocation/failure/retention
# of the actual service's heap records is covered by ios16_gpu_service_layout.
members=members.replace('**secureData;', '**secureData=fixtureSecureData;')
members=members.replace('*securePA,', '*securePA=fixtureSecurePA,')
fixture=r'''
#define GG_HOST_TEST 1
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include "giulia_gpu_pack.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_dev.h"
#include "giulia_gpu_platform.h"
#include "giulia_gpu_secure_mapping.h"
#include "giulia_gpu_cache.h"
#include "giulia_gpu_power.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"
using IOByteCount=uint64_t;
enum { kIOMemoryMapperNone=0x800,kIODirectionInOut=3,kIOMapInhibitCache=0x100,kIOMapWriteCombineCache=0x500 };
static unsigned mappedLogs,scmCalls;
static unsigned publishCalls,publishFail,publishBusyAt;
static volatile uint32_t *publishBusyRegister;
static std::vector<unsigned> publishedValid;
static bool powerAvailable=true;
static void IOLog(const char *format,...){if(strstr(format,"owned=1 mapped=1"))++mappedLogs;}
static void IOSleep(unsigned){}
static void gpuHfiBarrier(void *){__sync_synchronize();}
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
extern "C" int giulia_gpu_cache_publish(const void *cpu,size_t bytes) {
    assert(cpu && bytes==GGS_SEC_TABLE_BYTES);++publishCalls;
    const uint64_t *table=(const uint64_t *)cpu;unsigned valid=0;
    for(size_t i=0;i<bytes/8;i++)if((table[i]&3)==3 && (table[i]&(3ull<<53)))++valid;
    publishedValid.push_back(valid);
    if(publishCalls==publishBusyAt){assert(publishBusyRegister);*publishBusyRegister=1;}
    return publishCalls!=publishFail;
}
struct IOMemoryMap {uintptr_t address;uintptr_t getVirtualAddress(){return address;}};
struct IOMemoryDescriptor {
    uint64_t pa;size_t bytes;unsigned char *cpu;bool cp_only=false;
    IOMemoryMap mapping;
    IOMemoryDescriptor(uint64_t a,size_t n):pa(a),bytes(n) {
        assert(!posix_memalign((void **)&cpu,16384,(n+16383)&~16383ul));memset(cpu,0,n);mapping.address=(uintptr_t)cpu;
    }
    virtual ~IOMemoryDescriptor(){free(cpu);}
    IOMemoryMap *map(unsigned){return &mapping;}
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *span,unsigned options) {
        assert(options==kIOMemoryMapperNone && offset<bytes);if(span)*span=bytes-offset;return pa+offset;
    }
    void *getBytesNoCopy(){assert(!cp_only && "CPU accessed a CP_PIXEL-only buffer");return cpu;}
    size_t getLength(){return bytes;}
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t,size_t,unsigned);
};
struct IOBufferMemoryDescriptor: IOMemoryDescriptor {using IOMemoryDescriptor::IOMemoryDescriptor;};
static std::vector<std::unique_ptr<IOMemoryDescriptor>> memories;
IOMemoryDescriptor *IOMemoryDescriptor::withPhysicalAddress(uint64_t pa,size_t n,unsigned) {
    auto *p=new IOMemoryDescriptor(pa,n);memories.emplace_back(p);return p;
}
static uint64_t nextPA=0x880000000ull;
static IOBufferMemoryDescriptor *gpuAllocateHostMemory(size_t n,bool=true) {
    auto *p=new IOBufferMemoryDescriptor(nextPA,n);nextPA+=((n+0xffff)&~0xffffull)+0x4000;memories.emplace_back(p);return p;
}
static bool gpuHostRangeValid(void *p,uint64_t pa,uint64_t span,size_t n)
{return p && !(pa&16383) && span>=n && pa+n<(1ull<<36);}
static IOMemoryDescriptor *find(uint64_t pa) {
    for(auto &p:memories)if(pa>=p->pa && pa-p->pa<p->bytes)return p.get();assert(false);return nullptr;
}
static void *pointer(uint64_t pa) {auto *p=find(pa);return (unsigned char *)p->getBytesNoCopy()+pa-p->pa;}
static unsigned assignments,failAssignment;
static int ggs_aarch64_call(void *,struct ggs_regs *r) {
    ++scmCalls;
    uint64_t fn=r->x[0];
    if(fn==0xc2000601 || fn==0x42000601){r->x[0]=0;r->x[1]=1;return 0;}
    if(fn==0x42000c16) {
        assert(r->x[1]==0x1117 && (r->x[3]==16 || r->x[3]==512));
        auto *regions=(ggs_mem_region *)pointer(r->x[2]);
        assert(*(uint32_t *)pointer(r->x[4])==3);
        auto *ext=(uint64_t *)pointer(r->x[5]);assert(ext[0]==4 && !ext[3]);
        auto *dest=(ggs_vmperm *)pointer(ext[1]);
        if(ext[2]==48){assert(dest[0].vmid==3 && dest[0].perm==6 && dest[1].vmid==10 && dest[1].perm==4);}
        for(unsigned i=0;i<r->x[3]/16;i++) {
            auto region=regions[i];auto *memory=find(region.address);assert(region.bytes<=memory->bytes-(region.address-memory->pa));
            if(ext[2]==24) {assert(dest[0].vmid==10 && dest[0].perm==6);memory->cp_only=true;}
        }
        ++assignments;r->x[0]=0;r->x[1]=assignments==failAssignment?1:0;return 0;
    }
    assert(fn==0x42002801 || fn==0x42000c1b);r->x[0]=r->x[1]=0;return 0;
}
class GiuliaGPU {
public:
    IOBufferMemoryDescriptor *fixtureSecureData[GGS_SEC_SEGMENTS]={};
    uint64_t fixtureSecurePA[GGS_SEC_SEGMENTS]={};
'''+members+r'''
    bool manualMemoryPinned=true;unsigned mode=3;uint64_t gpuRoot=0x870000000ull;
    IOBufferMemoryDescriptor *firmwareMemory=nullptr,*allocations[8]={};IOMemoryMap *firmwareMap=nullptr;
    unsigned hfiIndex=0;uint32_t devPacket[255]={};bool devWritten[255]={};
    static int readRegister(void *,unsigned window,uint32_t offset,uint32_t *v) {
        *v=0;
        if(window==GGP_GCC)*v=1;
        if(window==GGP_GPUCC && offset==0x9168)*v=0x80000000;
        if(window==GGP_GPUCC && (offset==0x7000 || offset==0x913c))*v=1;
        if(window==GGP_SMMU && offset==0x20)*v=powerAvailable?0x4c017e18:0xffffffff;
        return 0;
    }
};
'''+(ROOT/'ios16/gpu/giulia_gpu_platform_impl.h').read_text()+r'''
int GiuliaGPU::configureDevMappings(){return 8;} // Separate actual mapping-method suite.
static void idle(GiuliaGPU &s) {
    s.platformWrite(0x3d7e000,1);
    for(unsigned i=0;i<24;i++)s.platformWrite(0x3da0c00+4*i,0x20000);
}
static void context(GiuliaGPU &s,uint64_t root,unsigned bank,unsigned slot,uint32_t sid,uint32_t asid) {
    // Captured readback values, independent of the production validator.
    const uint64_t base=0x3dc0000+(uint64_t)bank*4096;
    const struct {unsigned offset;uint32_t value;} registers[]={
        {0,0x27},{4,0x32b},{0x10,0x38071},{0x30,0x80802020},
        {0x20,(uint32_t)root},{0x24,(uint32_t)(root>>32)|(asid<<16)},
        {0x28,0},{0x2c,asid<<16},{0x38,0xff44},{0x3c,0},{0x58,0},{0x7f4,0}};
    for(auto r:registers)s.platformWrite(base+r.offset,r.value);
    s.platformWrite(0x3da0048,0);s.platformWrite(0x3da1800+4*bank,1);s.platformWrite(0x3da1000+4*bank,0x1f300);
    s.platformWrite(0x3da0800+4*slot,0x80000000u|sid);s.platformWrite(0x3da0c00+4*slot,bank);
}
static void context_refusals(GiuliaGPU &s,bool secure) {
    const unsigned bank=secure?3:0,slot=secure?2:1;
    const uint64_t base=0x3dc0000+(uint64_t)bank*4096;
    const struct {uint64_t address;uint32_t bits;} corruptions[]={
        {0x3da0048,1},{0x3da1800+4*bank,1},{0x3da1000+4*bank,1},
        {base,1},{base+4,1},{base+0x10,1},{base+0x30,1},
        {base+0x20,4096},{base+0x24,1},{base+0x24,0x10000},
        {base+0x28,4096},{base+0x2c,0x10000},{base+0x38,1},{base+0x3c,1},
        {base+0x58,2},{base+0x58,0xffffffff},{base+0x7f4,1},
        {0x3da0800+4*slot,1},{0x3da0800+4*slot,0x10000},
        {0x3da0800+4*slot,0x80000000},{0x3da0c00+4*slot,1}};
    const unsigned calls=scmCalls,logs=mappedLogs;
    for(auto c:corruptions) {
        uint32_t saved=s.platformRead(c.address);s.platformWrite(c.address,saved^c.bits);
        assert((secure?s.secureAllocate(4096,4096,0x1007,5):s.platformCommand(3))==8);
        assert(scmCalls==calls && mappedLogs==logs);
        for(auto &allocation:s.secureSlots)assert(!allocation.used);
        s.platformWrite(c.address,saved);
    }
    uint64_t &root=secure?s.secureRoot:s.gpuRoot,saved=root;
    for(uint64_t invalid:{uint64_t(0),saved|1,uint64_t(1)<<36}) {
        root=invalid;
        assert((secure?s.secureAllocate(4096,4096,0x1007,5):s.platformCommand(3))==8);
        assert(scmCalls==calls && mappedLogs==logs);
    }
    root=saved;
}
static void pagewalk(GiuliaGPU &s) {
    auto *tables=(uint64_t *)s.secureTables->getBytesNoCopy();
    for(unsigned segment=0;segment<GGS_SEC_SEGMENTS;segment++)for(unsigned offset=0;offset<GGS_SEC_GRANULE;offset+=4096) {
        uint64_t va=GGS_SEC_BASE+(uint64_t)segment*GGS_SEC_GRANULE+offset;size_t at=0;
        for(unsigned level=1;level<3;level++) {uint64_t e=tables[at*512+((va>>(39-9*level))&511)];assert((e&3)==3);at=((e&0x0000fffffffff000ull)-s.secureRoot)/4096;assert(at<GGS_SEC_TABLE_BYTES/4096);}
        uint64_t leaf=tables[at*512+((va>>12)&511)];
        uint64_t relative=va-GGS_SEC_BASE;
        const auto &slot=s.secureSlots[relative/GGS_SEC_CHUNK];
        unsigned mapped=slot.used?(slot.bytes+4095u)&~4095u:0;
        uint64_t expected=s.securePA[relative/GGS_SEC_GRANULE]+relative%GGS_SEC_GRANULE;
        if(relative%GGS_SEC_CHUNK<mapped)expected|=(3ull<<53)|0xe03;
        assert(leaf==expected); // unused slots and allocation tails are invalid tombstones
    }
}
static uint64_t *leaf(GiuliaGPU &s,unsigned offset) {
    auto *tables=(uint64_t *)s.secureTables->getBytesNoCopy();
    uint64_t va=GGS_SEC_BASE+(uint64_t)offset;size_t at=0;
    for(unsigned level=1;level<3;level++) {
        uint64_t entry=tables[at*512+((va>>(39-9*level))&511)];assert((entry&3)==3);
        at=((entry&0x0000fffffffff000ull)-s.secureRoot)/4096;
    }
    return &tables[at*512+((va>>12)&511)];
}
static void ready(GiuliaGPU &s) {
    assert(s.platformPrepare());idle(s);pagewalk(s);
    assert(s.platformCommand(2)==0 && s.platformCommand(4)==0);
    s.secureContextActive=true;context(s,s.secureRoot,3,2,2,0x753);
}
static void mapping_failures() {
    // Complete-pool preflight rejects corrupt backing/extra aliases before any
    // slot or valid leaf is consumed. It cannot be repaired by the allocator.
    for(unsigned fault=0;fault<9;fault++) {
        GiuliaGPU bad={};ready(bad);
        auto *tables=(uint64_t *)bad.secureTables->getBytesNoCopy();
        unsigned logs=mappedLogs;
        if(fault==0)*leaf(bad,GGS_SEC_CHUNK-4096)^=4096;
        if(fault==1)*leaf(bad,7*GGS_SEC_CHUNK)|=(3ull<<53)|0xe03;
        if(fault==2)tables[2]=tables[3];
        if(fault==3)bad.securePA[511]=bad.securePA[0];
        if(fault==4)bad.securePA[511]=bad.securePA[0]+16384;
        if(fault==5)bad.securePA[511]=bad.secureRoot;
        if(fault==6)bad.secureData[511]=bad.secureData[0];
        if(fault==7)bad.secureData[511]->bytes-=4096;
        if(fault==8) {
            // Corrupt both the recorded PA and matching PTEs: only comparing
            // table leaves to securePA would falsely accept unowned memory.
            bad.securePA[511]+=GGS_SEC_GRANULE;
            for(unsigned i=0;i<GGS_SEC_GRANULE;i+=4096)
                *leaf(bad,511*GGS_SEC_GRANULE+i)=bad.securePA[511]+i;
        }
        std::vector<uint64_t> snapshot(tables,tables+GGS_SEC_TABLE_BYTES/8);
        assert(bad.secureAllocate(4097,4096,0x1007,5)==8 && bad.platformPoisoned);
        assert(mappedLogs==logs && !bad.secureSlots[0].used);
        assert(!memcmp(tables,snapshot.data(),GGS_SEC_TABLE_BYTES));
        memories.clear();
    }
    const ggs_secure_slot malformed[]={
        {4096,0x1007,5,2},{0,0x1007,5,1},{GGS_SEC_CHUNK+1,0x1007,5,1},
        {4096,0x1107,5,1},{4096,0x1007,4,1},
        {4096,0,0,0},{0,0x1007,0,0},{0,0,5,0}};
    for(auto slot:malformed) {
        GiuliaGPU bad={};ready(bad);bad.secureSlots[7]=slot;
        auto *tables=(uint64_t *)bad.secureTables->getBytesNoCopy();
        std::vector<uint64_t> snapshot(tables,tables+GGS_SEC_TABLE_BYTES/8);
        unsigned logs=mappedLogs,published=publishCalls;
        assert(bad.secureAllocate(4097,4096,0x1007,5)==8 && bad.platformPoisoned);
        assert(!bad.secureSlots[0].used && mappedLogs==logs && publishCalls==published);
        assert(!memcmp(tables,snapshot.data(),GGS_SEC_TABLE_BYTES));memories.clear();
    }
    // Failure of the initial audit/publication consumes no allocation. Both
    // publication and context invalidation errors quarantine the existing pool.
    for(unsigned failure=0;failure<2;failure++) {
        GiuliaGPU bad={};ready(bad);unsigned logs=mappedLogs,calls=scmCalls;
        publishedValid.clear();
        if(failure==0)publishFail=publishCalls+1;
        else {
            publishBusyAt=publishCalls+1;
            publishBusyRegister=(uint32_t *)(bad.platformMaps[3]->getVirtualAddress()+0x237f4);
        }
        assert(bad.secureAllocate(65537,4096,0x1007,5)==8 && bad.platformPoisoned);
        assert(!bad.secureSlots[0].used && publishedValid==std::vector<unsigned>({0}));
        assert(mappedLogs==logs && scmCalls==calls);pagewalk(bad);
        assert(bad.auditSecureMappings()==8);
        publishFail=publishBusyAt=0;publishBusyRegister=nullptr;memories.clear();
    }
    // Publication/TLB failure after APPLY keeps the reserved slot and backing,
    // emits no success, and blocks both subsequent allocation and audit.
    for(unsigned failure=0;failure<2;failure++) {
        GiuliaGPU bad={};ready(bad);unsigned logs=mappedLogs,calls=scmCalls;
        publishedValid.clear();
        if(failure==0)publishFail=publishCalls+2;
        else {
            publishBusyAt=publishCalls+2;
            publishBusyRegister=(uint32_t *)(bad.platformMaps[3]->getVirtualAddress()+0x237f4);
        }
        assert(bad.secureAllocate(65537,4096,0x1007,5)==8 && bad.platformPoisoned);
        assert(bad.secureSlots[0].used && bad.secureSlots[0].bytes==65537);
        assert(publishedValid==std::vector<unsigned>({0,17}));pagewalk(bad);
        assert(mappedLogs==logs && scmCalls==calls && bad.secureOwnershipReady);
        for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++)assert(bad.secureData[i]->cp_only);
        assert(bad.secureAllocate(4096,4096,0x1007,5)==8 && !bad.secureSlots[1].used);
        assert(bad.auditSecureMappings()==8);
        publishFail=publishBusyAt=0;publishBusyRegister=nullptr;memories.clear();
    }
    // Already published secure allocations are checked exactly on each audit.
    GiuliaGPU bad={};ready(bad);assert(bad.secureAllocate(1,4096,0x1007,5)==0);pagewalk(bad);
    *leaf(bad,4096)|=(3ull<<53)|0xe03;
    assert(bad.auditSecureMappings()==8 && bad.platformPoisoned && bad.secureSlots[0].used);
    memories.clear();
}
static void inactive_refusals() {
    const struct {uint64_t address;uint32_t value;} corruptions[]={
        {0x3dc3000,2},{0x3dc3000,1},{0x3da0048,1},{0x3da0808,0x80000002},
        {0x3da0c08,3},{0x3dc37f4,1}};
    for(auto c:corruptions) {
        GiuliaGPU bad={};assert(bad.platformPrepare());idle(bad);
        bad.platformWrite(c.address,c.value);unsigned published=publishCalls,calls=scmCalls;
        assert(bad.auditSecureMappings()==8 && bad.platformPoisoned);
        assert(publishCalls==published && scmCalls==calls && !bad.platformOwned);pagewalk(bad);
        memories.clear();
    }
}
int main() {
    GiuliaGPU good={};assert(good.platformPrepare());idle(good);pagewalk(good);
    assert(good.platformCommand(1)==0 && good.platformCommand(2)==0);
    assert(good.platformCommand(4)==0 && good.secureOwnershipReady && assignments==17);
    for(unsigned i=0;i<GGS_SEC_SEGMENTS;i++)assert(good.secureData[i]->cp_only);
    assert(!good.secureTables->cp_only && !good.scmMemory->cp_only);
    assert(good.secureAllocate(4096,4096,0x1007,5)==8); /* No mapping success yet. */
    good.secureContextActive=true;good.platformWrite(0x3da0c08,3);
    assert(good.secureAllocate(4096,4096,0x1007,5)==8); /* A software flag/route alone is insufficient. */
    context(good,good.gpuRoot,0,1,0,0x752);context(good,good.secureRoot,3,2,2,0x753);
    context_refusals(good,false);context_refusals(good,true);
    unsigned before=scmCalls;assert(good.platformCommand(3)==0 && scmCalls==before+2);
    powerAvailable=false;assert(good.secureAllocate(4096,4096,0x1007,5)==8);powerAvailable=true;
    assert(mappedLogs==0);
    const uint32_t sizes[]={1,4095,4096,4097,65537,0x37d000,0x3fffff,0x400000};
    for(unsigned i=0;i<GGS_SEC_CHUNKS;i++) {
        assert(good.secureAllocate(sizes[i],4096,0x1007,5)==0);pagewalk(good);
        assert(good.auditSecureMappings()==0);
    }
    assert(mappedLogs==GGS_SEC_CHUNKS);
    assert(good.secureAllocate(4096,4096,0x1007,5)==3);
    assert(good.secureAllocate(4096,4096,0x1107,5)==4);
    memories.clear();
    GiuliaGPU bad={};assert(bad.platformPrepare());idle(bad);
    assert(bad.platformCommand(2)==0);failAssignment=assignments+3;
    assert(bad.platformCommand(4)!=0 && bad.platformPoisoned && bad.platformOwned && !bad.secureOwnershipReady);
    unsigned old=assignments;assert(bad.platformCommand(4)!=0 && assignments==old);
    assert(bad.secureAllocate(4096,4096,0x1007,5)!=0);
    memories.clear();failAssignment=0;mapping_failures();inactive_refusals();
    puts("Actual GPU platform methods: 8192-page independent walks, invalid unused slots/tails, exact rounded bounds, CPU publication/TLBI, denied CPU aliases, corrupt-PTE/topology refusal and retained slots after mapping failure passed");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-platform-runtime-') as temporary:
    p=Path(temporary);(p/'test.cpp').write_text(fixture)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-c',str(ROOT/'ios16/vendor/ArmSmmu4K/ArmSmmu4K.c'),'-o',str(p/'tables.o')],check=True)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-g',*include_flags(ROOT),str(p/'test.cpp'),str(p/'tables.o'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
