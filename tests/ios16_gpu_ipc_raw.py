"""Execute raw terminal mapping policy in both compiled GPU configurations."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'ios16/terminal/giulia_mmio.inc').read_text().replace(
    '__asm__ volatile("dsb sy" ::: "memory")','__sync_synchronize()')
prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
#define PAGE_SIZE 16384
#define VM_WIMG_IO 0
static unsigned mapped,unmapped;
static int power_test;
static unsigned long gPhysBase=0x880000000ull,gPhysSize=0x100000000ull;
static char reply[1024];
static int terminal_printf(const char *format,...) {
    va_list args;va_start(args,format);int n=vsnprintf(reply,sizeof(reply),format,args);va_end(args);return n;
}
#define printf terminal_printf
static int gs_enabled(void){return 1;}
static void PE_parse_boot_argn(const char *name,int *value,size_t n) {
    assert(n==sizeof(*value));
    if(!strcmp(name,"giulia_mmio"))*value=1;
    else {assert(!strcmp(name,"giulia_gpu_power_test"));*value=power_test;}
}
vm_offset_t ml_io_map_unmappable(vm_offset_t pa,vm_size_t bytes,uint32_t attr) {
    assert(pa && bytes && attr==VM_WIMG_IO);++mapped;return (vm_offset_t)calloc(1,bytes);
}
void ml_io_unmap(vm_offset_t va,vm_size_t bytes){assert(va && bytes);++unmapped;free((void *)va);}
'''
checks=r'''
int main(void) {
    const char *blocked[]={"reg.map 0x400000 0x4000","reg.map 0x404000 0x4000",
        "reg.map 0x500000 0x4000","reg.map 0x53c000 0x4000",
        "reg.map 0x3fc000 0x8000","reg.map 0x53c000 0x8000"};
    for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);i++) {
        unsigned before=mapped;assert(gm_command(blocked[i]));
#if GIULIA_GPU_INCLUDED
        assert(mapped==before && strstr(reply,"disabled for reserved IPC_CORE window"));
        assert(!gm_address(0x40600c,32));
#else
        assert(mapped==before+1);assert(gm_command("reg.unmap 0"));
#endif
    }
    assert(gm_command("reg.map 0x540000 0x4000") && gm_maps[0].physical==0x540000);
    assert(gm_command("reg.unmap 0"));
    assert(gm_command("reg.map 0x3d00000 0x4000"));
    assert(gm_command("reg.write 0x3d00000 32 0xabc"));
    assert(gm_command("reg.read 0x3d00000 32") && strstr(reply,"value=0xabc"));
    const char *isolated[]={"reg.map 0x3d04000 0x4000","reg.maps","reg.unmap 0",
        "reg.read 0x3d00000 32","reg.write 0x3d00000 32 0x123",
        "reg.rmw 0x3d00000 32 0xff 0x12","reg.dump 0x3d00000 32 4"};
    vm_offset_t original=gm_maps[0].virtual;unsigned before=mapped;
    for(int flag=-1;flag<=2;flag++)if(flag) {
        power_test=flag;
        for(unsigned i=0;i<sizeof(isolated)/sizeof(isolated[0]);i++) {
            assert(gm_command(isolated[i]) && strstr(reply,"disabled during GPU power-only test"));
            assert(gm_maps[0].virtual==original && *(uint32_t *)original==0xabc);
            assert(mapped==before && unmapped+1==mapped);
        }
    }
    power_test=0;
    assert(gm_command("reg.unmap 0"));assert(mapped==unmapped);
    puts("Raw MMIO: power-only flag blocks every register command before map/read/write, including an existing RAM alias");
    puts("Raw MMIO: reserved IPC_CORE aliases rejected in GPU profile; other windows and standalone profile preserved");
}
'''
with tempfile.TemporaryDirectory(prefix='gpu-ipc-raw-') as directory:
    folder=Path(directory);path=folder/'raw.c';path.write_text(prelude+source+checks)
    for gpu in (0,1):
        binary=folder/f'raw-{gpu}'
        subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror',
                        '-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all',
                        f'-DGIULIA_GPU_INCLUDED={gpu}',str(path),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
