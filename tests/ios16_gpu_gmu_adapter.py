"""Syntax-check the GMU adapter against a minimal IOKit surface.

The class in this translation unit mirrors the members GiuliaGPU.cpp declares;
the function bodies and the register sequence are the real installed sources.
No IOKit call, MMIO access or kernel link is performed.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

STUB = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint64_t IOByteCount;
typedef uint32_t IOOptionBits;
enum { kIODirectionInOut = 3, kIODirectionIn = 1,
       kIOMapInhibitCache = 0x100, kIOMapReadOnly = 0x200 };
// IOKit exposes an integer IOVirtualAddress, not a pointer.
struct IOMemoryMap { uintptr_t getVirtualAddress(); };
struct IOBufferMemoryDescriptor {
    void *getBytesNoCopy();
};
struct IOMemoryDescriptor {
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t, uint64_t, uint32_t);
    IOMemoryMap *map(IOOptionBits);
};
void IODelay(unsigned);
void *IOMalloc(size_t);
void IOFree(void *, size_t);
void IOLog(const char *, ...);
#include "giulia_gpu_pack.h"
#include "giulia_gpu_memory.h"
#include "giulia_gpu_gmu_boot.h"
#include "giulia_gpu_hfi.h"
#include "giulia_platform_power.h"
#include "giulia_gpu_control.h"
#include "giulia_gpu_terminal.h"
struct GiuliaGPU {
    unsigned mode, reservationCount, firmwarePowerState;
    bool hostPrepared, firmwareVerified, smmuActive, gmuFaulted;
    bool gmuWindowMapped, gmuActive;
    struct gpp_report firmwarePower;
    struct gg_gmu_state gmuState;
    struct gg_gmu_state dummyState;


    IOMemoryDescriptor *gmuMemory;
    IOMemoryMap *gmuMap;
    IOMemoryMap *firmwareMap;
    IOBufferMemoryDescriptor *allocations[GG_FW_MAX_BLOCKS];
    unsigned gmuLoadCalls, gmuStartCalls, smmuAsid, smmuBank, smmuSlot;
    bool gmuEnsureWindow();
    struct ggb_config gmuConfigValues();
    struct gg_gmu_ops gmuOps();
    int gmuCommand(unsigned);
    void gmuReport();
    static int gmuReadWord(void *, uint32_t, uint32_t *);
    static int gmuWriteWord(void *, uint32_t, uint32_t);
    static int gmuBarrierFn(void *);
    static int gmuWaitFn(void *, unsigned);
    static int gmuPreconditions(void *);
    static int gmuRamMatches(void *, uint32_t, const uint8_t *, size_t);
    static int gmuConfigureFn(void *);
    static int gmuHfiFn(void *);
    static int gmuHealthyFn(void *);
};
#include "giulia_gpu_gmu_impl.h"
int main() { return 0; }
'''


def run() -> None:
    with tempfile.TemporaryDirectory(prefix='giulia-gpu-gmu-adapter-') as directory:
        source = Path(directory) / 'adapter.cpp'
        source.write_text(STUB, encoding='utf-8')
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsyntax-only', *include_flags(ROOT),
                        '-I', str(ROOT / 'ios16/vendor/ArmSmmu4K'),
                        str(source)], check=True)
        print('GPU GMU adapter: class wiring, vendor register sequence and ops '
              'callbacks compile against a minimal IOKit surface; nothing executed')


if __name__ == '__main__':
    run()
