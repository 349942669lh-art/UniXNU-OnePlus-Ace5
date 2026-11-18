/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Isolated power-only entry/exit on GiuliaGPU's existing worker. These methods
 * cannot bootstrap a native source or infer a grant from the boot report.
 */
#ifndef GIULIA_GPU_POWER_OWNER_IMPL_H
#define GIULIA_GPU_POWER_OWNER_IMPL_H
void GiuliaGPU::powerOnlyStart()
{
    powerOnlyCode=GGPO_HANDOFF;
    if(bootGunyahCaptured)
        powerOnlyCode=(unsigned)giulia_gpu_power_owner_acquire(this,bootGunyah.epoch);
    powerOnlyAttached=powerOnlyCode==GGPO_OK || powerOnlyCode==GGPO_LOST ||
        powerOnlyCode==GGPO_QUARANTINED;
    IOLog("GIULIA GPU POWER OWNER: start code=%u power_only=1 smmu_start=0 gmu_start=0 submit=0 acceleration=0\n",powerOnlyCode);
    powerOnlyReport();
}
void GiuliaGPU::powerOnlyStop()
{
    struct ggpo_status status={};
    if(powerOnlyAttached && giulia_gpu_power_owner_status(&status)==GGPO_OK) {
        powerOnlyCode=(unsigned)giulia_gpu_power_owner_release(this);
        if(powerOnlyCode==GGPO_OK)powerOnlyAttached=false;
        (void)giulia_gpu_power_owner_status(&status);
    }
    if(powerOnlyAttached || status.quarantined || status.grant_held || status.power_held)
        manualMemoryPinned=true;
    IOLog("GIULIA GPU POWER OWNER: stop code=%u retained=%u acceleration=0\n",powerOnlyCode,manualMemoryPinned);
}
void GiuliaGPU::powerOnlyReport()
{
    struct ggpo_status status={};
    int code=giulia_gpu_power_owner_status(&status);
    IOLog("GIULIA GPU POWER OWNER: abi=1 registered=%u attached=%u grant_held=%u ready_cached=%u revoked=%u quarantine=%u code=%u status_code=%d\n",
        status.registered,powerOnlyAttached,status.grant_held,status.ready,status.revoked,status.quarantined,powerOnlyCode,code);
    IOLog("GIULIA GPU POWER OWNER: epoch=%#llx serial=%#llx held=%#x mutation_attempted=%u cached=1 hardware_proof=not-in-status\n",
        (unsigned long long)status.epoch,(unsigned long long)status.serial,
        status.power_held,status.mutation_attempted);
    IOLog("GIULIA GPU: power_acquired=%u smmu_active=0 gmu_started=0 hardware_ready=0 acceleration=0 device_mutation_attempted=%u\n",
        powerOnlyAttached && status.ready,status.mutation_attempted);
    IOLog("GIULIA GPU: runtime_lease_abi=1 native_submit_abi=1 runtime_provider_bound=%u submit_provider_bound=0\n",powerOnlyAttached);
}
#endif
