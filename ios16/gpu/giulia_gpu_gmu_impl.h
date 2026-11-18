/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * GMU boot adapter: the only place where gg_gmu_load()/gg_gmu_start() reach
 * real hardware. Included from GiuliaGPU.cpp after the class definition.
 *
 * The adapter maps the GMU window read/write (Normal NC), translates vendor
 * word offsets, and implements the vendor register preparation and HFI table
 * handoff from giulia_gpu_gmu_boot.h. It never releases the CM3 reset, writes
 * TCM outside the state machine, starts HFI without a verified queue table or
 * publishes an accelerator.
 */
#ifndef GIULIA_GPU_GMU_IMPL_H
#define GIULIA_GPU_GMU_IMPL_H

/* GMU window from the captured DT: 0x03d68000 / 0x37000. Vendor word offsets
 * are relative to 0x03d00000, so the window covers 0x68000..0x9f000. */
enum { GGU_GMU_BASE=0x03d68000, GGU_GMU_BYTES=0x37000,
       GGU_GMU_FIRST=0x68000, GGU_GMU_LAST=0x9f000 };

bool GiuliaGPU::gmuEnsureWindow()
{
    if(gmuWindowMapped)return true;
    if(!gmuMemory)
        gmuMemory=IOMemoryDescriptor::withPhysicalAddress(GGU_GMU_BASE,GGU_GMU_BYTES,
            kIODirectionInOut);
    if(!gmuMemory)return false;
    if(!gmuMap)gmuMap=gmuMemory->map(kIOMapInhibitCache);
    if(!gmuMap)return false;
    gmuWindowMapped=true;
    return true;
}
int GiuliaGPU::gmuReadWord(void *cookie,uint32_t reg,uint32_t *value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t offset;
    if(!self || !value)return 1;
    offset=(uint64_t)reg*4u;
    if(offset<GGU_GMU_FIRST || offset+4u>GGU_GMU_LAST)return 1;
    if(!self->gmuEnsureWindow())return 1;
    *value=*(volatile uint32_t *)((uintptr_t)self->gmuMap->getVirtualAddress()+
        (size_t)(offset-GGU_GMU_FIRST));
    return 0;
}
int GiuliaGPU::gmuWriteWord(void *cookie,uint32_t reg,uint32_t value)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    uint64_t offset;
    if(!self)return 1;
    offset=(uint64_t)reg*4u;
    if(offset<GGU_GMU_FIRST || offset+4u>GGU_GMU_LAST)return 1;
    if(!self->gmuEnsureWindow() || self->gmuFaulted)return 1;
    *(volatile uint32_t *)((uintptr_t)self->gmuMap->getVirtualAddress()+
        (size_t)(offset-GGU_GMU_FIRST))=value;
    return 0;
}
int GiuliaGPU::gmuBarrierFn(void *cookie)
{
    (void)cookie;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int GiuliaGPU::gmuWaitFn(void *cookie,unsigned us)
{
    (void)cookie;
    if(!us || us>1000u)return 1;
    IODelay(us);
    return 0;
}
/* The state machine may only touch the GMU when the firmware preparation is
 * complete, this service owns the memory, the window is mapped and SID 5
 * translation is live. No reset or clock mutation happens here. */
int GiuliaGPU::gmuPreconditions(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    if(!self)return 0;
    if(self->mode<3 || self->firmwarePowerState!=GGPH_PREPARED)return 0;
    if(!self->hostPrepared || !self->firmwareVerified)return 0;
    if(!self->gmuEnsureWindow())return 0;
    if(!self->smmuActive || self->gmuFaulted)return 0;
    return 1;
}
/* The ownership proof is the plan rebuilt from the pinned pack: allocation i
 * always belongs to plan entry i, so the GMU IOVA of every non-TCM block must
 * land inside its own reservation and already contain the exact payload. */
int GiuliaGPU::gmuRamMatches(void *cookie,uint32_t address,const uint8_t *data,size_t bytes)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    struct gg_fw_block *blocks;
    struct gg_reservation *plan;
    struct gg_pack pack;
    size_t count=0,reservations=0;
    int matched=0;
    if(!self || !data || !bytes || !self->firmwareMap)return 0;
    blocks=(struct gg_fw_block *)IOMalloc(sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    plan=(struct gg_reservation *)IOMalloc(sizeof(*plan)*GG_FW_MAX_BLOCKS);
    if(blocks && plan &&
       gg_pack_open((const void *)self->firmwareMap->getVirtualAddress(),GG_FIRMWARE_PACK_BYTES,&pack) &&
       gg_fw_plan(pack.asset[0].data,pack.asset[0].size,blocks,GG_FW_MAX_BLOCKS,&count)==GG_FW_OK &&
       gg_memory_plan(blocks,count,plan,GG_FW_MAX_BLOCKS,&reservations)==GG_FW_OK) {
        for(size_t i=0;i<reservations && i<GG_FW_MAX_BLOCKS;i++) {
            const struct gg_reservation *r=&plan[i];
            uint8_t *cpu;
            if(address<r->iova || bytes>r->bytes || address-r->iova>r->bytes-bytes)continue;
            if(!self->allocations[i])break;
            cpu=(uint8_t *)self->allocations[i]->getBytesNoCopy();
            if(!cpu)break;
            matched=memcmp(cpu+(address-r->iova),data,bytes)==0;
            break;
        }
    }
    if(blocks)IOFree(blocks,sizeof(*blocks)*GG_FW_MAX_BLOCKS);
    if(plan)IOFree(plan,sizeof(*plan)*GG_FW_MAX_BLOCKS);
    return matched;
}
struct ggb_config GiuliaGPU::gmuConfigValues()
{
    struct ggb_config c;
    c.hfi_iova=GGH_IOVA;c.vrb_iova=GGH_VRB_IOVA;
    c.log_iova=GGH_LOG_IOVA;c.log_bytes=GGH_LOG_BYTES;
    /* ADRENO_GMU_REV(ADRENO_REV(7,9,1,0)) for qcom,adreno-gpu-gen7-9-1,
     * written to GENERAL_10 before reset release as the vendor does. */
    c.chip_rev=0x07090100u;
    return c;
}
int GiuliaGPU::gmuConfigureFn(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    struct gg_gmu_ops ops;
    struct ggb_config config;
    if(!self)return GGM_ARGUMENT;
    ops=self->gmuOps();config=self->gmuConfigValues();
    return ggb_configure(&ops,&self->gmuState,&config);
}
int GiuliaGPU::gmuHfiFn(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    struct gg_gmu_ops ops;
    struct ggb_config config;
    if(!self)return GGM_ARGUMENT;
    ops=self->gmuOps();config=self->gmuConfigValues();
    return ggb_prepare_hfi(&ops,&self->gmuState,&config);
}
int GiuliaGPU::gmuHealthyFn(void *cookie)
{
    GiuliaGPU *self=(GiuliaGPU *)cookie;
    return self && self->gmuWindowMapped && !self->gmuFaulted ? 1 : 0;
}
struct gg_gmu_ops GiuliaGPU::gmuOps()
{
    struct gg_gmu_ops ops={this,gmuReadWord,gmuWriteWord,gmuBarrierFn,gmuWaitFn,
        gmuPreconditions,gmuRamMatches,gmuConfigureFn,gmuHfiFn,gmuHealthyFn,
        giulia_gpu_log_event};
    return ops;
}
void GiuliaGPU::gmuReport()
{
    IOLog("GIULIA GPU GMU: power_prepared=%u smmu_active=%u window=%u loaded=%u started=%u reset_released=%u fw_ready=%u hfi_ready=%u writes=%u phase=%s error=%d quarantine=%u load_calls=%u start_calls=%u\n",
        firmwarePowerState==GGPH_PREPARED,smmuActive,gmuWindowMapped,
        gmuState.loaded,gmuActive,gmuState.reset_released,gmuState.fw_ready,
        gmuState.hfi_ready,gmuState.writes,gg_phase_name(GGD_GMU,gmuState.phase),
        (int)gmuState.error,gmuState.quarantined,gmuLoadCalls,gmuStartCalls);
}
int GiuliaGPU::gmuCommand(unsigned action)
{
    struct gg_gmu_ops ops=gmuOps();
    if(action==0) { gmuReport(); return 0; }
    if(action==1) {
        struct gg_pack pack;struct gg_fw_block *scratch;int result;
        if(!firmwareMap || !firmwareVerified || !hostPrepared) {
            IOLog("GIULIA GPU GMU: command=load rejected reason=firmware-or-memory-not-prepared\n");
            return 1;
        }
        if(firmwarePowerState!=GGPH_PREPARED || mode<3) {
            IOLog("GIULIA GPU GMU: command=load rejected reason=power-not-prepared prepared=%u mode=%u\n",
                firmwarePowerState==GGPH_PREPARED,mode);
            return 1;
        }
        if(!smmuActive) {
            IOLog("GIULIA GPU GMU: command=load rejected reason=smmu-inactive (SID 5 translation is a prerequisite)\n");
            return 1;
        }
        if(!gg_pack_open((const void *)firmwareMap->getVirtualAddress(),GG_FIRMWARE_PACK_BYTES,&pack)) {
            IOLog("GIULIA GPU GMU: command=load rejected reason=pack-open\n");
            return 1;
        }
        scratch=(struct gg_fw_block *)IOMalloc(sizeof(*scratch)*64);
        if(!scratch) {
            IOLog("GIULIA GPU GMU: command=load rejected reason=scratch-allocation\n");
            return 1;
        }
        result=giulia_gpu_gmu_load(&ops,&gmuState,pack.asset[0].data,
            (size_t)pack.asset[0].size,scratch,64);
        IOFree(scratch,sizeof(*scratch)*64);
        if(result)gmuFaulted=true;
        gmuLoadCalls++;
        gmuReport();
        IOLog("GIULIA GPU GMU: command=load result=%d\n",result);
        return result?1:0;
    }
    if(action==2) {
        int result;
        if(!gmuState.loaded || gmuState.quarantined) {
            IOLog("GIULIA GPU GMU: command=start rejected reason=not-loaded-or-quarantined loaded=%u quarantine=%u\n",
                gmuState.loaded,gmuState.quarantined);
            return 1;
        }
        result=giulia_gpu_gmu_start(&ops,&gmuState);
        gmuStartCalls++;
        if(result)gmuFaulted=true;
        else gmuActive=true;
        gmuReport();
        IOLog("GIULIA GPU GMU: command=start result=%d gmu_started=%u acceleration=0\n",
            result,gmuActive);
        return result?1:0;
    }
    return 1;
}
#endif
