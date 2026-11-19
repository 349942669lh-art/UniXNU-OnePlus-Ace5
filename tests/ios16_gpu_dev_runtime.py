"""Run the production development commands and mapping transitions on host RAM.

The pack parser uses a synthetic fixture pin defined only in the generated host
translation unit. Production firmware authentication and device MMIO are not
replaced in target sources. ARM barriers become host barriers; no hardware DMA
or cache-coherence claim follows from this fixture.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "ios16/gpu/GiuliaGPU.cpp").read_text()
begin = source.index("static IOBufferMemoryDescriptor *gpuAllocateHostMemory(")
end = source.index("\nint GiuliaGPU::readRegister(", begin)

# Structurally valid, authenticated fixture assets exercise the actual parser.
assets = [struct.pack("<9I", 0x60000000, 0, 7, 4096, 0x60000004, 4, 0, 0, 0x12345678),
          bytes(range(256)) * 17, b"AQE fixture payload\0" * 17, b"ZAP fixture payload\0"]
pack = bytearray(4096)
pack[:8] = b"GGFWPK1\0"
struct.pack_into("<4I", pack, 8, 0x43051401, 4, 0, 1)
for index, data in enumerate(assets):
    struct.pack_into("<4I", pack, 64 + index * 64, index, len(pack), len(data), 0)
    pack[80 + index * 64:112 + index * 64] = hashlib.sha256(data).digest()
    pack.extend(data)
    pack.extend(bytes(-len(pack) % 4096))
struct.pack_into("<I", pack, 16, len(pack))
pin = ",".join(str(byte) for byte in hashlib.sha256(pack).digest())
fixture = f"""// Test-only trusted synthetic pack; never included in a kernel.
#define GIULIA_GPU_FIRMWARE_PIN_H
#define GG_FIRMWARE_PACK_BYTES {len(pack)}u
static const unsigned char gg_firmware_pack_sha256[32]={{{pin}}};
"""
fixture += r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_cache.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_memstore_mapping.h"
#include "giulia_gpu_ipc.h"
#include "giulia_gpu_ipcc_owner.h"
#include "giulia_gpu_fence_provider.h"
#include "giulia_gpu_fence_mapping.h"
#include "giulia_gpu_gmu_boot.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"
using IOByteCount=uint64_t;
// Enough disjoint simulated RAM for all scenarios, still below the 36-bit PA limit.
static unsigned long gPhysBase=0x880000000ULL,gPhysSize=0x700000000ULL;
static void *kernel_task=nullptr;
static struct ggi_owner gpuIpcOwner;
static bool qmpAvailable=true;
static int giulia_platform_qmp_available(){return qmpAvailable;}
enum { kIODirectionInOut=3,kIOMemoryHostPhysicallyContiguous=0x80,
    kIOMemoryMapperNone=0x800,kIOMapCopybackCache=0x400,kIOMapWriteCombineCache=0x500 };
static unsigned calls,allocated,freed,cleanCalls,cleanFail,changedAt,pollCalls,pollFail;
static unsigned char *watchByte;
static unsigned char watchOriginal;
static std::string logs;
static void IOLog(const char *format,...) {
    char line[1024];va_list args;va_start(args,format);
    vsnprintf(line,sizeof(line),format,args);va_end(args);logs+=line;
}
static void gpuHfiBarrier(void *) { __sync_synchronize(); }
static void *IOMalloc(size_t n){return malloc(n);}
static void IOFree(void *p,size_t){free(p);}
struct IOBufferMemoryDescriptor {
    void *cpu;size_t bytes;uint64_t pa;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,unsigned options,size_t n,size_t alignment) {
        assert(options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|kIOMapCopybackCache) ||
               options==(kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone|kIOMapWriteCombineCache));
        assert(n && !(n&0x3fff) && alignment==0x4000);
        auto *p=new IOBufferMemoryDescriptor;
        assert(!posix_memalign(&p->cpu,alignment,n));memset(p->cpu,0xa5,n);p->bytes=n;
        p->pa=gPhysBase+(++calls)*0x800000ULL;++allocated;return p;
    }
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *length,unsigned options) {
        assert(offset<bytes && options==kIOMemoryMapperNone);
        if(length)*length=bytes-offset;return pa+offset;
    }
    void *getBytesNoCopy(){return cpu;}
    size_t getLength(){return bytes;}
    void release(){free(cpu);++freed;delete this;}
};
extern "C" int giulia_gpu_cache_publish(const void *p,size_t n) {
    assert(p && !((uintptr_t)p&0x3fff) && n && !(n&0x3fff));++cleanCalls;
    if(watchByte && *watchByte!=watchOriginal && !changedAt)changedAt=cleanCalls;
    return cleanCalls!=cleanFail;
}
class GiuliaGPU {
public:
    IOBufferMemoryDescriptor *allocations[GG_FW_MAX_BLOCKS]={},*tableMemory=nullptr;
    IOBufferMemoryDescriptor *devMemory[GGD_CHUNKS]={},*gpuTableMemory=nullptr;
    unsigned reservationCount=0,tablePages=0,leafPages=0,hfiIndex=0;
    bool hostPrepared=false,manualMemoryPinned=true;
    uint64_t gmuRoot=0,gpuRoot=0;
    struct ggb_config gmuConfig;
    struct gg_reservation gmuReservations[GG_FW_MAX_BLOCKS];
    ArmSmmu4KResult gmuSmmuBuild;
    IOBufferMemoryDescriptor *memstoreMemory=nullptr;uint32_t memstoreFlags=0;bool memstoreMapped=false;
    bool ipcMapped=false;uint32_t ipcFlags=0;
    bool fenceMapped=false;uint32_t fenceFlags=0;const struct ggf_provider *fenceProvider=nullptr;
    int fenceAllocate(uint32_t,uint32_t,uint32_t,uint32_t);int fenceStatus();int auditFenceMappings();
    int ipcAllocate(uint32_t,uint32_t,uint32_t,uint32_t);int auditIpcMappings();
    bool prepareInitialMappings();int memstoreAllocate(uint32_t,uint32_t,uint32_t,uint32_t);
    int memstoreRead(uint32_t);int auditMemstoreMappings();int auditSecureMappings(){return 0;}
    struct ggd_pool devPool={};bool devUcode[GGD_MAX]={},devMappingsManaged=false;
    bool platformPrepared=true,platformPoisoned=false,rmOnlyTest=false;
    uint32_t devPacket[255]={};bool devWritten[255]={};
    IOBufferMemoryDescriptor *firmwareMemory=nullptr;
    struct Map {uintptr_t address;uintptr_t getVirtualAddress(){return address;}} packMap={};
    Map *firmwareMap=nullptr;
    std::map<uint64_t,uint32_t> registers;
    GiuliaGPU(){devPool.next=GGD_CACHE_BYTES;registers[0x3d7e340]=3;}
    ~GiuliaGPU(){for(auto *p:allocations)if(p)p->release();if(tableMemory)tableMemory->release();
        for(auto *p:devMemory)if(p)p->release();if(gpuTableMemory)gpuTableMemory->release();
        if(firmwareMemory)firmwareMemory->release();if(memstoreMemory)memstoreMemory->release();
        // End of an isolated host boot model. Production retains this until reset.
        if(ggi_owns(&gpuIpcOwner,GGI_GPU,this))gpuIpcOwner={};
        gpuFenceProvider={};}
    bool prepareDevMemory(ArmSmmu4KRange *,size_t *);bool prepareMemory(const gg_pack *);
    int configureDevMappings();int mapDevAllocation(unsigned,bool clearOnly=false);
    int syncDevMappings();int auditDevMappings();
    uint8_t *devPointer(uint32_t);void devAllocation(unsigned);void devCommand(const uint32_t *);
    void setProperty(const char *,unsigned long long,unsigned){}
    void setProperty(const char *,const char *){}
    bool platformPower(){return true;}
    bool platformIdle(){return !(registers[0x3dc0000]&1) && !(registers[0x3dc2000]&1);}
    uint32_t platformRead(uint64_t a){return registers[a];}
    void platformWrite(uint64_t a,uint32_t v){registers[a]=v;}
    bool platformPoll(uint64_t,uint32_t,uint32_t){return ++pollCalls!=pollFail;}
    int platformCommand(unsigned){return 6;}
    int gunyahCommand(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int gmuCommand(unsigned){return 1;} /* production adapter needs IOKit + SID5 */
    int secureAllocate(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int platformWatch(unsigned,unsigned){return 6;}
    int platformSnapshot(unsigned){return 6;}
    void active(){for(unsigned i=0;i<2;i++) {
        uint64_t b=i?0x3dc2000:0x3dc0000,r=i?gmuRoot:gpuRoot;
        unsigned bank=i?2:0,slot=i?0:1,sid=i?5:0,asid=i?0x750:0x752;
        registers[b]=0x27;registers[b+0x20]=(uint32_t)r;registers[b+0x24]=(uint32_t)(r>>32)|(asid<<16);
        registers[b+0x2c]=asid<<16;registers[b+0x10]=0x38071;registers[b+0x30]=0x80802020;
        registers[b+0x38]=0xff44;registers[0x3da1800+4*bank]=1;registers[0x3da1000+4*bank]=0x1f300;
        registers[0x3da0800+4*slot]=0x80000000u|sid;registers[0x3da0c00+4*slot]=bank;}}
};
'''
fixture += source[begin:end]
fixture += (ROOT / "ios16/gpu/giulia_gpu_dev_impl.h").read_text()
fixture += (ROOT / "ios16/gpu/giulia_gpu_mapping_impl.h").read_text()
fixture += (ROOT / "ios16/gpu/giulia_gpu_fence_impl.h").read_text()
provider_start=source.index('static struct ggf_registration gpuFenceProvider;')
# The platform broker owns IPC arbitration now, so the old
# giulia_gpu_ipcc_* wrappers are gone; the region ends at the next extern block.
provider_end=source.index('extern "C" int giulia_gpu_terminal(',provider_start)
fixture=fixture.replace('class GiuliaGPU {',source[provider_start:provider_end]+'class GiuliaGPU {',1)
fixture += r'''
static std::vector<uint8_t> fixturePack;
static gg_pack decoded;
static int command(GiuliaGPU &s,uint32_t op,uint32_t a=0,uint32_t b=0,uint32_t c=0,uint32_t d=0) {
    static unsigned token=1;uint32_t args[6]={token++,op,a,b,c,d};logs.clear();s.devCommand(args);
    auto done=logs.rfind("GIULIA GPU DEV: done token=");assert(done!=std::string::npos);
    unsigned actualToken,actualOp;int code=-1;
    assert(sscanf(logs.c_str()+done,"GIULIA GPU DEV: done token=%u op=%u code=%d",&actualToken,&actualOp,&code)==3);
    assert(actualToken==args[0] && actualOp==op);return code;
}
static void prepare(GiuliaGPU &s,bool managed) {
    s.firmwareMemory=new IOBufferMemoryDescriptor;
    s.firmwareMemory->cpu=malloc(fixturePack.size());assert(s.firmwareMemory->cpu);
    s.firmwareMemory->bytes=fixturePack.size();s.firmwareMemory->pa=0;++allocated;
    memcpy(s.firmwareMemory->cpu,fixturePack.data(),fixturePack.size());
    s.packMap.address=(uintptr_t)s.firmwareMemory->cpu;s.firmwareMap=&s.packMap;
    bool prepared=s.prepareMemory(&decoded);if(!prepared)fputs(logs.c_str(),stderr);
    assert(prepared);if(managed){assert(!s.configureDevMappings());s.active();}
}
static unsigned add(GiuliaGPU &s,uint32_t bytes=12288,uint32_t flags=0x301,uint32_t kind=0) {
    unsigned before=s.devPool.count;assert(!command(s,1,bytes,4096,flags,kind));
    assert(s.devPool.count==before+1);return before;
}
// Independent leaf walk: verify actual descriptors, not the permission helper.
static uint64_t leaf(IOBufferMemoryDescriptor *t,uint64_t va) {
    const uint64_t address=0x0000fffffffff000ULL;auto *words=(uint64_t *)t->cpu;size_t page=0;
    for(unsigned level=1;level<3;level++) {
        uint64_t e=words[page*512+((va>>(39-9*level))&(level==1?3:511))],pa=e&address;
        assert((e&~address)==3 && pa>=t->pa && pa-t->pa<t->bytes);
        page=(size_t)((pa-t->pa)/4096);
    }
    return words[page*512+((va>>12)&511)];
}
static void code_leaves(GiuliaGPU &s,unsigned index,bool mapped) {
    const auto &a=s.devPool.allocations[index];
    for(uint32_t off=a.offset;off<a.offset+a.bytes;off+=4096) {
        uint64_t pa=s.devMemory[off/GGD_CHUNK]->pa+off%GGD_CHUNK;
        assert(leaf(s.gpuTableMemory,GGD_BASE+off)==(pa|(mapped?0xec3:0)));
        assert(leaf(s.tableMemory,GGD_BASE+off)==pa);
        if(off<GGD_CACHE_BYTES)assert(leaf(s.tableMemory,GGD_CACHE+off)==pa);
    }
}
static std::vector<uint8_t> bytes(GiuliaGPU &s,uint32_t at,uint32_t n) {
    std::vector<uint8_t> result;for(uint32_t i=0;i<n;i++)result.push_back(*s.devPointer(at+i));return result;
}
static void seal(bool managed,unsigned asset,bool split,bool cached=false) {
    GiuliaGPU s;prepare(s,managed);
    if(split)s.devPool.next=GGD_CHUNK*2-4096;
    unsigned index=add(s,12288,cached?0x381:0x301);auto a=s.devPool.allocations[index];uint32_t target=a.offset+4092;
    assert(!command(s,3,a.offset,0x5a5a5a5a));
    assert(!command(s,3,a.offset+a.bytes-4,0x5a5a5a5a));
    if(managed) {watchByte=s.devPointer(a.offset);watchOriginal=0x5a;changedAt=0;}
    unsigned start=cleanCalls;assert(!command(s,7,asset,target));
    if(managed)assert(changedAt==start+5);watchByte=nullptr;
    assert(s.devUcode[index]);
    for(uint32_t i=0;i<a.bytes;i++) {
        uint8_t expected=i>=4092 && i-4092<decoded.asset[asset].size?decoded.asset[asset].data[i-4092]:0;
        assert(*s.devPointer(a.offset+i)==expected);
    }
    auto saved=bytes(s,a.offset,a.bytes);
    assert(command(s,3,target,0)==4 && command(s,4,a.offset,a.bytes)==4);
    assert(command(s,7,asset,target)==4 && command(s,7,GG_ASSET_GMU,target)==4);
    assert(bytes(s,a.offset,a.bytes)==saved && !s.platformPoisoned);
    // The pre-policy copy is already sealed and becomes executable at setup.
    if(!managed)assert(!s.configureDevMappings());code_leaves(s,index,true);
    assert(!command(s,2,target,2) && !command(s,9,target));
    assert(!command(s,16));
}
static void command_guards() {
    GiuliaGPU s;assert(command(s,3,GGD_CACHE_BYTES,1)==1);prepare(s,true);
    s.manualMemoryPinned=false;assert(command(s,3,GGD_CACHE_BYTES,1)==2);s.manualMemoryPinned=true;
    unsigned index=add(s,8192,0x33f);auto a=s.devPool.allocations[index];
    assert(!command(s,3,a.offset,0x12345678));assert(*((uint32_t *)s.devPointer(a.offset))==0x12345678);
    assert(!command(s,4,a.offset,4) && !*(uint32_t *)s.devPointer(a.offset));
    assert(command(s,3,a.offset+a.bytes,1)==4);
    assert(command(s,4,a.offset+a.bytes-4,8)==4);
    assert(command(s,7,GG_ASSET_SQE,a.offset+a.bytes-4)==4);
    assert(command(s,7,GG_ASSET_SQE,GGD_BYTES-4)==4);
    assert(command(s,7,GG_ASSETS,a.offset)==4);
    unsigned data=add(s,8192,0x33f,2);assert(command(s,7,GG_ASSET_SQE,s.devPool.allocations[data].offset)==4);
    unsigned nogpu=add(s,8192,0x338,0);assert(command(s,7,GG_ASSET_SQE,s.devPool.allocations[nogpu].offset)==4);
    assert(!command(s,5,0,2u<<8));assert(command(s,6,2)==5);assert(!command(s,5,1,0x89));
    assert(!command(s,6,2));auto *hfi=(uint32_t *)s.allocations[s.hfiIndex]->cpu;
    assert(hfi[GGH_HEADER_WORDS+11]==4 && hfi[GGH_RING_OFFSET/4+1]==0x89);
    assert(!s.devWritten[0] && !s.devWritten[1]);
    assert(command(s,5,255,0)==4 && command(s,6,256)==4);
    assert(!command(s,10));assert(command(s,22)==6);
    s.devPool.count=GGD_MAX+1;assert(command(s,8)==8);s.devPool.count=nogpu+1;
}
static void rm_dispatch() {
    GiuliaGPU s;prepare(s,true);s.rmOnlyTest=true;
    auto saved=s.registers;
    unsigned count=s.devPool.count;
    for(unsigned op=0;op<=24;op++) {
        if(op==22)continue;
        assert(command(s,op,0,4096,0,0)==4);
        assert(s.registers==saved && s.devPool.count==count);
    }
    for(unsigned action=0;action<=6;action++)
        assert(command(s,22,action,action==5?10:0,0,0)==((action==0 || action==5)?6:4));
}
static void quarantine(GiuliaGPU &s,uint32_t at) {
    auto saved=bytes(s,at,12288);auto count=s.devPool.count;
    auto *hfi=(uint8_t *)s.allocations[s.hfiIndex]->cpu;
    std::vector<uint8_t> hfiBefore(hfi,hfi+GGH_BYTES);
    uint32_t packet[255];bool written[255];memcpy(packet,s.devPacket,sizeof(packet));memcpy(written,s.devWritten,sizeof(written));
    for(unsigned op: {1u,3u,4u,5u,6u,7u,10u})assert(command(s,op,at,4,0x33f,0)==8);
    assert(s.devPool.count==count && bytes(s,at,12288)==saved);
    assert(!memcmp(hfi,hfiBefore.data(),GGH_BYTES));
    assert(!memcmp(packet,s.devPacket,sizeof(packet)) && !memcmp(written,s.devWritten,sizeof(written)));
    assert(!command(s,0) && !command(s,2,at,1) && !command(s,8) && !command(s,9,at));
}
static void failures() {
    for(unsigned phase=0;phase<8;phase++) {
        GiuliaGPU s;prepare(s,true);unsigned index=add(s);auto a=s.devPool.allocations[index];
        assert(!command(s,3,a.offset,0x5a5a5a5a));
        assert(!command(s,5,0,1u<<8));
        if(phase<3) {
            auto before=s.devPool.count;auto next=s.devPool.next;
            cleanFail=cleanCalls+1+2*phase;assert(command(s,1,4096,4096,0x33f,0)==8);
            assert(s.devPool.count==before+1 && s.devPool.next==next+4096);
        } else {
            // Failure before revocation leaves bytes untouched; failure after
            // revocation/copy preserves the sealed record and invalid leaves.
            cleanFail=cleanCalls+1+2*(phase-3);
            assert(command(s,7,GG_ASSET_SQE,a.offset+4092)==8);
            assert(s.devUcode[index]==(phase>=5));
            if(phase<5)assert(*(uint32_t *)s.devPointer(a.offset)==0x5a5a5a5a);
            else {code_leaves(s,index,phase==7);assert(!*(uint32_t *)s.devPointer(a.offset));}
        }
        assert(s.platformPoisoned);cleanFail=0;quarantine(s,a.offset);
    }
    // The authenticated pack gate must fail before touching owned bytes.
    GiuliaGPU s;prepare(s,false);unsigned i=add(s);auto a=s.devPool.allocations[i];
    auto saved=bytes(s,a.offset,a.bytes);((uint8_t *)s.firmwareMemory->cpu)[4096]^=1;
    assert(command(s,7,GG_ASSET_SQE,a.offset+4092)==4);
    assert(!s.devUcode[i] && bytes(s,a.offset,a.bytes)==saved);
}
static void memstore_leaves(GiuliaGPU &s,uint32_t flags) {
    for(unsigned at=0;at<32768;at+=4096) {
        uint64_t pa=s.memstoreMemory->pa+at;
        uint64_t ga=flags?((3ull<<53)|0xe03):0;
        uint64_t ma=(flags&8)?(0xe03ull|((flags&16)?0:0x40)|((flags&32)?0:0x80)):0;
        assert(leaf(s.gpuTableMemory,0x66000000+at)==(pa|ga));
        assert(leaf(s.tableMemory,0x66000000+at)==(pa|ma));
    }
}
static void memstore() {
    // All independent GMU permission combinations retain a single 32 KiB
    // timestamp provider and never consume or expose ordinary pool offsets.
    for(unsigned access=0;access<8;access++) {
        GiuliaGPU s;prepare(s,false);memstore_leaves(s,0);
        auto *words=(uint32_t *)s.memstoreMemory->cpu;
        for(unsigned i=0;i<32768/4;i++)assert(!words[i]);
        assert(command(s,17,24,0,0x307,22)==8 && !s.memstoreMapped);
        assert(!s.configureDevMappings());s.active();
        uint32_t flags=0x307|(access<<3);
        assert(!command(s,17,24,0x0f16,flags,22));
        assert(s.devPool.count==0 && s.memstoreMapped && s.memstoreFlags==flags);
        assert(logs.find("cpu_offset=none mapped=1")!=std::string::npos);
        assert(logs.find("reused=0")!=std::string::npos);memstore_leaves(s,flags);
        // Simulate device-written SOP/EOP, including 32-bit wrap values.
        words[10]=0xffffffff;words[12]=0x12345678;
        std::vector<uint32_t> before(words,words+32768/4);
        assert(!command(s,17,32768,0,flags,22));assert(logs.find("reused=1")!=std::string::npos);
        assert(!memcmp(before.data(),words,32768));memstore_leaves(s,flags);
        assert(!command(s,18,1));
        assert(logs.find("sop=0xffffffff eop=0x12345678 stable=1 completion_verified=0")!=std::string::npos);
        assert(!command(s,18,812));assert(command(s,18,813)==4);
        assert(command(s,18,1,1)==4);
        assert(command(s,17,32769,0,flags,22)==4);
        assert(command(s,17,24,0x1000,flags,22)==4);
        assert(command(s,17,24,0,flags^8,22)==4); // Immutable singleton permissions.
        for(unsigned bad:{0x306u,0x303u,0x387u,0x1007u})assert(command(s,17,24,0,bad,22)==4);
        assert(command(s,1,32768,4096,flags,22)==3);
        assert(command(s,3,0x66000000,0)==4 && command(s,4,0x66000000,32768)==4);
        assert(!command(s,16));assert(!memcmp(before.data(),words,32768));
    }
    // No success response on table publication or TLBI failure after APPLY.
    for(unsigned failure=0;failure<3;failure++) {
        GiuliaGPU s;prepare(s,true);
        if(failure==0)cleanFail=cleanCalls+1; // Preflight, before any leaf edit.
        if(failure==1)cleanFail=cleanCalls+3;
        if(failure==2)pollFail=pollCalls+3;
        assert(command(s,17,32768,0,0x33f,22)==8);
        assert(logs.find("cpu_offset=none mapped=1")==std::string::npos);
        cleanFail=pollFail=0;
        if(failure) {
            assert(s.platformPoisoned && s.memstoreMapped && s.memstoreFlags==0x33f);
            memstore_leaves(s,0x33f);
            auto calls=cleanCalls;assert(command(s,17,32768,0,0x33f,22)==8 && cleanCalls==calls);
            assert(s.memstoreRead(1)==0); // Dedicated normal RAM remains observable.
        } else {assert(!s.memstoreMapped && !s.platformPoisoned);memstore_leaves(s,0);}
    }
    // Descriptor metadata is not enough: a changed PTE must fail the audit.
    GiuliaGPU s;prepare(s,true);assert(!command(s,17,32768,0,0x33f,22));
    auto *table=(uint64_t *)s.tableMemory->cpu;size_t page=0;
    for(unsigned level=1;level<3;level++)
        page=((table[page*512+((0x66000000ull>>(39-9*level))&(level==1?3:511))]&0x0000fffffffff000ull)-s.tableMemory->pa)/4096;
    table[page*512]^=0x40;
    assert(command(s,16)==8 && s.platformPoisoned);
}
static void ipc_leaves(GiuliaGPU &s,uint32_t flags) {
    uint64_t attrs=flags?(0xe03ull|((flags&16)?0:0x40)|((flags&32)?0:0x80)):0;
    for(unsigned at=0;at<0x140000;at+=4096)
        assert(leaf(s.tableMemory,0x67000000+at)==((0x400000ull+at)|attrs));
    auto *gpu=(uint64_t *)s.gpuTableMemory->cpu;
    auto middle=((gpu[1]&0x0000fffffffff000ull)-s.gpuTableMemory->pa)/4096;
    assert(gpu[middle*512+((0x67000000u>>21)&511)]==0); // No GPU page table branch.
}
static void ipc() {
    unsigned qmp,audio;
    for(unsigned flags:{8u,0x18u,0x28u,0x38u,0x48u,0x58u,0x68u,0x78u}) {
        GiuliaGPU s;prepare(s,true);ipc_leaves(s,0);unsigned before=allocated;
        assert(ggi_claim(&gpuIpcOwner,GGI_HOST_QMP,&qmp));
        assert(ggi_claim(&gpuIpcOwner,GGI_HOST_AUDIO,&audio));
        assert(command(s,19,4096,0,flags,19)==3 && !s.ipcMapped);
        assert(ggi_release(&gpuIpcOwner,GGI_HOST_QMP,&qmp));
        assert(command(s,19,4096,0,flags,19)==3 && !s.ipcMapped);
        assert(ggi_release(&gpuIpcOwner,GGI_HOST_AUDIO,&audio));
        qmpAvailable=false;assert(command(s,19,4096,0,flags,19)==3);qmpAvailable=true;
        ipc_leaves(s,0);assert(!s.platformPoisoned);
        assert(!command(s,19,4096,0x1216,flags,19));ipc_leaves(s,flags);
        assert(allocated==before && s.ipcMapped && s.ipcFlags==flags && !s.devPool.count);
        assert(logs.find("gpu=0 gmu=0x67000000")!=std::string::npos);
        assert(logs.find("host_excluded=1")!=std::string::npos);
        assert(!ggi_claim(&gpuIpcOwner,GGI_HOST_QMP,&qmp) && !ggi_claim(&gpuIpcOwner,GGI_HOST_AUDIO,&audio));
        assert(!command(s,19,0x140000,0,flags,19));assert(logs.find("reused=1")!=std::string::npos);
        assert(command(s,19,4096,0,flags^16,19)==4);
        assert(command(s,19,0x140001,0,flags,19)==4);
        assert(command(s,19,4096,0x1300,flags,19)==4);
        for(unsigned bad:{0u,0x39u,0xb8u,0x138u,0x238u,0x1038u})assert(command(s,19,4096,0,bad,19)==4);
        assert(command(s,1,4096,4096,flags,19)==3);
        assert(command(s,2,0x67000000,1)==4 && command(s,4,0x67000000,4096)==4);
        assert(!command(s,16));ipc_leaves(s,flags);
    }
    for(unsigned failure=0;failure<3;failure++) {
        GiuliaGPU s;prepare(s,true);
        if(failure==0)cleanFail=cleanCalls+1;
        if(failure==1)cleanFail=cleanCalls+3;
        if(failure==2)pollFail=pollCalls+3;
        assert(command(s,19,4096,0,0x38,19)==8);
        assert(logs.find("mapped=1 host_excluded=1")==std::string::npos);
        cleanFail=pollFail=0;
        if(failure) {
            assert(s.platformPoisoned && s.ipcMapped && ggi_owns(&gpuIpcOwner,GGI_GPU,&s));
            ipc_leaves(s,0x38);assert(command(s,19,4096,0,0x38,19)==8);
            assert(!ggi_claim(&gpuIpcOwner,GGI_HOST_QMP,&qmp));
        } else {assert(!s.platformPoisoned && !s.ipcMapped);ipc_leaves(s,0);}
    }
}
// Synthetic authoritative peer for host failure injection only. There is no
// target implementation returning these proof bits or fabricated READY state.
struct FenceFixture {
    struct ggf_layout layout;std::vector<uint32_t> memory;
    struct ggf_proof proof={};unsigned observations=0,barriers=0,loseAt=0,breakAt=0;
    bool live=true;
    FenceFixture(unsigned page=4096) {
        assert(ggf_layout(page,&layout));memory.resize(layout.bytes/4,0);
        memory[0]=0;memory[1]=layout.bytes;memory[2]=24;memory[3]=56;memory[4]=memory[5]=2;
        for(unsigned q=0;q<2;q++) {
            unsigned h=(24+q*56)/4;
            memory[h+1]=(uint32_t)layout.physical+136+q*51200;
            memory[h+2]=q+1;memory[h+3]=51200;memory[h+4]=64;
        }
        proof={55,GGF_CARVEOUT_PHYSICAL,layout.physical,GGF_CARVEOUT_BYTES,layout.bytes,page,3,5,6,1,GGFP_REQUIRED};
    }
    static int observe(void *cookie,struct ggf_proof *out) {
        auto &f=*(FenceFixture *)cookie;++f.observations;
        if(f.observations==f.loseAt)++f.proof.epoch;
        *out=f.proof;return f.live?1:0;
    }
    static void barrier(void *cookie) {
        auto &f=*(FenceFixture *)cookie;++f.barriers;
        if(f.barriers==f.breakAt)f.memory[0]=1;
        __sync_synchronize();
    }
    struct ggf_provider provider() {
        return {GGFP_ABI,sizeof(struct ggf_provider),this,this,55,layout,memory.data(),observe,barrier};
    }
};
static void fence_leaves(GiuliaGPU &s,const FenceFixture *f,uint32_t flags=0) {
    for(unsigned at=0;at<0x200000;at+=4096) {
        uint64_t expected=0;
        if(f && at<f->layout.bytes)
            expected=(f->layout.physical+at)|0xe03ull|((flags&16)?0:0x40)|((flags&32)?0:0x80);
        assert(leaf(s.tableMemory,0x68000000+at)==expected);
    }
    auto *gpu=(uint64_t *)s.gpuTableMemory->cpu;
    auto middle=((gpu[1]&0x0000fffffffff000ull)-s.gpuTableMemory->pa)/4096;
    assert(!gpu[middle*512+((0x68000000u>>21)&511)]);
}
static void fence() {
    {GiuliaGPU s;prepare(s,true);fence_leaves(s,nullptr);
        auto clean=cleanCalls;assert(command(s,20,4096,0,0x178,23)==7 && cleanCalls==clean);
        assert(!command(s,21) && logs.find("registered=0 ready=0 mapped=0")!=std::string::npos);}
    for(unsigned page:{4096u,16384u,65536u}) {
        FenceFixture f(page);GiuliaGPU s;prepare(s,true);auto p=f.provider();
        assert(giulia_gpu_fence_register(&p));p.epoch=99; // Registry freezes caller metadata.
        assert(ggfp_get(&gpuFenceProvider)->epoch==55 && !giulia_gpu_fence_register(&p));
        assert(command(s,20,4096,0,0x178,23)==8 && !f.observations);
        assert(!command(s,19,4096,0,0x38,19));
        assert(!giulia_gpu_fence_ipcc_claim(&s,55) && !giulia_gpu_fence_ipcc_claim(&f,56));
        auto copy=f.memory;auto allocations=allocated;
        assert(!command(s,20,4096,0,0x178,23));fence_leaves(s,&f,0x178);
        assert(allocated==allocations && s.fenceMapped && s.fenceFlags==0x178 && f.memory==copy);
        assert(logs.find("gpu=0 gmu=0x68000000")!=std::string::npos);
        assert(logs.find("completion_verified=0")!=std::string::npos);
        assert(giulia_gpu_fence_ipcc_owns(&f,55) && !ggi_release(&gpuIpcOwner,GGI_GPU,&s));
        unsigned observed=f.observations;assert(!command(s,20,f.layout.bytes,0,0x178,23));
        assert(f.observations>observed && logs.find("reused=1")!=std::string::npos);
        assert(!command(s,21));assert(logs.find("completion_verified=0")!=std::string::npos);
        assert(!command(s,16));assert(f.memory==copy);
        assert(command(s,20,f.layout.bytes+1,0,0x178,23)==4);
        assert(command(s,20,4096,0x1600,0x178,23)==4);
        assert(command(s,20,4096,0,0x138,23)==4);
        assert(command(s,1,4096,4096,0x178,23)==3);
        assert(command(s,2,0x68000000,1)==4 && command(s,4,0x68000000,4096)==4);
    }
    // Incomplete ownership/READY and malformed headers never enable a leaf.
    for(unsigned failure=0;failure<12;failure++) {
        FenceFixture f;GiuliaGPU s;prepare(s,true);auto p=f.provider();assert(giulia_gpu_fence_register(&p));
        assert(!command(s,19,4096,0,0x38,19));
        if(failure<7)f.proof.proofs^=1u<<failure;
        if(failure==7)f.proof.queue_physical+=4096;
        if(failure==8)f.proof.peer=4;
        if(failure==9)f.proof.page_bytes=16384;
        if(failure==10)f.memory[1]/=4; // Vendor size is bytes, not dwords.
        if(failure==11)f.memory[(24+48)/4]=1;
        auto clean=cleanCalls;assert(command(s,20,4096,0,0x178,23)==7);
        assert(!s.fenceMapped && !s.platformPoisoned && cleanCalls==clean);fence_leaves(s,nullptr);
    }
    // Lost epoch before import, after publication, and publication/TLBI faults
    // are distinct: no success response, no queue writes and no reclamation.
    for(unsigned failure=0;failure<5;failure++) {
        FenceFixture f;GiuliaGPU s;prepare(s,true);auto p=f.provider();assert(giulia_gpu_fence_register(&p));
        assert(!command(s,19,4096,0,0x38,19));auto copy=f.memory;
        if(failure==0)f.loseAt=2;
        if(failure==1)f.loseAt=5;
        if(failure==2)cleanFail=cleanCalls+3;
        if(failure==3)pollFail=pollCalls+3;
        if(failure==4)f.breakAt=2;
        assert(command(s,20,4096,0,0x178,23)==((failure==0 || failure==4)?7:8));
        assert(logs.find("cpu_offset=none mapped=1")==std::string::npos);
        cleanFail=pollFail=0;
        if(failure==0 || failure==4) {assert(!s.fenceMapped);fence_leaves(s,nullptr);}
        else {assert(s.fenceMapped && s.platformPoisoned);fence_leaves(s,&f,0x178);
            assert(command(s,20,4096,0,0x178,23)==8);}
        if(failure!=4)assert(f.memory==copy);
    }
    {FenceFixture f;GiuliaGPU s;prepare(s,true);auto p=f.provider();assert(giulia_gpu_fence_register(&p));
        assert(!command(s,19,4096,0,0x38,19) && !command(s,20,4096,0,0x178,23));
        ++f.proof.epoch;assert(command(s,16)==8 && s.platformPoisoned);fence_leaves(s,&f,0x178);}
    for(unsigned op:{1u,3u,4u,5u,6u,7u,10u,11u,12u,13u,17u,19u,20u}) {
        FenceFixture f;GiuliaGPU s;prepare(s,true);auto p=f.provider();assert(giulia_gpu_fence_register(&p));
        unsigned allocation=add(s,12288,0x33f,2);uint32_t offset=s.devPool.allocations[allocation].offset;
        assert(!command(s,19,4096,0,0x38,19) && !command(s,20,4096,0,0x178,23));
        auto copy=bytes(s,offset,12288);auto count=s.devPool.count,clean=cleanCalls;
        uint32_t packet[255];memcpy(packet,s.devPacket,sizeof(packet));
        auto *hfi=(uint8_t *)s.allocations[s.hfiIndex]->cpu;
        std::vector<uint8_t> queue(hfi,hfi+GGH_BYTES);f.live=false;
        assert(command(s,op,offset,4096,0x33f,2)==8 && s.platformPoisoned);
        assert(cleanCalls==clean && s.devPool.count==count && bytes(s,offset,12288)==copy);
        assert(!memcmp(packet,s.devPacket,sizeof(packet)) && !memcmp(queue.data(),hfi,GGH_BYTES));
    }
}
int main(int argc,char **argv) {
    assert(argc==2);std::ifstream file(argv[1],std::ios::binary);assert(file);
    fixturePack={std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    assert(gg_pack_open(fixturePack.data(),fixturePack.size(),&decoded));
    seal(false,GG_ASSET_SQE,false);seal(true,GG_ASSET_SQE,true);seal(true,GG_ASSET_AQE,false,true);
    command_guards();rm_dispatch();failures();memstore();ipc();fence();assert(allocated==freed);
    puts("Actual GPU devCommand: RAM/UCODE, memstore, IPC exclusion, registered fence import/live epochs, paired provider ownership, publication/TLBI failure retention and quarantine passed (synthetic provider; no controller)");
}
'''
fixture = fixture.replace('__asm__ volatile("dsb sy" ::: "memory")', '__sync_synchronize()')
with tempfile.TemporaryDirectory(prefix="gpu-dev-runtime-") as folder:
    path = Path(folder)
    (path / "commands.cpp").write_text(fixture)
    (path / "fixture.pack").write_bytes(pack)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-c", str(ROOT / "ios16/vendor/ArmSmmu4K/ArmSmmu4K.c"),
                    "-o", str(path / "tables.o")], check=True)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-misleading-indentation", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    *include_flags(ROOT), str(path / "commands.cpp"), str(path / "tables.o"),
                    "-o", str(path / "commands")], check=True)
    subprocess.run([str(path / "commands"), str(path / "fixture.pack")], check=True)
