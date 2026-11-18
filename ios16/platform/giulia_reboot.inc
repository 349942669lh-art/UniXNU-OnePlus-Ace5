/* Debug-only Fastboot request. Disk synchronization uses XNU's normal shutdown
 * path; the final PSCI call is hooked after that path, before host_reboot(). */
#include <sys/reboot.h>
/* GIULIA_REBOOT_CORE is embedded here by the profile generator. */
static struct {
    vm_offset_t core,owner,observer,writer;
    struct grb_state state;
    thread_t reboot_thread;
    unsigned pending,reset_armed,read_probe_complete;
} gr_runtime;
static uint32_t gr_read(void *ctx,unsigned space,unsigned offset)
{
    (void)ctx;
    vm_offset_t base=space==GRB_CORE?gr_runtime.core:space==GRB_OWNER?gr_runtime.owner:
        space==GRB_OBSERVER?gr_runtime.observer:gr_runtime.writer;
    uint32_t value=*(volatile uint32_t *)(base+offset);
    __asm__ volatile("dsb sy" ::: "memory");return value;
}
static void gr_write(void *ctx,unsigned space,unsigned offset,uint32_t value)
{
    (void)ctx;vm_offset_t base=space==GRB_OBSERVER?gr_runtime.observer:gr_runtime.writer;
    *(volatile uint32_t *)(base+offset)=value;__asm__ volatile("dsb sy" ::: "memory");
}
static void gr_delay(void *ctx,unsigned us){(void)ctx;delay(us);}
static int gr_map_channel(void *ctx,unsigned apid)
{
    (void)ctx;
    gr_runtime.observer=ml_io_map(0x0c440000+0x20*apid,0x20);
    gr_runtime.writer=ml_io_map(0x0c500000+0x1000*apid,0x1000);
    return gr_runtime.observer && gr_runtime.writer;
}
static const struct grb_io gr_io={NULL,gr_read,gr_write,gr_delay,gr_map_channel};
static int64_t gr_smc(uint64_t fn,uint64_t argument)
{
    register uint64_t x0 __asm__("x0")=fn,x1 __asm__("x1")=argument;
    register uint64_t x2 __asm__("x2")=0,x3 __asm__("x3")=0;
    __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3) : :
        "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
    return (int64_t)(int32_t)(uint32_t)x0; /* PSCI SMC32 return value */
}
static void gr_sleep(unsigned ms)
{
    uint64_t deadline;clock_interval_to_deadline(ms,NSEC_PER_MSEC,&deadline);
    assert_wait_deadline((event_t)&gr_runtime,THREAD_UNINT,deadline);
    thread_block(THREAD_CONTINUE_NULL);
}
/* Published by the root-selection hook after its UFS read probe returns. */
void giulia_reboot_read_probe_complete(void);
void giulia_reboot_read_probe_complete(void)
{
    __atomic_store_n(&gr_runtime.read_probe_complete,1u,__ATOMIC_RELEASE);
}
static int gr_readonly_probe(void)
{
    int probe=0,writable=0,mode=0,host_only=0,mmio=0,power_test=0;
    PE_parse_boot_argn("giulia_ufs_probe",&probe,sizeof(probe));
    PE_parse_boot_argn("vermeer_ufs_rw",&writable,sizeof(writable));
    PE_parse_boot_argn("giulia_gpu",&mode,sizeof(mode));
    PE_parse_boot_argn("giulia_gpu_hostonly",&host_only,sizeof(host_only));
    PE_parse_boot_argn("giulia_mmio",&mmio,sizeof(mmio));
    PE_parse_boot_argn("giulia_gpu_power_test",&power_test,sizeof(power_test));
    return grb_readonly_probe_allowed(
        __atomic_load_n(&gr_runtime.read_probe_complete,__ATOMIC_ACQUIRE),
        probe,writable,mode,host_only,mmio,power_test);
}
static int gr_reset(const char *disks)
{
    printf("GIULIA REBOOT: resetting target=fastboot disks=%s\n",disks);
    gr_sleep(500); /* CDC stays on its own worker while the acknowledgement drains. */
    __asm__ volatile("dsb sy\nisb" ::: "memory");
    int64_t result=gr_smc(0x84000009u,0); /* PSCI SYSTEM_RESET, same as stock cold restart */
    printf("GIULIA REBOOT: PSCI SYSTEM_RESET returned=%lld\n",(long long)result);
    __atomic_store_n(&gr_runtime.reset_armed,0u,__ATOMIC_RELEASE);
    return EIO; /* Never silently fall through to a different reboot target. */
}
int giulia_reboot_finish(void);
int giulia_reboot_finish(void)
{
    if(!__atomic_load_n(&gr_runtime.reset_armed,__ATOMIC_ACQUIRE) ||
       current_thread()!=gr_runtime.reboot_thread)return 0;
    return gr_reset("shutdown-complete");
}
static void gr_worker(void *unused,wait_result_t wait)
{
    (void)unused;(void)wait;
    gr_runtime.reboot_thread=current_thread();
    int error=0,read_probe=0,diagnostic=gr_readonly_probe();
    int64_t version=0,feature=-1;
    PE_parse_boot_argn("giulia_ufs_probe",&read_probe,sizeof(read_probe));
    if(read_probe==1 && !diagnostic) {
        printf("GIULIA REBOOT: read-only reset refused (probe incomplete or profile unsupported)\n");
        goto out;
    }
    version=gr_smc(0x84000000u,0);
    if(version>=0x10000)feature=gr_smc(0x8400000au,0x84000009u);
    if(version<0x10000 || feature<0) {
        printf("GIULIA REBOOT: unsupported PSCI version=%lld reset_feature=%lld\n",(long long)version,(long long)feature);
        goto out;
    }
    if(!gr_runtime.core)gr_runtime.core=ml_io_map(0x0c400000,0x4000);
    if(!gr_runtime.owner)gr_runtime.owner=ml_io_map(0x0c42d000,0x1000);
    if(!gr_runtime.core || !gr_runtime.owner) {printf("GIULIA REBOOT: register map failed\n");goto out;}
    error=grb_prepare(&gr_io,&gr_runtime.state);
    printf("GIULIA REBOOT: prepare result=%d version=0x%x apid=%u before=0x%x expected=0x%x observed=0x%x status=0x%x changed=%u\n",
        error,gr_runtime.state.version,gr_runtime.state.apid,gr_runtime.state.before,
        gr_runtime.state.expected,gr_runtime.state.observed,gr_runtime.state.status,gr_runtime.state.changed);
    if(error)goto out;
    __atomic_store_n(&gr_runtime.reset_armed,1u,__ATOMIC_RELEASE);
    gr_sleep(500);
    /* The completed read-only diagnostic parks BSD root selection. Keep its
     * scheduler and CDC alive through acknowledgement and the global reset. */
    if(diagnostic)error=gr_reset("read-only-probe");
    else error=reboot_kernel(RB_AUTOBOOT,NULL);
    __atomic_store_n(&gr_runtime.reset_armed,0u,__ATOMIC_RELEASE);
    printf("GIULIA REBOOT: shutdown returned=%d reason_restore=%d\n",error,grb_restore(&gr_io,&gr_runtime.state));
out:
    __atomic_store_n(&gr_runtime.pending,0u,__ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
static int gr_command(const char *cmd)
{
    if(strcmp(cmd,"reboot.fastboot") && strcmp(cmd,"reboot.status"))return 0;
    if(!gs_enabled()) {printf("GIULIA REBOOT: requires giulia_debug=1\n");return 1;}
    if(!strcmp(cmd,"reboot.status")) {
        printf("GIULIA REBOOT: pending=%u armed=%u read_probe_complete=%u\n",
            __atomic_load_n(&gr_runtime.pending,__ATOMIC_ACQUIRE),
            __atomic_load_n(&gr_runtime.reset_armed,__ATOMIC_ACQUIRE),
            __atomic_load_n(&gr_runtime.read_probe_complete,__ATOMIC_ACQUIRE));return 1;
    }
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&gr_runtime.pending,&expected,1u,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
        printf("GIULIA REBOOT: request already pending\n");return 1;
    }
    thread_t thread=THREAD_NULL;
    if(kernel_thread_start(gr_worker,NULL,&thread)!=KERN_SUCCESS) {
        __atomic_store_n(&gr_runtime.pending,0u,__ATOMIC_RELEASE);
        printf("GIULIA REBOOT: worker creation failed\n");return 1;
    }
    thread_deallocate(thread);
    printf("GIULIA REBOOT: accepted target=fastboot\n");return 1;
}
