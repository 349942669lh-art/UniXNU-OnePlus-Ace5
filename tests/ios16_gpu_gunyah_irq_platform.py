"""Exercise the actual generated GIC platform wrappers using a RAM-only shim.

The function bodies are generator._GLUE, not a second wrapper implementation.
Host verification substitutes only the AArch64 DSB instruction. Native codegen
uses that original DSB and minimal kernel-compatible declarations; this is not
a full XNU compilation, target execution or proof that a hardware lease exists.
"""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import include_flags
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_gunyah_irq_patch import _GLUE, _CPU_GLUE

DECLARATIONS = '''#include "giulia_gpu_gunyah_irq.h"
#define ARM64_BOARD_CONFIG_SWEET 1
#define HAS_GIC_V3 1
#define FALSE 0
typedef int boolean_t;
typedef struct { unsigned num_cpus; } ml_topology_info_t;
typedef struct { void *interrupt_handler; } cpu_data_t;
'''

NATIVE = DECLARATIONS + '''extern cpu_data_t BootCpuData;
extern cpu_data_t *getCpuDatap(void);
extern const ml_topology_info_t *ml_get_topology_info(void);
extern int ml_at_interrupt_context(void);
extern boolean_t ml_set_interrupts_enabled(boolean_t);
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t, uint64_t *);
extern uintptr_t gicd_base;
extern unsigned giulia_gunyah_gic_initialized;
extern uint64_t giulia_gunyah_gic_bytes;
static uint32_t gicd_read32(unsigned offset)
{return *(volatile uint32_t *)(gicd_base + offset);}
static void gicd_write32(unsigned offset, uint32_t value)
{*(volatile uint32_t *)(gicd_base + offset) = value;}
'''

HOST = DECLARATIONS + '''#include <assert.h>
#include <stdio.h>
#include <string.h>
static cpu_data_t BootCpuData, otherCpuData;
static ml_topology_info_t topology = {1};
static unsigned irq_enabled = 1, writes, boot_cpu = 1;
static uint32_t regs[256];
static uintptr_t gicd_base = 1;
static unsigned giulia_gunyah_gic_initialized = 1;
static uint64_t giulia_gunyah_gic_bytes = sizeof(regs);
static cpu_data_t *getCpuDatap(void) { return boot_cpu ? &BootCpuData : &otherCpuData; }
static const ml_topology_info_t *ml_get_topology_info(void) { return &topology; }
static int ml_at_interrupt_context(void) { return 0; }
static boolean_t ml_set_interrupts_enabled(boolean_t value)
{unsigned old = irq_enabled; irq_enabled = !!value; return old;}
static uint64_t mach_absolute_time(void) { return 0; }
static void absolutetime_to_nanoseconds(uint64_t value, uint64_t *out) { *out = value; }
static uint32_t gicd_read32(unsigned offset)
{assert(!irq_enabled && offset < sizeof(regs)); return regs[offset / 4];}
static void gicd_write32(unsigned offset, uint32_t value)
{
    assert(!irq_enabled && offset >= 0x180 && offset < 0x200 && !(offset & 3));
    assert(value && !(value & (value - 1)));
    ++writes; regs[(offset - 0x80) / 4] &= ~value;
}
'''

