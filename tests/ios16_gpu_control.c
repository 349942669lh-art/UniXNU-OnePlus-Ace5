/* Host fixtures only. No phone, device MMIO or firmware execution. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ios16/gpu/giulia_gpu_control.h"
#include "../ios16/gpu/giulia_gpu_memory.h"

static unsigned cases;
static void trace(void *cookie,const struct gg_event *event) {(void)cookie;assert(event->sequence && event->subsystem && event->operation);}
static void example(const char *name,const struct gg_diagnostics *d)
{
    printf("HOST FIXTURE: %s (not hardware evidence)\n",name);
    assert(d->failure.sequence || d->cleanup_failure.sequence);
    if(d->failure.sequence) giulia_gpu_log_event(NULL,&d->failure);
    if(d->cleanup_failure.sequence) giulia_gpu_log_event(NULL,&d->cleanup_failure);
}

struct power_peer {unsigned fail,uncertain,release_fail,releases,acquires,ready,stopped;uint32_t held;};
static int resolve(void *p) {(void)p;return 0;}
static int pquiet(void *p) {return ((struct power_peer *)p)->stopped?1:0;}
static enum gg_resource_result acquire(void *p,unsigned id,uint32_t value,uint64_t *token)
{
    struct power_peer *f=p; ++f->acquires;
    assert(value==(id==GGR_GMU_RATE?625000000u:id==GGR_HUB_RATE?200000000u:1));
    if(f->fail==id+1) return f->uncertain?GGRC_UNCERTAIN:GGRC_UNCHANGED_ERROR;
    f->held|=1u<<id; *token=id+100;return GGRC_OK;
}
static int release(void *p,unsigned id,uint64_t token)
{
    struct power_peer *f=p;assert(token==id+100 && (f->held&(1u<<id)));
    ++f->releases;if(f->release_fail==id+1)return 91;
    /* No lower parent is dropped while a higher resource is still held. */
    assert(!(f->held & ~((1u<<(id+1))-1)));f->held&=~(1u<<id);return 0;
}
static int observe(void *p) {return ((struct power_peer *)p)->ready?1:0;}
static int delay(void *p,unsigned us) {(void)p;assert(us==10);return 0;}
static void power_tests(void)
{
    unsigned i;
    for(i=0;i<=GGR_COUNT;++i) {
        struct power_peer f={0};struct gg_power_state s={0};
        struct gg_power_ops o={&f,resolve,pquiet,acquire,release,observe,delay,trace};
        f.fail=i;f.ready=f.stopped=1;
        assert(gg_power_acquire(&o,&s)==(i?GGC_PROVIDER:GGC_OK));
        if(!i) {assert(s.ready && s.held==0x3ff);assert(!gg_power_release(&o,&s));}
        else {assert(s.diag.failure.phase==i-1 && s.diag.failure.operation==GGD_ACQUIRE);}
        assert(!s.held && !f.held && !s.quarantined);++cases;
    }
    for(i=1;i<=GGR_COUNT;++i) {
        struct power_peer f={0};struct gg_power_state s={0};
        struct gg_power_ops o={&f,resolve,pquiet,acquire,release,observe,delay,trace};
        f.fail=i;f.uncertain=1;f.ready=f.stopped=1;
        assert(gg_power_acquire(&o,&s)==GGC_PROVIDER && s.quarantined && !f.releases);
        assert(gg_power_release(&o,&s)==GGC_BUSY && !f.releases);++cases;
    }
    {
        struct power_peer f={0};struct gg_power_state s={0};
        struct gg_power_ops o={&f,resolve,pquiet,acquire,release,observe,delay,trace};
        f.stopped=1;f.release_fail=GGR_HUB_CLK+1;
        assert(gg_power_acquire(&o,&s)==GGC_TIMEOUT);
        assert(s.quarantined && s.held && s.diag.failure.operation==GGD_POLL);
        assert(s.diag.cleanup_failure.error==GGC_RELEASE && s.diag.cleanup_failure.native_error==91 && s.diag.cleanup_failure.phase==GGR_HUB_CLK);
        example("power readiness timeout, followed by failed hub-clock release",&s.diag);++cases;
    }
}

