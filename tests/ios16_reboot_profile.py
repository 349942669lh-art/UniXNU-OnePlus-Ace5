"""Compile the actual boot-profile reset decision with RAM-only boot arguments."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'ios16/platform/giulia_reboot.inc').read_text()
start = source.index('static int gr_readonly_probe(void)')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1

fixture = r'''
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "giulia_reboot.h"
static struct { unsigned read_probe_complete; } gr_runtime;
static struct { const char *name; int value; } arguments[] = {
    {"giulia_ufs_probe",1},{"vermeer_ufs_rw",0},{"giulia_gpu",3},
    {"giulia_gpu_hostonly",0},{"giulia_mmio",0},{"giulia_gpu_power_test",0},
    {"giulia_gpu_rm_diagnostic",1}
};
static int PE_parse_boot_argn(const char *name,void *out,size_t bytes)
{
    for(unsigned i=0;i<sizeof(arguments)/sizeof(arguments[0]);i++) {
        if(!strcmp(name,arguments[i].name) && bytes==sizeof(int)) {
            memcpy(out,&arguments[i].value,bytes);
            return 1;
        }
    }
    return 0;
}
'''
fixture += '\n' + source[start:end] + '\n'
fixture += r'''
int main(void)
{
    gr_runtime.read_probe_complete=1;
    if(gr_readonly_probe()!=1) {
        fputs("RM diagnostic reset rejected after completed read-only probe\n",stderr);
        return 1;
    }
    unsigned cases=0;
    for(int mode=0;mode<4;mode++)for(int host=0;host<3;host++)
    for(int rm=0;rm<3;rm++)for(int writable=0;writable<2;writable++)
    for(int mmio=0;mmio<2;mmio++)for(int power=0;power<2;power++)
    for(int completed=0;completed<2;completed++)for(int probe=0;probe<2;probe++) {
        arguments[0].value=probe;arguments[1].value=writable;
        arguments[2].value=mode;arguments[3].value=host;
        arguments[4].value=mmio;arguments[5].value=power;arguments[6].value=rm;
        gr_runtime.read_probe_complete=(unsigned)completed;
        int expected=completed==1 && probe==1 && !writable && !mmio && !power &&
            ((mode==1 && host==1 && rm==0) || (mode==3 && host==0 && rm==1));
        if(gr_readonly_probe()!=expected)return 2;
        ++cases;
    }
    printf("Actual reset profile decision: %u RAM cases passed\n",cases);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='giulia-reboot-profile-') as directory:
    folder = Path(directory)
    path = folder / 'profile.c'
    path.write_text(fixture)
    output = folder / 'profile'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-I', str(ROOT / 'ios16/platform'), str(path),
                    '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)