HOST_TEST = '''
static void fresh(void)
{
    /* Synthetic test boot only; native static state has no reset/release API. */
    memset(&giulia_gunyah_irq_state, 0, sizeof(giulia_gunyah_irq_state));
    memset(regs, 0, sizeof(regs)); regs[1] = 31;
    irq_enabled = boot_cpu = 1; writes = 0;
    BootCpuData.interrupt_handler = NULL;
    giulia_gunyah_gic_initialized = 1; topology.num_cpus = 1;
}
int main(void)
{
    static const unsigned owner = 1, other = 2;
    unsigned saved = 9;
    fresh();
    assert(giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77) == 1);
    assert(irq_enabled && writes == 2);
    /* A new pending RX event remains queued; ownership does not ACK it. */
    regs[(0x200 + 4 * (77 / 32)) / 4] |= 1u << (77 % 32);
    assert(giulia_gpu_gunyah_irq_owns(&owner, 17, 44, 77) == 1 && irq_enabled);
    assert(regs[(0x200 + 4 * (77 / 32)) / 4] & (1u << (77 % 32)));
    assert(!giulia_gpu_gunyah_irq_guard_begin(&other, 17, 44, 77, &saved));
    assert(saved == 9 && irq_enabled && !giulia_gunyah_irq_state.quarantined);
    assert(giulia_gpu_gunyah_irq_guard_begin(&owner, 17, 44, 77, &saved) == 1);
    assert(!irq_enabled && saved == 1);
    assert(giulia_gpu_gunyah_irq_owns(&owner, 17, 44, 77) == 1 && !irq_enabled);
    assert(giulia_gpu_gunyah_irq_guard_end(&owner, 17, 44, 77, saved) == 1 && irq_enabled);
    /* Restoring IRQs before guard_end permanently invalidates the lease. */
    assert(giulia_gpu_gunyah_irq_guard_begin(&owner, 17, 44, 77, &saved) == 1);
    irq_enabled = 1;
    assert(giulia_gpu_gunyah_irq_guard_end(&owner, 17, 44, 77, saved) == 0);
    assert(irq_enabled && giulia_gunyah_irq_state.quarantined);
    assert(!giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77));

    fresh(); BootCpuData.interrupt_handler = (void *)1;
    assert(!giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77) && irq_enabled && !writes);
    fresh(); topology.num_cpus = 2;
    assert(!giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77) && irq_enabled && !writes);
    fresh(); boot_cpu = 0;
    assert(!giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77) && irq_enabled && !writes);

    fresh(); assert(giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77));
    giulia_gpu_gunyah_irq_handler_registered();
    assert(irq_enabled && giulia_gunyah_irq_state.quarantined);
    fresh(); assert(giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77));
    giulia_gpu_gunyah_irq_controller_changed();
    assert(irq_enabled && !giulia_gunyah_gic_initialized && giulia_gunyah_irq_state.quarantined);
    fresh(); assert(giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77));
    giulia_gpu_gunyah_irq_invalidate(&other, 17);
    assert(!giulia_gunyah_irq_state.quarantined && irq_enabled);
    giulia_gpu_gunyah_irq_invalidate(&owner, 17);
    assert(giulia_gunyah_irq_state.quarantined && irq_enabled);

    fresh(); assert(giulia_gpu_gunyah_irq_claim(&owner, 17, 44, 77));
    irq_enabled = 0;
    assert(!giulia_gpu_gunyah_irq_dispatch(78) && !giulia_gunyah_irq_state.quarantined);
    assert(giulia_gpu_gunyah_irq_dispatch(77) == 1 && !irq_enabled);
    assert(giulia_gunyah_irq_state.quarantined);
    puts("Native Gunyah IRQ glue RAM shim: guarded GIC access, IRQ-state restoration, handler/CPU refusal and retained loss passed");
    return 0;
}
'''


def run() -> None:
    barrier = '__asm__ volatile("dsb sy" ::: "memory");'
    assert _GLUE.count(barrier) == 1, 'Review changed native barrier before updating the host shim'
    # Every GIC write in the RAM model rejects pending/active/EOI operations.
    assert 'ICC_IAR' not in _GLUE and 'ICC_EOIR' not in _GLUE
    host_glue = _GLUE.replace(barrier, '__asm__ volatile("" ::: "memory");')
    with tempfile.TemporaryDirectory(prefix='giulia-gunyah-irq-platform-') as directory:
        work = Path(directory)
        host = work / 'irq-host.c'
        native = work / 'irq-native.c'
        executable = work / 'irq-host'
        assembly = work / 'irq-native.s'
        host.write_text(HOST + _CPU_GLUE + host_glue + HOST_TEST, encoding='utf-8')
        native.write_text(NATIVE + _CPU_GLUE + _GLUE, encoding='utf-8')
        common = ['-std=c11', '-Wall', '-Wextra', '-Werror', *include_flags(ROOT)]
        subprocess.run([os.environ.get('CC', 'cc'), *common,
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g',
            str(host), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        subprocess.run(['clang', '--target=arm64-apple-darwin22', '-ffreestanding',
            *common, '-O2', '-S', str(native), '-o', str(assembly)], check=True)
        text = assembly.read_text(encoding='utf-8')
        assert re.search(r'\bdsb\s+sy\b', text), 'Native GIC barrier missing'
        assert re.search(r'\bstr\s+w\d+,', text), 'Native 32-bit register writes missing'
        for name in ('claim', 'owns', 'invalidate', 'guard_begin', 'guard_end',
                     'dispatch', 'handler_registered', 'controller_changed', 'cpu_uncontended'):
            assert re.search(r'^_giulia_gpu_gunyah_irq_' + name + r':', text, re.M)
        print('Native Gunyah IRQ glue emits all nine Darwin/AArch64 exports, 32-bit MMIO and DSB; minimal declarations, no hardware execution')


if __name__ == '__main__':
    run()
