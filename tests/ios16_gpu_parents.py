"""Execute actual GCC branch core/native code with explicitly RAM-only doubles."""
from pathlib import Path
import json
import re
import os
import subprocess
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import include_flags, kernel_source
manifest = json.loads((ROOT / 'hardware/gpu-gcc-parents-source.json').read_text())
reg = manifest['register']
assert reg['gcc_base'] + reg['offset'] == reg['physical_page'] == 0x152000
assert reg['map_bytes'] == 4096 and reg['page_offset'] == 0 and reg['width_bits'] == 32
assert [b['bit'] for b in manifest['branches']] == [15, 16]
assert [b['required_hz'] for b in manifest['branches']] == [600000000, 300000000]
assert manifest['settle'] == {'policy': 'BRANCH_HALT_DELAY', 'microseconds': 10, 'halt_bit_polling': False}
assert not manifest['production_platform_authority_supplied'] and not manifest['hardware_validated']
for path in (ROOT / 'ios16').rglob('*'):
    if path.suffix not in ('.c', '.cpp', '.h', '.inc') or 'parents' in path.name:
        continue
    assert not re.search(r'\bgiulia_gpu_parents_(open|acquire|release|initialize)\s*\(', path.read_text()), path
with tempfile.TemporaryDirectory(prefix='gpu-parents-') as folder:
    folder = Path(folder)
    flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g', *include_flags(ROOT)]
    core = folder / 'parents.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-DGG_HOST_TEST',
                    '-c', str(ROOT / 'ios16/gpu/giulia_gpu_parents.c'), '-o', str(core)], check=True)
    for name, language, compiler in (('parents', 'c11', os.environ.get('CC', 'cc')),
                                     ('parents_native', 'c++17', os.environ.get('CXX', 'c++'))):
        binary = folder / name
        source = ROOT / ('tests/ios16_gpu_' + name + ('.cpp' if language=='c++17' else '.c'))
        subprocess.run([compiler, '-std='+language, *flags, '-pthread', str(source), str(core),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=45)
    # Staged XNU headers omit stdint.h: check their fallback constants with
    # only the kernel's integer typedefs, not accidental libc macro support.
    header = kernel_source((ROOT / 'ios16/gpu/giulia_gpu_parents.h').read_text())
    staged = folder / 'staged.cpp'
    staged.write_text('typedef unsigned long long uint64_t; typedef unsigned int uint32_t;\n' + header +
                      '\nstatic_assert(GGCP_PAGE==0x152000 && UINT32_MAX==0xffffffffu, "constants");\n')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsyntax-only', str(staged)], check=True)
print('GCC GPU parents: staged header constants checked; no hardware/provider registration')
# ISA check only: declaration-only IOKit doubles are NOT native XNU headers.
# Compile the original production accessors (not the host barrier branch).
with tempfile.TemporaryDirectory(prefix='gpu-parents-isa-') as folder:
    folder = Path(folder)
    definitions = '''#pragma once
#include <stdint.h>
#include <stddef.h>
struct IOLock;
IOLock *IOLockAlloc(); void IOLockFree(IOLock *); void IOLockLock(IOLock *); void IOLockUnlock(IOLock *);
struct IOService {void retain(); void release();};
struct IOMemoryMap {uintptr_t getVirtualAddress(); uint64_t getLength(); void release();};
struct IOMemoryDescriptor {static IOMemoryDescriptor *withPhysicalAddress(uint64_t,uint64_t,unsigned);
IOMemoryMap *map(unsigned); void release();};
enum { kIODirectionInOut=3, kIOMapInhibitCache=0x100 };
void IODelay(unsigned); void IOLog(const char *,...);
'''
    (folder / 'fixture.h').write_text(definitions)
    for header in ('IOService.h', 'IOLib.h', 'IOMemoryDescriptor.h'):
        path = folder / 'IOKit' / header
        path.parent.mkdir(exist_ok=True)
        path.write_text('#include "fixture.h"\n')
    path = folder / 'pexpert/arm64/board_config.h'
    path.parent.mkdir(parents=True)
    path.write_text('#define ARM64_BOARD_CONFIG_SWEET 1\n')
    assembly = folder / 'native.s'
    subprocess.run([os.environ.get('ISA_CC', 'clang++'), '--target=arm64-apple-macos',
                    '-ffreestanding', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(folder), '-S', str(ROOT / 'ios16/gpu/giulia_gpu_parents_native.cpp'),
                    '-o', str(assembly)], check=True)
    text = assembly.read_text()
    assert re.search(r'\bdsb\s+sy\b', text)
    # Inspect the accessor functions, not unrelated pointer-width loads/stores.
    for name, instruction in (('parentRead', 'ldr'), ('parentWrite', 'str')):
        functions = [chunk for chunk in text.split('\n\t.p2align')
                     if re.search(r'^__ZL\d+' + name + r'[^\n]*:', chunk, re.M)]
        assert len(functions) == 1, name
        assert re.search(r'\b' + instruction + r'\s+w\d+,\s*\[', functions[0]), name
        assert re.search(r'\bdsb\s+sy\b', functions[0]), name
print('GCC GPU parents ISA: original ARM64 accessors emit 32-bit LDR/STR and DSB SY; declaration doubles only, not an XNU build')
