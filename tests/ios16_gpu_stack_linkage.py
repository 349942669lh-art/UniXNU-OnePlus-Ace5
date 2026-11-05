"""Link actual C stack exports from C++, as the native IOService does.

The host executes ordinary RAM state transitions only. Darwin cross-compilation
checks the caller's C ABI without requiring an SDK or running target code.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_sources import include_flags

CALLER='''#include "giulia_gpu_stack.h"
int main(void) {
    struct ggsk s={};
    struct ggsk_policy policy={};
    giulia_gpu_stack_init(&s,&policy);
    giulia_gpu_stack_register(&s,GGSK_BOOT,0);
    giulia_gpu_stack_plan(&s);
    giulia_gpu_stack_advance(&s,GGSK_BOOT);
    giulia_gpu_stack_retreat(&s);
    giulia_gpu_stack_log(&s);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='gpu-stack-linkage-') as directory:
    work=Path(directory);caller=work/'caller.cpp';caller.write_text(CALLER)
    obj=work/'stack.o';exe=work/'stack-linkage';assembly=work/'caller-darwin.s'
    common=['-Wall','-Wextra','-Werror',*include_flags(ROOT)]
    sanitizer=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CC','cc'),'-std=c11',*common,*sanitizer,
        '-DGG_HOST_TEST','-c',str(ROOT/'ios16/gpu/giulia_gpu_stack.c'),'-o',str(obj)],check=True)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17',*common,*sanitizer,
        str(caller),str(obj),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['clang++','--target=arm64-apple-darwin22','-ffreestanding',
        '-std=c++17',*common,'-O0','-S',str(caller),'-o',str(assembly)],check=True)
    text=assembly.read_text()
    for name in ('init','register','plan','advance','retreat','log'):
        assert re.search(r'\bbl\s+_giulia_gpu_stack_'+name+r'\b',text),name
    assert not re.search(r'\bbl\s+__Z[^\s]*giulia_gpu_stack_',text)
print('Actual C stack exports link and run from C++; all six Darwin caller symbols use C linkage (RAM only)')