struct smmu_peer {uint32_t mem[0x40000/4];unsigned calls,fail,after,writes,leased,published,quiet,corrupt;};
static int sevent(struct smmu_peer *f) {return ++f->calls==f->fail;}
static int sr32(void *p,size_t at,uint32_t *v) {struct smmu_peer *f=p;assert(at<=0x3fffc && !(at&3));if(sevent(f))return 71;*v=f->mem[at/4];return 0;}
static int sr64(void *p,size_t at,uint64_t *v) {struct smmu_peer *f=p;assert(at<=0x3fff8 && !(at&7));if(sevent(f))return 72;memcpy(v,f->mem+at/4,8);return 0;}
static int sw32(void *p,size_t at,uint32_t v) {
    struct smmu_peer *f=p;int e=sevent(f);assert(at<=0x3fffc && !(at&3));++f->writes;
    assert(at!=0 && at!=0x800 && at!=0xc00); /* Unrelated SID 9 is preserved. */
    assert((at>=0x22000 && at<0x23000) || at==0x1008 || at==0x1808 || at==0x808 || at==0xc08);
    if(e && !f->after)return 73;
    if(at==0x22058) f->mem[at/4]&=~v;
    else f->mem[at/4]=v;
    if(f->corrupt && at==0x22030)f->mem[at/4]^=1;
    return e?73:0;
}
static int sw64(void *p,size_t at,uint64_t v) {struct smmu_peer *f=p;int e=sevent(f);++f->writes;assert(at>=0x22000 && at<0x23000);if(e && !f->after)return 74;memcpy(f->mem+at/4,&v,8);return e?74:0;}
static int sbar(void *p) {return sevent(p)?75:0;}
static int lease(void *p,unsigned bank,unsigned asid,unsigned slot) {struct smmu_peer *f=p;assert(bank==2 && asid==7 && slot==2);f->leased=1;return 0;}
static int squiet(void *p) {return ((struct smmu_peer *)p)->quiet?1:0;}
static int publish(void *p,const ArmSmmu4KConfig *c,const ArmSmmu4KResult *r) {struct smmu_peer *f=p;assert(c->table_physical==r->root_physical);f->published=1;return 0;}
static int unlease(void *p) {struct smmu_peer *f=p;f->leased=0;return 0;}
static void smmu_reset(struct smmu_peer *f) {
    unsigned i;memset(f,0,sizeof(*f));f->quiet=1;
    f->mem[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    f->mem[0x24/4]=0x40000010;f->mem[0x28/4]=0x1433;f->mem[0]=0x436;
    for(i=0;i<8;++i)f->mem[(0xc00+4*i)/4]=0x20000;
    f->mem[0x800/4]=0x80000009;f->mem[0xc00/4]=3;
}
static void smmu_tests(void)
{
    static uint64_t tables[16*512];
    ArmSmmu4KConfig cfg={32,36,0x10000000,1024};
    ArmSmmu4KRange ranges[2]={{0x4000,0x20000000,8192,15,0,2},{0x60000000,0x30000000,8192,11,0,2}};
    ArmSmmu4KResult built;ArmSmmu4KContextConfig c={0};
    struct smmu_peer *f=malloc(sizeof(*f));unsigned count,i,after;
    struct gg_smmu_ops o={{f,0x40000,sr32,sr64,sw32,sw64,sbar},lease,squiet,publish,unlease,trace};
    struct gg_smmu_state s={0};assert(f);
    assert(!ArmSmmu4KBuild(&cfg,ranges,2,tables,16,&built));
    c.lower=&cfg;c.lower_tables=&built;c.asid=7;c.continue_after_fault=1;
    smmu_reset(f);
    {int e=gg_smmu_start(&o,&s,&c,2,2);if(e) {
        fprintf(stderr,"SMMU start: code=%d phase=%u context_phase=%u route_phase=%u\n",e,s.phase,s.context.phase,s.route.phase);
        giulia_gpu_log_event(NULL,s.diag.failure.sequence?&s.diag.failure:&s.diag.last);
    } assert(!e);}
    count=f->calls;
    assert(s.active && s.retain_resources && f->published);
    assert(f->mem[0x808/4]==0x80000005 && f->mem[0xc08/4]==2);
    assert(!gg_smmu_stop(&o,&s,&c) && !s.retain_resources && !f->leased);
    assert(f->mem[0xc08/4]==0x20000 && !f->mem[0x22000/4]);++cases;
    for(after=0;after<2;++after)for(i=1;i<=count;++i) {
        smmu_reset(f);memset(&s,0,sizeof(s));f->fail=i;f->after=after;
        assert(gg_smmu_start(&o,&s,&c,2,2)!=ArmSmmu4KSuccess);
        assert(f->mem[0x800/4]==0x80000009 && f->mem[0xc00/4]==3);
        assert(s.diag.failure.sequence && s.diag.failure.valid&GGD_ADDRESS ?
            s.diag.failure.address>=0x03da0000ull : 1);
        if(f->writes) assert(s.retain_resources && s.quarantined && f->leased);
        ++cases;
    }
    smmu_reset(f);memset(&s,0,sizeof(s));f->corrupt=1;
    assert(gg_smmu_start(&o,&s,&c,2,2)!=0 && s.quarantined);
    assert(s.diag.failure.address==0x03dc2030 && (s.diag.failure.valid&7)==7 &&
        s.diag.failure.expected!=s.diag.failure.observed);
    example("SMMU TCR write/readback mismatch; resources quarantined",&s.diag);++cases;
    smmu_reset(f);memset(&s,0,sizeof(s));f->mem[0x804/4]=0x80000005;
    assert(gg_smmu_start(&o,&s,&c,2,2)!=0 && !f->writes && !f->leased);++cases;
    free(f);
}

struct gmu_peer {uint32_t regs[0x20000];unsigned calls,fail,after,writes,timeout,stale,prereq,cleanup_fail;};
static int gevent(struct gmu_peer *f) {return ++f->calls==f->fail;}
static int gr(void *p,uint32_t reg,uint32_t *v) {struct gmu_peer *f=p;assert(reg<0x20000);if(gevent(f))return 81;*v=f->regs[reg];return 0;}
static int gw(void *p,uint32_t reg,uint32_t v) {
    struct gmu_peer *f=p;int e=gevent(f);++f->writes;assert(reg<0x20000);
    assert((reg>=GGM_ITCM && reg<GGM_DTCM+0x1000) || reg==GGM_RESET || reg==GGM_FW_READY || reg==GGM_HFI_INIT);
    if(reg==GGM_RESET && v==1 && f->cleanup_fail)return 89;
    if(e && !f->after)return 82;
    f->regs[reg]=v;
    if(reg==GGM_RESET && !v && f->timeout!=1) f->regs[GGM_FW_READY]=0x100;
    if(reg==GGM_HFI_INIT && v && f->timeout!=2)f->regs[GGM_HFI_STATUS]=1;
    if(reg==GGM_FW_READY && !v && f->stale)f->regs[reg]=0x100;
    return e?82:0;
}
static int gb(void *p) {return gevent(p)?83:0;}
static int gpre(void *p) {return ((struct gmu_peer *)p)->prereq?1:0;}
static int gram(void *p,uint32_t a,const uint8_t *data,size_t n) {(void)p;assert(a==0x4000 && data && n==0x12ff0);return 1;}
static int configure(void *p) {struct gmu_peer *f=p;assert(f->regs[GGM_RESET]==1);return gevent(f)?84:0;}
static int hfi(void *p) {struct gmu_peer *f=p;assert(f->regs[GGM_FW_READY]==0x100);return gevent(f)?85:0;}
static int healthy(void *p) {(void)p;return 1;}
static void greset(struct gmu_peer *f) {memset(f,0,sizeof(*f));f->prereq=1;f->regs[GGM_RESET]=1;}
static uint64_t walk(const uint64_t *tables,const ArmSmmu4KResult *r,uint32_t va)
{
    size_t page=0;unsigned level;
    for(level=1;level<=3;++level) {
        unsigned shift=12+9*(3-level),mask=level==1?3:511;
        uint64_t entry=tables[page*512+((va>>shift)&mask)];
        if(!entry)return 0;
        assert((entry&3)==3);
        if(level==3)return entry;
        assert((entry&0x0000fffffffff000ull)>=r->root_physical);
        page=((entry&0x0000fffffffff000ull)-r->root_physical)/4096;
        assert(page<r->table_pages);
    }
    abort();
}
static void stock_tables(const struct gg_fw_block *blocks,size_t count)
{
    struct gg_reservation plan[64];size_t n,i;static uint64_t tables[16*512];
    ArmSmmu4KRange ranges[64];ArmSmmu4KResult result;ArmSmmu4KConfig cfg={32,36,0x10000000,1024};
    assert(gg_memory_plan(blocks,count,plan,64,&n)==GG_FW_OK && n==5);
    for(i=0;i<n;++i) {
        ranges[i]=(ArmSmmu4KRange){plan[i].iova,0x20000000ull+i*0x100000,plan[i].bytes,
            plan[i].region==GG_ICACHE?15:11,0,2};
    }
    assert(!ArmSmmu4KBuild(&cfg,ranges,n,tables,16,&result));
    assert(result.table_pages==6 && result.leaf_pages==104);
    for(i=0;i<n;++i) {
        uint32_t at;
        for(at=0;at<ranges[i].bytes;at+=4096) {
            uint64_t leaf=walk(tables,&result,(uint32_t)ranges[i].iova+at);
            assert((leaf&0x0000fffffffff000ull)==ranges[i].physical+at);
            assert(!(leaf&(1u<<6)) && !(leaf&(1u<<7))); /* Privileged RW. */
            assert(((leaf>>53)&3)==(plan[i].region==GG_ICACHE?0:3));
        }
        assert(!walk(tables,&result,(uint32_t)(ranges[i].iova+ranges[i].bytes)));
    }
    assert(!walk(tables,&result,0));
    puts("Stock GMU: independent walk verified 104 native4K leaves in 6 table pages; holes unmapped");++cases;
}
static void gmu_tests(const char *path)
{
    struct gmu_peer *f=malloc(sizeof(*f));struct gg_gmu_state s={0};unsigned i,after,n;
    struct gg_gmu_ops o={f,gr,gw,gb,delay,gpre,gram,configure,hfi,healthy,trace};
    assert(f);greset(f);s.loaded=1;s.retain_resources=1;
    assert(!gg_gmu_start(&o,&s) && s.hfi_ready);n=f->calls;++cases;
    for(after=0;after<2;++after)for(i=1;i<=n;++i) {
        greset(f);memset(&s,0,sizeof(s));s.loaded=1;s.retain_resources=1;f->fail=i;f->after=after;
        assert(gg_gmu_start(&o,&s)!=0 && s.quarantined && s.retain_resources && !s.hfi_ready);++cases;
    }
    for(i=1;i<=2;++i) {
        greset(f);memset(&s,0,sizeof(s));s.loaded=1;f->timeout=i;f->cleanup_fail=1;
        assert(gg_gmu_start(&o,&s)==GGM_TIMEOUT && s.quarantined && s.retain_resources);
        assert(s.diag.failure.operation==GGD_POLL && s.diag.failure.observed==0);
        assert(s.diag.cleanup_failure.sequence && s.diag.failure.sequence<s.diag.cleanup_failure.sequence);
        example(i==1?"GMU firmware-ready timeout and reset failure":"GMU HFI-ready timeout and reset failure",&s.diag);++cases;
    }
    greset(f);memset(&s,0,sizeof(s));s.loaded=1;f->stale=1;
    assert(gg_gmu_start(&o,&s)==GGM_READBACK && !s.reset_released);++cases;
    greset(f);memset(&s,0,sizeof(s));s.loaded=1;f->prereq=0;
    assert(gg_gmu_start(&o,&s)==GGM_PREREQUISITE && !f->calls && !f->writes);++cases;
    if(path) {
        FILE *file=fopen(path,"rb");uint8_t *bytes=malloc(GG_FIRMWARE_PACK_BYTES);
        struct gg_fw_block scratch[64];assert(file && bytes);
        assert(fread(bytes,1,GG_FIRMWARE_PACK_BYTES,file)==GG_FIRMWARE_PACK_BYTES);fclose(file);
        greset(f);memset(&s,0,sizeof(s));
        assert(!gg_gmu_load(&o,&s,bytes,GG_FIRMWARE_PACK_BYTES,scratch,64));n=f->calls;
        assert(s.loaded && f->regs[GGM_RESET]==1 && !s.reset_released);
        stock_tables(scratch,15);
        for(after=0;after<2;++after)for(i=1;i<=n;++i) {
            greset(f);memset(&s,0,sizeof(s));f->fail=i;f->after=after;
            assert(gg_gmu_load(&o,&s,bytes,GG_FIRMWARE_PACK_BYTES,scratch,64)!=0);
            if(f->writes) assert(s.quarantined && s.retain_resources);
            ++cases;
        }
        greset(f);memset(&s,0,sizeof(s));bytes[4096]^=1;
        assert(gg_gmu_load(&o,&s,bytes,GG_FIRMWARE_PACK_BYTES,scratch,64)==GGM_FIRMWARE && !f->calls);++cases;
        free(bytes);
    }
    free(f);
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    power_tests();smmu_tests();gmu_tests(argc==2?argv[1]:NULL);
    printf("GPU control offline scenarios: %u passed; no hardware backend invoked\n",cases);return 0;
}
