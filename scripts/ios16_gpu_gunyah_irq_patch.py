"""Generate the actual single-CPU GIC polling reservation hooks.

Input must be the three real XNU sources AFTER the pinned Sweet platform
patches. Every anchor is checked; missing or duplicate anchors fail closed.
The GPU service patch must also install giulia_gpu_gunyah_irq.h at
iokit/Kernel/arm/giulia_gpu_gunyah_irq.h. This module does not modify sources.
"""
from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import subprocess
import tempfile


PATHS = ('pexpert/arm/pe_fiq.c', 'osfmk/arm64/sleh.c',
         'osfmk/arm64/machine_routines.c')
# Static 0085 provenance: Apple XNU 1b191cb58250d0705d8a51287127505aa4bc0789,
# with 0004, 0008 and 0017 from the generated giulia GPU platform profile.
# The profile is derived from reference-vermeer d68e05df683f863f054b0f9c063c54d2b1342b2a.
XNU_COMMIT = '1b191cb58250d0705d8a51287127505aa4bc0789'
REFERENCE_COMMIT = 'd68e05df683f863f054b0f9c063c54d2b1342b2a'


def _replace(text: str, old: str, new: str, name: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f'Gunyah GIC source drift in {name}: expected one anchor, got {text.count(old)}')
    return text.replace(old, new)


_GLUE = r'''

#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
/* Source baseline: XNU 1b191cb58250d0705d8a51287127505aa4bc0789,
 * post-Sweet 0004/0008/0017 from reference-vermeer
 * d68e05df683f863f054b0f9c063c54d2b1342b2a, prepared for giulia.
 */
/* Actual platform owner for the current single-CPU giulia profile. Static
 * storage, reservations and quarantined state remain pinned until reboot.
 * Existing generic interrupt handlers prevent claiming. Later handler
 * registration revokes this lease before publishing the new handler.
 */
static struct ggirq_state giulia_gunyah_irq_state;
static uint32_t giulia_gunyah_gic_read(void *cookie, unsigned offset)
{ (void)cookie; return gicd_read32(offset); }
static void giulia_gunyah_gic_write(void *cookie, unsigned offset, uint32_t value)
{ (void)cookie; gicd_write32(offset, value); }
static void giulia_gunyah_gic_barrier(void *cookie)
{ (void)cookie; __asm__ volatile("dsb sy" ::: "memory"); }
static uint64_t giulia_gunyah_gic_now(void *cookie)
{
	uint64_t ns;
	(void)cookie; absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}
static int giulia_gunyah_gic_uncontended(void *cookie)
{
	(void)cookie;
	return giulia_gunyah_gic_initialized && gicd_base &&
	    giulia_gunyah_gic_bytes >= GGIRQ_REG_BYTES &&
	    giulia_gpu_gunyah_irq_cpu_uncontended();
}
static struct ggirq_io giulia_gunyah_gic_io(void)
{
	struct ggirq_io io = { NULL, (size_t)giulia_gunyah_gic_bytes,
	    giulia_gunyah_gic_read, giulia_gunyah_gic_write,
	    giulia_gunyah_gic_barrier, giulia_gunyah_gic_now,
	    giulia_gunyah_gic_uncontended };
	return io;
}
int giulia_gpu_gunyah_irq_claim(const void *owner, uint64_t epoch, uint32_t tx, uint32_t rx)
{
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	int valid = ggirq_claim(&io, &giulia_gunyah_irq_state, owner, epoch, tx, rx) == GGIRQ_OK;
	(void)ml_set_interrupts_enabled(previous);
	return valid;
}
int giulia_gpu_gunyah_irq_owns(const void *owner, uint64_t epoch, uint32_t tx, uint32_t rx)
{
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	int valid = ggirq_owns(&io, &giulia_gunyah_irq_state, owner, epoch, tx, rx);
	(void)ml_set_interrupts_enabled(previous);
	return valid;
}
void giulia_gpu_gunyah_irq_invalidate(const void *owner, uint64_t epoch)
{
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	if (owner && giulia_gunyah_irq_state.owner == owner && giulia_gunyah_irq_state.epoch == epoch)
		ggirq_invalidate(&io, &giulia_gunyah_irq_state);
	(void)ml_set_interrupts_enabled(previous);
}
int giulia_gpu_gunyah_irq_guard_begin(const void *owner, uint64_t epoch,
    uint32_t tx, uint32_t rx, unsigned *saved)
{
	if (!saved) return 0;
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	if (!ggirq_owns(&io, &giulia_gunyah_irq_state, owner, epoch, tx, rx)) {
		(void)ml_set_interrupts_enabled(previous); return 0;
	}
	*saved = !!previous;
	return 1; /* Keep interrupts disabled across one non-sleeping actual HVC. */
}
int giulia_gpu_gunyah_irq_guard_end(const void *owner, uint64_t epoch,
    uint32_t tx, uint32_t rx, unsigned saved)
{
	boolean_t unexpected = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	int valid = !unexpected && saved <= 1 &&
	    ggirq_owns(&io, &giulia_gunyah_irq_state, owner, epoch, tx, rx);
	if (!valid && giulia_gunyah_irq_state.owner == owner)
		ggirq_invalidate(&io, &giulia_gunyah_irq_state);
	(void)ml_set_interrupts_enabled(saved == 1);
	return valid;
}
int giulia_gpu_gunyah_irq_dispatch(uint32_t intid)
{
	struct ggirq_io io = giulia_gunyah_gic_io();
	return ggirq_dispatch(&io, &giulia_gunyah_irq_state, intid);
}
void giulia_gpu_gunyah_irq_handler_registered(void)
{
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	struct ggirq_io io = giulia_gunyah_gic_io();
	ggirq_invalidate(&io, &giulia_gunyah_irq_state);
	(void)ml_set_interrupts_enabled(previous);
}
void giulia_gpu_gunyah_irq_controller_changed(void)
{
	boolean_t previous = ml_set_interrupts_enabled(FALSE);
	giulia_gunyah_gic_initialized = 0;
	giulia_gpu_gunyah_irq_handler_registered();
	(void)ml_set_interrupts_enabled(previous);
}
#endif
'''

