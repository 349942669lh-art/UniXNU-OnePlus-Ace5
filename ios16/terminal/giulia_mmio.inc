/* Explicit development MMIO. Never automatically probe an address window.
 * The operator owns power/clock sequencing and conflicts with active drivers.
 * An unpowered/secure bus access can abort or hang; mapping is not a probe.
 */
/* These Mach-private entry points are linked into the same kernel. BSD does
 * not receive their declarations through its exported machine header. */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,uint32_t);
extern void ml_io_unmap(vm_offset_t,vm_size_t);
static struct { uint64_t physical,bytes; vm_offset_t virtual; } gm_maps[16];
static int gm_number(const char **cursor,uint64_t *value)
{
    const char *p=*cursor;uint64_t n=0;unsigned base=10,digits=0;
    while(*p==' ')p++;
    if(p[0]=='0' && (p[1]=='x'||p[1]=='X')) {base=16;p+=2;}
    while(*p && *p!=' ') {
        unsigned d=*p>='0'&&*p<='9'?(unsigned)(*p-'0'):
            *p>='a'&&*p<='f'?(unsigned)(*p-'a'+10):
            *p>='A'&&*p<='F'?(unsigned)(*p-'A'+10):99;
        if(d>=base || n>(UINT64_MAX-d)/base)return 0;
        n=n*base+d;p++;digits++;
    }
    if(!digits)return 0;
    *value=n;*cursor=p;return 1;
}
static vm_offset_t gm_address(uint64_t address,unsigned width)
{
    unsigned bytes=width/8;
    if((width!=8 && width!=16 && width!=32 && width!=64) || address%bytes)return 0;
    for(unsigned i=0;i<16;i++)if(gm_maps[i].virtual && address>=gm_maps[i].physical &&
        address-gm_maps[i].physical<=gm_maps[i].bytes-bytes)
        return gm_maps[i].virtual+(vm_offset_t)(address-gm_maps[i].physical);
    return 0;
}
static uint64_t gm_read(vm_offset_t address,unsigned width)
{
    uint64_t value;
    __asm__ volatile("dsb sy" ::: "memory");
    if(width==8)value=*(volatile uint8_t *)address;
    else if(width==16)value=*(volatile uint16_t *)address;
    else if(width==32)value=*(volatile uint32_t *)address;
    else value=*(volatile uint64_t *)address;
    __asm__ volatile("dsb sy" ::: "memory");
    return value;
}
static void gm_write(vm_offset_t address,unsigned width,uint64_t value)
{
    __asm__ volatile("dsb sy" ::: "memory");
    if(width==8)*(volatile uint8_t *)address=(uint8_t)value;
    else if(width==16)*(volatile uint16_t *)address=(uint16_t)value;
    else if(width==32)*(volatile uint32_t *)address=(uint32_t)value;
    else *(volatile uint64_t *)address=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int gm_command(const char *cmd)
{
    if(strncmp(cmd,"reg.",4))return 0;
    /* Enforce isolation even if GPU matching fails or giulia_mmio=1 was
     * inherited from a debug profile. No raw alias may race the native worker. */
    int power_test=0;PE_parse_boot_argn("giulia_gpu_power_test",&power_test,sizeof(power_test));
    if(power_test) {
        printf("GIULIA REG: disabled during GPU power-only test\n");return 1;
    }
    int enabled=0;PE_parse_boot_argn("giulia_mmio",&enabled,sizeof(enabled));
    if(!gs_enabled() || enabled!=1) {
        printf("GIULIA REG: disabled; requires giulia_debug=1 giulia_mmio=1\n");return 1;
    }
    if(!strcmp(cmd,"reg.maps")) {
        for(unsigned i=0;i<16;i++)if(gm_maps[i].virtual)
            printf("GIULIA REG: slot=%u base=%#llx bytes=%#llx accessibility=unchecked\n",i,gm_maps[i].physical,gm_maps[i].bytes);
        return 1;
    }
    const char *p=strchr(cmd,' ');uint64_t a=0,b=0,c=0,d=0;
    if(!p || !gm_number(&p,&a))goto syntax;
    if(!strncmp(cmd,"reg.unmap ",10)) {
        while(*p==' ')p++;
        if(*p || a>=16 || !gm_maps[a].virtual)goto syntax;
        ml_io_unmap(gm_maps[a].virtual,(vm_size_t)gm_maps[a].bytes);
        bzero(&gm_maps[a],sizeof(gm_maps[a]));
        printf("GIULIA REG: unmapped slot=%llu\n",a);return 1;
    }
    if(!gm_number(&p,&b))goto syntax;
    if(!strncmp(cmd,"reg.map ",8)) {
        while(*p==' ')p++;
        /* SM8650 peripheral aperture only; reject DRAM, wrap and overlarge maps.
         * Still includes secure/unpowered regions: operator must consult DTS.
         */
        if(*p || a<0x100000 || a>=0x80000000 || !b || b>0x10000 ||
           a%PAGE_SIZE || b%PAGE_SIZE || b>0x80000000-a ||
           (gPhysSize && a<gPhysBase+gPhysSize && a+b>gPhysBase))goto syntax;
#if GIULIA_GPU_INCLUDED
        /* The fixed GMU IPC aperture overlaps APSS host doorbells. A raw
         * alias would bypass the GPU/Wi-Fi/audio mapping-lifetime broker. */
        if(a<0x540000 && a+b>0x400000) {
            printf("GIULIA REG: disabled for reserved IPC_CORE window; use gpu.dev resource provider\n");return 1;
        }
#endif
        unsigned slot=16;
        for(unsigned i=0;i<16;i++) {
            if(!gm_maps[i].virtual) { if(slot==16)slot=i;continue; }
            if(a<gm_maps[i].physical+gm_maps[i].bytes && a+b>gm_maps[i].physical) {
                printf("GIULIA REG: overlapping mapping; use reg.maps\n");return 1;
            }
        }
        if(slot==16) {printf("GIULIA REG: map table full\n");return 1;}
        vm_offset_t va=ml_io_map_unmappable((vm_offset_t)a,(vm_size_t)b,VM_WIMG_IO);
        if(!va) {printf("GIULIA REG: map allocation failed\n");return 1;}
        gm_maps[slot].physical=a;gm_maps[slot].bytes=b;gm_maps[slot].virtual=va;
        printf("GIULIA REG: mapped slot=%u base=%#llx bytes=%#llx accessibility=unchecked no_device_access_yet\n",slot,a,b);
        return 1;
    }
    if(b!=8 && b!=16 && b!=32 && b!=64)goto syntax;
    vm_offset_t address=gm_address(a,(unsigned)b);
    if(!address) {printf("GIULIA REG: unmapped or misaligned address\n");return 1;}
    if(!strncmp(cmd,"reg.read ",9)) {
        while(*p==' ')p++;if(*p)goto syntax;
        c=gm_read(address,(unsigned)b);
        printf("GIULIA REG: read address=%#llx width=%llu value=%#llx\n",a,b,c);return 1;
    }
    if(!gm_number(&p,&c))goto syntax;
    if(!strncmp(cmd,"reg.dump ",9)) {
        while(*p==' ')p++;
        if(*p || !c || c>64 || a>UINT64_MAX-(c-1)*(b/8) || !gm_address(a+(c-1)*(b/8),(unsigned)b))goto syntax;
        for(uint64_t i=0;i<c;i++)if(!gm_address(a+i*(b/8),(unsigned)b))goto syntax;
        for(uint64_t i=0;i<c;i++) {
            vm_offset_t va=gm_address(a+i*(b/8),(unsigned)b);
            printf("GIULIA REG: read address=%#llx width=%llu value=%#llx\n",a+i*(b/8),b,gm_read(va,(unsigned)b));
        }
        return 1;
    }
    if(b<64 && c>>b)goto syntax;
    if(!strncmp(cmd,"reg.write ",10)) {
        while(*p==' ')p++;if(*p)goto syntax;
        gm_write(address,(unsigned)b,c);
        /* No implicit readback: write-only and W1C registers must not be read. */
        printf("GIULIA REG: write-issued address=%#llx width=%llu value=%#llx readback=not-performed\n",a,b,c);return 1;
    }
    if(!strncmp(cmd,"reg.rmw ",8)) {
        if(!gm_number(&p,&d) || (b<64 && d>>b) || (d&~c))goto syntax;
        while(*p==' ')p++;if(*p)goto syntax;
        uint64_t old=gm_read(address,(unsigned)b),value=(old&~c)|d;
        gm_write(address,(unsigned)b,value);
        printf("GIULIA REG: rmw-issued address=%#llx old=%#llx mask=%#llx value=%#llx atomic_with_driver=0\n",a,old,c,value);return 1;
    }
syntax:
    printf("GIULIA REG: invalid arguments; map BASE BYTES | maps | unmap SLOT | read ADDR WIDTH | write ADDR WIDTH VALUE | rmw ADDR WIDTH MASK BITS | dump ADDR WIDTH COUNT (prefix reg.)\n");
    return 1;
}
