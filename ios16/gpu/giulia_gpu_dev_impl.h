/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Included after GiuliaGPU definitions. This is an explicit development pool,
 * not a production task-isolated allocator. Prepared tables become restricted
 * by per-allocation policy before activation. Backing remains pinned until
 * reboot after gpu.hfi.pin; a failed transition quarantines further writes.
 */
bool GiuliaGPU::prepareDevMemory(ArmSmmu4KRange *ranges,size_t *count)
{
    ArmSmmu4KRange gpu[GGD_CHUNKS+1];
    if(*count+GGD_CHUNKS+3>GG_FW_MAX_BLOCKS)return false;
    for(unsigned i=0;i<GGD_CHUNKS;i++) {
        devMemory[i]=gpuAllocateHostMemory(GGD_CHUNK,true);
        IOByteCount contiguous=0;
        uint64_t pa=devMemory[i]?devMemory[i]->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
        void *cpu=devMemory[i]?devMemory[i]->getBytesNoCopy():NULL;
        if(!gpuHostRangeValid(cpu,pa,contiguous,GGD_CHUNK))return false;
        bzero(cpu,GGD_CHUNK);
        gpu[i]={GGD_BASE+(uint64_t)i*GGD_CHUNK,pa,GGD_CHUNK,15,0,2};
        ranges[(*count)++]=gpu[i];
        if(i==0) {ArmSmmu4KRange cached=gpu[i];cached.iova=GGD_CACHE;ranges[(*count)++]=cached;}
    }
    /* One persistent, shared timestamp store, separate from ordinary allocations.
     * Build its topology now; no device leaf remains valid after preparation. */
    memstoreMemory=gpuAllocateHostMemory(GGMS_BYTES,true);
    IOByteCount memstoreSpan=0;
    uint64_t memstorePA=memstoreMemory?memstoreMemory->getPhysicalSegment(0,&memstoreSpan,kIOMemoryMapperNone):0;
    void *memstoreCPU=memstoreMemory?memstoreMemory->getBytesNoCopy():NULL;
    if(!gpuHostRangeValid(memstoreCPU,memstorePA,memstoreSpan,GGMS_BYTES))return false;
    bzero(memstoreCPU,GGMS_BYTES);
    if(!giulia_gpu_cache_publish(memstoreCPU,GGMS_BYTES))return false;
    gpu[GGD_CHUNKS]={GGMS_IOVA,memstorePA,GGMS_BYTES,11,0,2};
    ranges[(*count)++]=gpu[GGD_CHUNKS];
    /* DT-bound IPC_CORE resource: GMU topology only, never RAM allocation,
     * CPU mapping, zeroing or an ordinary GPU alias. Initially invalid. */
    ranges[(*count)++]={GGIPC_IOVA,GGIPC_PHYSICAL,GGIPC_BYTES,15,0,2};
    for(size_t i=1;i<*count;i++)for(size_t j=i;j>0 && ranges[j].iova<ranges[j-1].iova;j--) {
        ArmSmmu4KRange tmp=ranges[j];ranges[j]=ranges[j-1];ranges[j-1]=tmp;
    }
    gpuTableMemory=gpuAllocateHostMemory(128*4096);
    IOByteCount contiguous=0;
    gpuRoot=gpuTableMemory?gpuTableMemory->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone):0;
    void *cpu=gpuTableMemory?gpuTableMemory->getBytesNoCopy():NULL;
    if(!gpuHostRangeValid(cpu,gpuRoot,contiguous,128*4096))return false;
    bzero(cpu,128*4096);
    ArmSmmu4KConfig config={32,36,gpuRoot,65536};ArmSmmu4KResult result;
    if(ArmSmmu4KBuild(&config,gpu,GGD_CHUNKS+1,(uint64_t *)cpu,128,&result)!=ArmSmmu4KSuccess)return false;
    return true; /* Both tables are invalidated before their first publication. */
}
uint8_t *GiuliaGPU::devPointer(uint32_t at)
{
    if(at>=GGD_BYTES || !devMemory[at/GGD_CHUNK])return NULL;
    uint8_t *cpu=(uint8_t *)devMemory[at/GGD_CHUNK]->getBytesNoCopy();
    return cpu && at%GGD_CHUNK<devMemory[at/GGD_CHUNK]->getLength()?cpu+at%GGD_CHUNK:NULL;
}
/* HOST_INIT is not a CPU write permission. Only ownership and sealed UCODE
 * constrain these explicit development writes; the HFI access flags describe
 * GPU/GMU mappings. Once firmware is sealed its entire allocation is immutable. */