_CPU_GLUE = r'''

#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
/* CPU-private state stays in osfmk, where cpu_data_internal.h is available.
 * The caller holds local interrupts disabled throughout the reservation check. */
int giulia_gpu_gunyah_irq_cpu_uncontended(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	return topology && topology->num_cpus == 1 &&
	    getCpuDatap() == &BootCpuData && !ml_at_interrupt_context() &&
	    !getCpuDatap()->interrupt_handler;
}
#endif
'''


def gunyah_irq_sources(source_texts: dict[str, str]) -> dict[str, str]:
    """Return changed source text; refuse an unpatched/different platform."""
    for name in PATHS:
        if name not in source_texts:
            raise ValueError(f'Missing actual XNU source: {name}')
        if 'giulia_gpu_gunyah_irq_' in source_texts[name]:
            raise ValueError(f'Gunyah GIC hook already present: {name}')
    changed = {name: source_texts[name] for name in PATHS}
    name = PATHS[0]
    text = changed[name]
    anchor = '#include <pexpert/arm64/board_config.h>\n'
    text = _replace(text, anchor, anchor + '''#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
#include "../../iokit/Kernel/arm/giulia_gpu_gunyah_irq.h"
static unsigned giulia_gunyah_gic_initialized;
static uint64_t giulia_gunyah_gic_bytes;
#endif
''', name)
    anchor = 'pe_init_fiq()\n{\n\tint error;'
    text = _replace(text, anchor, '''pe_init_fiq()
{
#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
	giulia_gpu_gunyah_irq_controller_changed();
#endif
	int error;''', name)
    anchor = '\t\tgicr_size = gicr_size_prop;'
    text = _replace(text, anchor, anchor + '''
#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
		giulia_gunyah_gic_bytes = gicd_size_prop;
#endif''', name)
    anchor = '\t__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);\n\t__builtin_arm_isb(ISB_SY);'
    text = _replace(text, anchor, anchor + '\n\tgiulia_gunyah_gic_initialized = 1;', name)
    changed[name] = text + _GLUE

    name = PATHS[1]
    anchor = '#include <pexpert/arm64/board_config.h>\n'
    changed[name] = _replace(changed[name], anchor, anchor + '''#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
#include "../../iokit/Kernel/arm/giulia_gpu_gunyah_irq.h"
#endif
''', name)
    # This is the Sweet Group1 branch; leave its EOI in place and never run
    # a second consumer if a reserved IRQ escaped the mask.
    anchor = '\t\tif (cdp->interrupt_handler) {'
    changed[name] = _replace(changed[name], anchor,
        '\t\tif (!giulia_gpu_gunyah_irq_dispatch(intid) && cdp->interrupt_handler) {', name)

    name = PATHS[2]
    anchor = '#include <arm/cpu_data_internal.h>\n'
    changed[name] = _replace(changed[name], anchor, anchor + '''#include <pexpert/arm64/board_config.h>
#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
#include "../../iokit/Kernel/arm/giulia_gpu_gunyah_irq.h"
#endif
''', name)
    anchor = '\tcurrent_state = ml_set_interrupts_enabled(FALSE);\n\tcpu_data_ptr = getCpuDatap();'
    changed[name] = _replace(changed[name], anchor, anchor + '''
#if defined(ARM64_BOARD_CONFIG_SWEET) && HAS_GIC_V3
	/* Revoke a polling lease before publishing any new generic controller
	 * handler. Preserve the existing registration semantics for other IRQs. */
	giulia_gpu_gunyah_irq_handler_registered();
#endif''', name)
    changed[name] += _CPU_GLUE
    return changed


def gunyah_irq_patch(source_texts: dict[str, str]) -> str:
    changed = gunyah_irq_sources(source_texts)
    return ''.join(''.join(difflib.unified_diff(source_texts[name].splitlines(True),
        changed[name].splitlines(True), fromfile='a/' + name, tofile='b/' + name))
        for name in PATHS)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True,
                        help='Actual XNU tree with the Sweet patches already applied')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--check-static', type=Path,
                        help='Require exact generator/static patch equality and git apply --check')
    args = parser.parse_args()
    if not args.out and not args.check_static:
        parser.error('Specify --out or --check-static')
    source_texts = {name: (args.source / name).read_text(encoding='utf-8') for name in PATHS}
    patch = gunyah_irq_patch(source_texts)
    if args.check_static:
        if args.check_static.read_text(encoding='utf-8') != patch:
            raise ValueError('Static Gunyah GIC patch differs from the actual-source generator')
        # read_text uses universal newlines. Check that same actual source in
        # a disposable LF tree, so Windows CRLF/global autocrlf settings cannot
        # change patch semantics or modify the caller's files/configuration.
        with tempfile.TemporaryDirectory(prefix='giulia-gunyah-irq-patch-') as directory:
            work = Path(directory)
            for name in PATHS:
                target = work / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(source_texts[name], encoding='utf-8', newline='\n')
            checked = work / 'irq.patch'
            checked.write_text(patch, encoding='utf-8', newline='\n')
            subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply', '--check', str(checked)],
                           cwd=work, check=True)
        print('Gunyah GIC static patch matches the actual-source generator and LF git apply --check')
    if args.out:
        args.out.write_text(patch, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
