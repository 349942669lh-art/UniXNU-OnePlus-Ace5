"""Cross-compile the native fence CPU owner without executing IOKit or pmap.

Only Apple-style declarations are stubs; the C++ bodies, the C header contracts
and the exported C linkage are the real sources installed by the GPU patch.
This is not a full XNU build, does not link IOKit and never maps memory.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

IOKIT_STUBS = {
    'IOKit/IOLib.h': '#pragma once\n#include <stdint.h>\n',
    'IOKit/IOMemoryDescriptor.h': '''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint64_t addr64_t;
typedef uint64_t IOByteCount;
typedef uint32_t IOOptionBits;
enum { kIODirectionInOut = 3 };
enum { kIOMapAnywhere = 0x00000001, kIOMapInhibitCache = 0x00000100 };
extern void *kernel_task;
struct OSObject { void retain(void); void release(void); };
struct IOMemoryMap : OSObject { uintptr_t getVirtualAddress(void); };
struct IOMemoryDescriptor : OSObject {
    static IOMemoryDescriptor *withPhysicalAddress(addr64_t, IOByteCount, unsigned);
    IOMemoryMap *createMappingInTask(void *, uint64_t, IOOptionBits, IOByteCount, IOByteCount);
};
''',
}

PLATFORM_STUBS = {
    'pexpert/arm64/board_config.h': '#define ARM64_BOARD_CONFIG_SWEET 1\n',
    'sys/param.h': '#include <stdint.h>\n#include <stddef.h>\n',
    'sys/systm.h': '',
}


def run() -> None:
    with tempfile.TemporaryDirectory(prefix='giulia-fence-native-build-') as directory:
        work = Path(directory)
        stub = work / 'stub'
        for group in (IOKIT_STUBS, PLATFORM_STUBS):
            for name, text in group.items():
                target = stub / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding='utf-8')
        source = ROOT / 'ios16/gpu/giulia_gpu_fence_native.c'
        backend = ROOT / 'ios16/gpu/giulia_gpu_fence_iokit.cpp'
        common = ['-Wall', '-Wextra', '-Werror', '-O2', '-I', str(stub),
                  *include_flags(ROOT)]
        darwin = work / 'native-darwin.s'
        subprocess.run(['clang++', '--target=arm64-apple-macos', '-ffreestanding',
                        '-std=c++17', *common, '-S', str(backend),
                        '-o', str(darwin)], check=True)
        assembly = darwin.read_text(encoding='utf-8')
        for name in ('map', 'audit'):
            assert re.search(r'^_giulia_gpu_fence_native_' + name + r':', assembly, re.M), name
        assert re.search(r'\bbl\s+_giulia_gpu_fence_cpu_audit\b', assembly), \
            'native audit must call the pmap gate'
        # Match XNU's language selection from the emitted source suffixes.
        darwin_c = work / 'native-darwin-c.s'
        subprocess.run(['clang', '--target=arm64-apple-macos', '-ffreestanding',
                        '-std=c11', *common, '-S', str(source),
                        '-o', str(darwin_c)], check=True)
        c_text = darwin_c.read_text(encoding='utf-8')
        for name in ('ggfn_import', 'ggfn_get', 'ggfn_status', 'ggfn_invalidate'):
            assert re.search(r'^_' + name + r':', c_text, re.M), name
        assert '_giulia_gpu_fence_native_map:' not in c_text
        freestanding = work / 'native-aarch64.s'
        subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding',
                        '-std=c11', *common, '-DGG_FENCE_NATIVE_STANDALONE', '-S',
                        str(source), '-o', str(freestanding)], check=True)
        text = freestanding.read_text(encoding='utf-8')
        for name in ('ggfn_import', 'ggfn_get', 'ggfn_status', 'ggfn_invalidate'):
            assert re.search(r'^' + name + r':', text, re.M), name
        from ios16_gpu_patch import gpu_patch
        gpu_patch(work, ROOT)
        patch = (work / '0077-giulia-gpu-runtime.patch').read_text()
        for unit in (source, backend):
            assert '+iokit/Kernel/arm/' + unit.name + ' optional iokitcpp\n' in patch
            assert '+++ b/iokit/Kernel/arm/' + unit.name + '\n' in patch
        print('Native fence CPU owner: Darwin C++ IOKit map/audit exports and '
              'AArch64 standalone state machine compiled; nothing executed')


if __name__ == '__main__':
    run()
