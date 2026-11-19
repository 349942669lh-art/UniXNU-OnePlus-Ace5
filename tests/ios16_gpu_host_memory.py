"""Exercise production GMU allocation/copy/table preparation with IOKit doubles.

The fixture enforces Normal RAM and explicit publication, but cannot emulate
ARM page attributes or prove device DMA/cache coherence. Optional pinned pack
tests the exact five reservations used on the phone, without uploading firmware.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'ios16/gpu/GiuliaGPU.cpp').read_text()
begin = source.index('static IOBufferMemoryDescriptor *gpuAllocateHostMemory(')
end = source.index('\nint GiuliaGPU::readRegister(', begin)
fixture = r'''
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_cache.h"
#include "giulia_gpu_hfi.h"
#include "giulia_gpu_dev.h"
#include "giulia_gpu_mapping.h"
#include "giulia_gpu_memstore_mapping.h"
#include "giulia_gpu_ipc.h"
#include "giulia_gpu_fence_mapping.h"
#include "vendor/ArmSmmu4K/ArmSmmu4K.h"
using IOByteCount=uint64_t;
static unsigned long gPhysBase=0x880000000ULL,gPhysSize=0x100000000ULL;
static void *kernel_task=nullptr;
enum { kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=0x10,
    kIOMemoryHostPhysicallyContiguous=0x80,kIOMemoryMapperNone=0x800,
    kIOMapInhibitCache=0x100,kIOMapCopybackCache=0x400,kIOMapWriteCombineCache=0x500 };
static unsigned allocated,freed,calls,failAt,badAt,badKind,cleanCalls,cleanFail;
static unsigned scratchCalls,scratchFail,scratchLive;
struct Publication {const void *cpu;std::vector<uint8_t> contents;};
static std::vector<Publication> tablePublications;
static void IOLog(const char *,...) {}
static void gpuHfiBarrier(void *) { __sync_synchronize(); }
static void *IOMalloc(size_t bytes) {
    if(++scratchCalls==scratchFail)return nullptr;
    ++scratchLive;return malloc(bytes);
}
static void IOFree(void *p,size_t){--scratchLive;free(p);}
struct IOBufferMemoryDescriptor {
    void *cpu;size_t bytes;uint64_t pa,span;unsigned kind,cache;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *,unsigned options,size_t n,size_t alignment) {
        ++calls;if(calls==failAt)return nullptr;
        // These assertions reject the original panic-causing allocation and
        // an allocation that could be routed through the system IOMMU.
        unsigned base=kIODirectionInOut|kIOMemoryHostPhysicallyContiguous|kIOMemoryMapperNone;
        assert(options==(base|kIOMapCopybackCache) || options==(base|kIOMapWriteCombineCache));
        assert(alignment==0x4000 && n && !(n&0x3fff));
        auto *p=new IOBufferMemoryDescriptor;
        p->cache=options&0x700;
        assert(!posix_memalign(&p->cpu,alignment,n));
        memset(p->cpu,0xa5,n);p->bytes=n;
        p->pa=gPhysBase+calls*0x800000;p->span=n;
        p->kind=calls==badAt?badKind:0;
        if(p->kind==1)++p->pa;
        if(p->kind==2)--p->span;
        if(p->kind==3)p->pa=gPhysBase-0x4000;
        if(p->kind==4)p->pa=gPhysBase+gPhysSize;
        if(p->kind==5)p->pa=1ULL<<36;
        ++allocated;return p;
    }
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *length,unsigned options) {
        assert(!offset && options==kIOMemoryMapperNone);*length=span;return pa;
    }
    void *getBytesNoCopy(){return kind==6?nullptr:kind==7?(char *)cpu+1:cpu;}
    size_t getLength(){return bytes;}
    void release(){free(cpu);++freed;delete this;}
};
extern "C" int giulia_gpu_cache_publish(const void *p,size_t n) {
    assert(p && !((uintptr_t)p&0x3fff) && n && !(n&0x3fff));
    if(n==128*4096)tablePublications.push_back({p,std::vector<uint8_t>((const uint8_t *)p,(const uint8_t *)p+n)});
    return ++cleanCalls!=cleanFail;
}
class GiuliaGPU {
public:
    IOBufferMemoryDescriptor *allocations[64]={},*tableMemory=nullptr;
    unsigned reservationCount=0,tablePages=0,leafPages=0,hfiIndex=0;bool hostPrepared=false;
    IOBufferMemoryDescriptor *devMemory[GGD_CHUNKS]={},*gpuTableMemory=nullptr;
    IOBufferMemoryDescriptor *memstoreMemory=nullptr;
    uint32_t memstoreFlags=0;bool memstoreMapped=false;
    uint64_t gmuRoot=0,gpuRoot=0;
    struct ggd_pool devPool={};bool manualMemoryPinned=true,rmOnlyTest=false;
    bool devMappingsManaged=false,devUcode[GGD_MAX]={},platformPoisoned=false,fenceMapped=false;
    int auditFenceMappings(){return 6;}
    // This allocation fixture never authorizes or invokes a Gunyah operation.
    int gunyahCommand(uint32_t,uint32_t,uint32_t,uint32_t){assert(false);return 6;}
    uint32_t devPacket[255]={};bool devWritten[255]={};
    IOBufferMemoryDescriptor *firmwareMemory=nullptr;
    struct Map {uintptr_t getVirtualAddress(){return 0;}} *firmwareMap=nullptr;
    uint8_t *devPointer(uint32_t);void devAllocation(unsigned);void devCommand(const uint32_t *);
    int platformCommand(unsigned){return 6;}
    int secureAllocate(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int memstoreAllocate(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int memstoreRead(uint32_t){return 6;}
    int ipcAllocate(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int fenceAllocate(uint32_t,uint32_t,uint32_t,uint32_t){return 6;}
    int fenceStatus(){return 6;}
    int platformWatch(unsigned,unsigned){return 6;}
    int platformSnapshot(unsigned){return 6;}
    int mapDevAllocation(unsigned,bool=false){return 6;}
    int auditDevMappings(){return 6;}
    bool prepareDevMemory(ArmSmmu4KRange *,size_t *);
    bool prepareInitialMappings();
    bool prepareMemory(const gg_pack *);
    void setProperty(const char *,unsigned long long,unsigned){}
    void setProperty(const char *,const char *){}
    void releaseResources(){for(auto *p:allocations)if(p)p->release();if(tableMemory)tableMemory->release();for(auto *p:devMemory)if(p)p->release();if(gpuTableMemory)gpuTableMemory->release();if(memstoreMemory)memstoreMemory->release();}
};
'''
fixture += source[begin:end]
impl=(ROOT/'ios16/gpu/giulia_gpu_dev_impl.h').read_text()
fixture += impl.replace('__asm__ volatile("dsb sy" ::: "memory")','__sync_synchronize()')
mapping=(ROOT/'ios16/gpu/giulia_gpu_mapping_impl.h').read_text()
fixture += mapping[:mapping.index('int GiuliaGPU::auditMemstoreMappings()')]
fixture += r'''
static void reset() {
    allocated=freed=calls=failAt=badAt=badKind=cleanCalls=cleanFail=0;
    scratchCalls=scratchFail=scratchLive=0;
    tablePublications.clear();
}
static uint64_t leaf(IOBufferMemoryDescriptor *memory,uint64_t va) {
    auto *words=(uint64_t *)memory->cpu;size_t page=0;
    for(unsigned level=1;level<3;level++) {
        uint64_t entry=words[page*512+((va>>(39-9*level))&511)];
        assert((entry&~UINT64_C(0x0000fffffffff000))==3);
        uint64_t pa=entry&UINT64_C(0x0000fffffffff000);
        assert(pa>=memory->pa && pa-memory->pa<memory->bytes);
        page=(pa-memory->pa)/4096;
    }
    return words[page*512+((va>>12)&511)];
}
static void verifyFirstPublication(IOBufferMemoryDescriptor *memory) {
    unsigned count=0;
    for(const auto &publication:tablePublications)if(publication.cpu==memory->cpu) {
        ++count;
        assert(publication.contents.size()==memory->bytes);
        assert(!memcmp(publication.contents.data(),memory->cpu,memory->bytes));
    }
    assert(count==1); // Tombstones must be present at the first publication.
}
static void verify(GiuliaGPU &service,const gg_pack &pack) {
    gg_fw_block blocks[64];gg_reservation plan[64];size_t count,n;
    assert(gg_fw_plan(pack.asset[0].data,pack.asset[0].size,blocks,64,&count)==GG_FW_OK);
    assert(gg_memory_plan(blocks,count,plan,64,&n)==GG_FW_OK);
    size_t firmware=n;
    plan[n++]={GGH_IOVA,GGH_BYTES,GG_KERNEL,0};
    plan[n++]={GGH_LOG_IOVA,GGH_LOG_BYTES,GG_KERNEL,0};
    plan[n++]={GGH_VRB_IOVA,GGH_VRB_BYTES,GG_KERNEL,0};
    assert(service.reservationCount==n && cleanCalls==n+3);
    assert(service.hfiIndex==firmware && service.tableMemory->cache==kIOMapCopybackCache);
    for(size_t i=0;i<n;++i) {
        auto *a=service.allocations[i];std::vector<unsigned char> expected(a->bytes,0);
        assert(a->cache==(i<firmware?kIOMapCopybackCache:kIOMapWriteCombineCache));
        for(size_t j=0;j<count;++j) {
            auto &b=blocks[j];
            if(b.size && b.address>=plan[i].iova && b.size<=plan[i].bytes &&
               b.address-plan[i].iova<=plan[i].bytes-b.size)
                memcpy(expected.data()+b.address-plan[i].iova,pack.asset[0].data+b.payload_offset,b.size);
        }
        if(i==firmware)assert(ggh_init(expected.data(),expected.size(),GGH_IOVA)==GGH_OK);
        assert(!memcmp(expected.data(),a->cpu,a->bytes)); // includes every padding byte
    }
    auto *pool=(uint64_t *)service.tableMemory->cpu;
    for(size_t i=service.tablePages*512;i<service.tableMemory->bytes/8;++i)assert(!pool[i]);
    // The unbound provider aperture has its own all-zero L3 before either
    // table is published: no physical-address tombstones or valid leaves.
    for(unsigned at=0;at<GGF_APERTURE;at+=4096)
        assert(leaf(service.tableMemory,GGF_IOVA+at)==0);
    // No GPU-side branch may be created for this GMU-only provider aperture.
    auto *gpuWords=(uint64_t *)service.gpuTableMemory->cpu;
    uint64_t gpuParent=gpuWords[GGF_IOVA>>30];
    assert((gpuParent&~UINT64_C(0x0000fffffffff000))==3);
    uint64_t gpuParentPa=gpuParent&UINT64_C(0x0000fffffffff000);
    assert(gpuParentPa>=service.gpuTableMemory->pa &&
           gpuParentPa-service.gpuTableMemory->pa<service.gpuTableMemory->bytes);
    size_t gpuL2=(gpuParentPa-service.gpuTableMemory->pa)/4096;
    assert(gpuWords[gpuL2*512+((GGF_IOVA>>21)&511)]==0);
    // Every ordinary GPU/GMU leaf and cached GMU alias starts invalid,
    // retaining exactly its owned PA with no permissions or attribute bits.
    for(unsigned chunk=0;chunk<GGD_CHUNKS;chunk++)for(unsigned at=0;at<GGD_CHUNK;at+=4096) {
        uint64_t off=chunk*(uint64_t)GGD_CHUNK+at,pa=service.devMemory[chunk]->pa+at;
        assert(leaf(service.gpuTableMemory,GGD_BASE+off)==pa);
        assert(leaf(service.tableMemory,GGD_BASE+off)==pa);
        if(off<GGD_CACHE_BYTES)assert(leaf(service.tableMemory,GGD_CACHE+off)==pa);
    }
    // The singleton owns independent zeroed Normal NC RAM, with eight invalid
    // leaves in each address space until a checked MEMSTORE request imports it.
    auto *memstore=service.memstoreMemory;
    assert(memstore && memstore->bytes==GGMS_BYTES && memstore->cache==kIOMapWriteCombineCache);
    assert(!service.memstoreMapped && !service.memstoreFlags);
    for(size_t at=0;at<memstore->bytes;at++)assert(!((uint8_t *)memstore->cpu)[at]);
    for(auto *a:service.devMemory) {
        assert(memstore!=a);
        assert(memstore->pa+memstore->bytes<=a->pa || a->pa+a->bytes<=memstore->pa);
    }
    for(unsigned at=0;at<GGMS_BYTES;at+=4096) {
        assert(leaf(service.gpuTableMemory,GGMS_IOVA+at)==memstore->pa+at);
        assert(leaf(service.tableMemory,GGMS_IOVA+at)==memstore->pa+at);
    }
    for(unsigned at=0;at<GGIPC_BYTES;at+=4096)
        assert(leaf(service.tableMemory,GGIPC_IOVA+at)==GGIPC_PHYSICAL+at);
    verifyFirstPublication(service.gpuTableMemory);
    verifyFirstPublication(service.tableMemory);
    // Independent walk: every prepared 4K page resolves to its owned PA.
    for(size_t i=0;i<n;++i)for(size_t off=0;off<plan[i].bytes;off+=4096) {
        uint64_t va=plan[i].iova+off;size_t table=0;
        for(unsigned level=1;level<3;++level) {
            auto entry=pool[table*512+((va>>(39-9*level))&511)];assert((entry&3)==3);
            table=((entry&0x0000fffffffff000ULL)-service.tableMemory->pa)/4096;
            assert(table<service.tablePages);
        }
        auto entry=pool[table*512+((va>>12)&511)];
        assert((entry&0x0000fffffffff000ULL)==service.allocations[i]->pa+off);
        assert((entry&3)==3 && !(entry&4)); // GMU-side Normal NC remains explicit
    }
}
static void run(const gg_pack &pack,bool stock) {
    reset();GiuliaGPU good;assert(good.prepareMemory(&pack) && good.hostPrepared);verify(good,pack);
    if(stock)assert(good.reservationCount==8 && good.tablePages==43 && good.leafPages==17875);
    unsigned total=calls, totalCleans=cleanCalls;good.releaseResources();assert(allocated==freed && !scratchLive);
    for(unsigned i=1;i<=3;++i) {
        reset();scratchFail=i;GiuliaGPU s;assert(!s.prepareMemory(&pack) && !s.hostPrepared);
        s.releaseResources();assert(!calls && !scratchLive);
    }
    for(unsigned i=1;i<=total;++i) {
        reset();failAt=i;GiuliaGPU s;assert(!s.prepareMemory(&pack) && !s.hostPrepared);
        s.releaseResources();assert(allocated==freed && !scratchLive);
        for(unsigned kind=1;kind<=7;++kind) {
            reset();badAt=i;badKind=kind;
            // Include addresses inside host RAM but outside the GPU's 36-bit aperture.
            if(kind==5)gPhysSize=1ULL<<38;
            GiuliaGPU bad;assert(!bad.prepareMemory(&pack) && !bad.hostPrepared);
            bad.releaseResources();assert(allocated==freed && !scratchLive);
            gPhysSize=0x100000000ULL;
        }
    }
    for(unsigned i=1;i<=totalCleans;i++) {
        reset();cleanFail=i;GiuliaGPU bad;assert(!bad.prepareMemory(&pack) && !bad.hostPrepared);
        bad.releaseResources();assert(allocated==freed && !scratchLive);
    }
    printf("Production GPU host memory: %u allocations, contents/padding, independent tables and %u failure cases passed%s\n",
           total,3+8*total+totalCleans,stock?" (pinned stock firmware)":"");
}
int main(int argc,char **argv) {
    // Persistent RAM plus a payload at +4 exercises non-cache-line-aligned copying.
    uint32_t data[]={0x60000000,0,7,4096,0x60000004,4,0,0,0x12345678};
    gg_pack pack={};pack.asset[0].data=(const uint8_t *)data;pack.asset[0].size=sizeof(data);run(pack,false);
    if(argc==2) {
        std::ifstream f(argv[1],std::ios::binary);assert(f);
        std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
        assert(gg_pack_open(blob.data(),blob.size(),&pack));run(pack,true);
    }
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)
    (path / 'memory.cpp').write_text(fixture)
    vendor = ROOT / 'ios16/vendor/ArmSmmu4K'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-c', str(vendor / 'ArmSmmu4K.c'),
                    '-o', str(path / 'tables.o')], check=True)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    *include_flags(ROOT), str(path / 'memory.cpp'),
                    str(path / 'tables.o'), '-o', str(path / 'memory')], check=True)
    subprocess.run([str(path / 'memory'), *sys.argv[1:]], check=True)