static bool gpuDevWritable(const struct ggd_pool *pool,const bool *ucode,
    bool managed,uint32_t at,uint32_t bytes)
{
    if(!ggd_bounds(at,bytes) || !ggd_pool_valid(pool) ||
       (managed && ggd_owner(pool,at,bytes)<0))return false;
    for(unsigned i=0;i<pool->count;i++) {
        const struct ggd_allocation &a=pool->allocations[i];
        if(ucode[i] && at<a.offset+a.bytes && a.offset<at+bytes)return false;
    }
    return true;
}
void GiuliaGPU::devAllocation(unsigned n)
{
    const struct ggd_allocation &a=devPool.allocations[n];
    uint32_t gpu=GGD_BASE+a.offset,gmu=(a.flags&0x80?GGD_CACHE:GGD_BASE)+a.offset;
    if(devMappingsManaged) {
        if(!ggm_attributes(a.flags,0,devUcode[n]))gpu=0;
        if(!ggm_attributes(a.flags,1,devUcode[n]))gmu=0;
    }
    IOLog("GIULIA GPU DEV: allocation=%u offset=%#x bytes=%#x flags=%#x kind=%u gpu=%#x gmu=%#x\n",
        n,a.offset,a.bytes,a.flags,a.kind,gpu,gmu);
}
void GiuliaGPU::devCommand(const uint32_t *v)
{
    unsigned token=v[0],op=v[1];uint32_t a=v[2],b=v[3],c=v[4],d=v[5];int code=0;
    if(rmOnlyTest && !ggd_rm_diagnostic_allowed(op,a))code=4;
    else if(!hostPrepared)code=1;
    else if(op!=0 && !(op==22 && a==0) && !manualMemoryPinned)code=2;
    /* Reads remain available for diagnosis. No CPU/HFI mutation may escape
     * quarantine after a mapping, publication, or TLB synchronization failure. */
    else if(platformPoisoned && op!=0 && op!=2 && op!=8 && op!=9 && op!=14 && op!=15 && op!=18 && !(op==22 && a==0))code=8;
    else if(fenceMapped && op!=0 && op!=2 && op!=8 && op!=9 && op!=14 && op!=15 && op!=18 && !(op==22 && a==0) &&
        auditFenceMappings())code=8;
    else if(!ggd_pool_valid(&devPool))code=8;
    else switch(op) {
    case 0:
        IOLog("GIULIA GPU DEV: abi=1 gmu_root=%#llx gpu_root=%#llx base=%#x bytes=%#x cache_base=%#x pinned=%u\n",
            (unsigned long long)gmuRoot,(unsigned long long)gpuRoot,GGD_BASE,GGD_BYTES,GGD_CACHE,manualMemoryPinned);
        IOLog("GIULIA GPU DEV: mappings=%s native_page=4096 lifecycle=until-reboot device_writes=0 quarantined=%u\n",
            devMappingsManaged?"per-allocation-normal-nc":"prepared-inactive-until-policy",platformPoisoned);
        IOLog("GIULIA GPU DEV: resources_abi=1 memstore=1 secure_allocation_mappings=1\n");
        IOLog("GIULIA GPU DEV: ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1\n");
        IOLog("GIULIA GPU DEV: fence_import_abi=1 fence_completion=0\n");
        IOLog("GIULIA GPU DEV: gunyah_owner_abi=1 current_boot_resources=1 guarded_hvc=1 fence_completion=0\n");break;
    case 1: {
        int n=ggd_alloc(&devPool,a,b,c,d);
        if(n<0){code=3;break;}
        if(devMappingsManaged) {
            code=mapDevAllocation((unsigned)n);
            if(code){platformPoisoned=true;break;}
        }
        devAllocation((unsigned)n);break;
    }
    case 2:
        if(!b || b>32 || (a&3) || !ggd_bounds(a,b*4)){code=4;break;}
        __asm__ volatile("dsb sy" ::: "memory");
        for(unsigned i=0;i<b;i++)IOLog("GIULIA GPU DEV: word=%u value=%#x\n",i,*(volatile uint32_t *)devPointer(a+i*4));
        break;
    case 3:
        if((a&3)||!gpuDevWritable(&devPool,devUcode,devMappingsManaged,a,4)){code=4;break;}
        *(volatile uint32_t *)devPointer(a)=b;__asm__ volatile("dsb sy" ::: "memory");break;
    case 4:
        if(!gpuDevWritable(&devPool,devUcode,devMappingsManaged,a,b)){code=4;break;}
        while(b) {uint32_t n=GGD_CHUNK-a%GGD_CHUNK;if(n>b)n=b;bzero(devPointer(a),n);a+=n;b-=n;}
        __asm__ volatile("dsb sy" ::: "memory");break;
    case 5:
        if(a>=255){code=4;break;}devPacket[a]=b;devWritten[a]=true;break;
    case 6: {
        if(!a || a>255 || ((devPacket[0]>>8)&255)!=a){code=4;break;}
        for(unsigned i=0;i<a;i++)if(!devWritten[i])code=5;
        if(code)break;
        struct ggh_io io={(volatile uint32_t *)allocations[hfiIndex]->getBytesNoCopy(),GGH_BYTES,GGH_IOVA,NULL,gpuHfiBarrier};
        code=ggh_send(&io,devPacket,a);
        if(!code)bzero(devWritten,sizeof(devWritten));
        break;
    }
    case 7: {
        struct gg_pack pack={};
        if(a>=GG_ASSETS || !firmwareMap || !firmwareMemory ||
           !gg_pack_open((const void *)firmwareMap->getVirtualAddress(),firmwareMemory->getLength(),&pack) ||
           pack.asset[a].size>UINT32_MAX ||
           !gpuDevWritable(&devPool,devUcode,devMappingsManaged,b,(uint32_t)pack.asset[a].size)) {code=4;break;}
        int owner=ggd_owner(&devPool,b,(uint32_t)pack.asset[a].size);
        bool ucode=a==GG_ASSET_SQE || a==GG_ASSET_AQE;
        if(owner<0 || (ucode && (devPool.allocations[owner].kind!=0 || !(devPool.allocations[owner].flags&1)))) {
            code=4;break;
        }
        /* Revoke data access before modifying bytes that will become executable.
         * The descriptor remains pinned and is never reused on a failed change. */
        if(ucode && devMappingsManaged) {
            code=mapDevAllocation((unsigned)owner,true);
            if(code){platformPoisoned=true;break;}
        }
        if(ucode) {
            uint32_t at=devPool.allocations[owner].offset,left=devPool.allocations[owner].bytes;
            while(left) {uint32_t n=GGD_CHUNK-at%GGD_CHUNK;if(n>left)n=left;bzero(devPointer(at),n);at+=n;left-=n;}
        }
        size_t left=pack.asset[a].size,offset=0;
        while(left){size_t n=GGD_CHUNK-b%GGD_CHUNK;if(n>left)n=left;memcpy(devPointer(b),pack.asset[a].data+offset,n);b+=n;offset+=n;left-=n;}
        __asm__ volatile("dsb sy" ::: "memory");
        if(ucode) {
            devUcode[owner]=true;
            if(devMappingsManaged) {
                code=mapDevAllocation((unsigned)owner);
                if(code){platformPoisoned=true;break;}
            }
        }
        IOLog("GIULIA GPU DEV: asset=%u bytes=%lu\n",a,(unsigned long)pack.asset[a].size);break;
    }
    case 8:for(unsigned i=0;i<devPool.count;i++)devAllocation(i);break;
    case 9:
        if(!ggd_bounds(a,1)){code=4;break;}
        IOLog("GIULIA GPU DEV: offset=%#x pa=%#llx\n",a,(unsigned long long)
            devMemory[a/GGD_CHUNK]->getPhysicalSegment(a%GGD_CHUNK,NULL,kIOMemoryMapperNone));break;
    case 10:bzero(devWritten,sizeof(devWritten));break;
    case 11:
        if(b||c||d || a>9){code=4;break;}
        code=platformCommand(a);break;
    case 12:code=secureAllocate(a,b,c,d);break;
    case 13:
        if(c||d){code=4;break;}
        code=platformWatch(a,b);break;
    case 14:
        if(a||b||c||d){code=4;break;}
        code=platformSnapshot(0);break;
    case 15: {
        if(c||d||!b||b>64||a>GGH_LOG_BYTES/4-b){code=4;break;}
        volatile uint32_t *words=(volatile uint32_t *)allocations[hfiIndex+1]->getBytesNoCopy();
        gpuHfiBarrier(NULL);
        for(unsigned i=0;i<b;i++)IOLog("GIULIA GPU DEV: log_word=%u value=%#x\n",a+i,words[a+i]);
        break;
    }
    case 16:
        if(a||b||c||d){code=4;break;}
        code=auditDevMappings();break;
    case 17:code=memstoreAllocate(a,b,c,d);break;
    case 18:
        if(b||c||d){code=4;break;}
        code=memstoreRead(a);break;
    case 19:code=ipcAllocate(a,b,c,d);break;
    case 20:code=fenceAllocate(a,b,c,d);break;
    case 21:
        if(a||b||c||d){code=4;break;}
        code=fenceStatus();break;
    case 22:code=gunyahCommand(a,b,c,d);break;
    default:code=6;break;
    }
    IOLog("GIULIA GPU DEV: done token=%u op=%u code=%d\n",token,op,code);
}
