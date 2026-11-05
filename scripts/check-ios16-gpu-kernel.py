"""Verify the linked A750 preparation service before local firmware embedding."""
import argparse
import json
import struct
from pathlib import Path


MARKERS = (
    b'GIULIA GPU: host preparation complete;',
    b'GIULIA GPU: firmware pack mapped read-only and digest verified',
    b'GIULIA FB: native panel 1264x2780 full coverage;',
    b'GiuliaGPU',
    b'GIULIA GPU: boot Gunyah resources',
    b'GIULIA GPU DEV: gunyah_owner_abi=1 current_boot_resources=1 guarded_hvc=1 fence_completion=0',
    b'cpu_cache=normal-copyback',
    b'GIULIA GPU: host RAM cache publication complete;',
    b'GIULIA GPU: runtime_lease_abi=1 native_submit_abi=1 runtime_provider_bound=0 submit_provider_bound=0',
    b'GIULIA GPU POWER OWNER: abi=1 registered=',
    b'GIULIA GPU POWER OWNER: start code=',
    b'GIULIA REG: disabled during GPU power-only test',
    b'GIULIA GPU HFI: table_iova=',
    b'GIULIA GPU HFI: query_ack=1',
    b'GIULIA GPU PLATFORM: abi=3',
    b'GIULIA GPU SECURE: pool_owned=1',
    b'GIULIA GPU WATCH: stage=doorbell_before header=',
    b'GIULIA GPU MAPPING: managed=1',
    b'GIULIA GPU MAPPING: audit_complete=1',
    b'GIULIA GPU DEV: resources_abi=1 memstore=1 secure_allocation_mappings=1',
    b'GIULIA GPU MEMSTORE: bytes=',
    b'GIULIA GPU MEMSTORE: context=',
    b'GIULIA GPU SECURE: audit_complete=1',
    b'GIULIA GPU DEV: ipc_core=1 ipc_host_arbitration=1 ipc_retained_until_reboot=1',
    b'GIULIA GPU IPC: bytes=',
    b'GIULIA GPU DEV: fence_import_abi=1 fence_completion=0',
    b'GIULIA GPU FENCE: unavailable reason=provider-not-registered',
    b'GIULIA GPU FENCE: bytes=',
    b'GIULIA GPU ZAP: pas=13 authenticated=1',
)
SYMBOLS = tuple('_giulia_gpu_' + name for name in (
    'power_acquire', 'power_release', 'smmu_start', 'smmu_stop',
    'gmu_load', 'gmu_start', 'log_event', 'terminal', 'cache_publish',
    'fence_register', 'fence_ipcc_claim', 'fence_ipcc_owns',
)) + (
      # QMP and IPCC arbitration belong to the shared platform broker. Require
      # its real executable definitions, including in GPU-only builds.
      '_giulia_platform_qmp_available',
      '_gpr_global_legacy_ipcc_claim','_gpr_global_legacy_ipcc_owns',
      '_gpr_global_legacy_ipcc_release',
      '_ggh_hvc_probe_compatibility','_ggh_hvc_valid','_ggh_hvc_send','_ggh_hvc_recv',
      '_ggh_hvc_doorbell_valid','_ggh_hvc_doorbell_read_clean',
      '_ggrs_init','_ggrs_init_hvc','_ggrs_probe','_ggrs_notification',
      '_ggrs_invalidate','_ggrs_copy_snapshot','_ggrs_poll',
      '_ggsh_init','_ggsh_run','_ggsh_invalidate',
      '_giulia_gpu_gunyah_irq_claim','_giulia_gpu_gunyah_irq_owns',
      '_giulia_gpu_gunyah_irq_guard_begin','_giulia_gpu_gunyah_irq_guard_end',
      '_giulia_gpu_gunyah_irq_invalidate','_giulia_gpu_gunyah_irq_dispatch',
      '_giulia_gpu_gunyah_irq_handler_registered','_giulia_gpu_gunyah_irq_controller_changed',
      '_ggown_init','_ggown_bootstrap','_ggown_probe','_ggown_poll',
      '_ggown_copy_snapshot','_ggown_get_status','_ggown_get_channel','_ggown_invalidate',
      '_giulia_gpu_runtime_acquire','_giulia_gpu_runtime_live',
      '_giulia_gpu_runtime_dma','_giulia_gpu_runtime_drain',
      '_giulia_gpu_runtime_release','_giulia_gpu_runtime_quarantine',
      '_giulia_gpu_runtime_reserve','_giulia_gpu_runtime_cancel',
      '_giulia_gpu_submit_encode','_giulia_gpu_submit_init','_giulia_gpu_submit',
      '_giulia_gpu_submit_poll','_giulia_gpu_submit_invalidate') + tuple(
      '_giulia_gpu_power_owner_' + name for name in (
          'initialize','register','acquire','release','invalidate','unregister','status'))


def linked_functions(kernel):
    count, command_bytes = struct.unpack_from('<II', kernel, 16)
    if count > 128 or command_bytes > len(kernel) - 32:
        raise ValueError('Invalid Mach-O command bounds')
    at, end, sections, symtab = 32, 32 + command_bytes, [], None
    for _ in range(count):
        if at + 8 > end:
            raise ValueError('Truncated Mach-O command')
        command, size = struct.unpack_from('<II', kernel, at)
        if size < 8 or size % 8 or size > end - at:
            raise ValueError('Invalid Mach-O command size')
        if command == 0x19:
            if size < 72:
                raise ValueError('Truncated segment')
            va, vm, offset, length, _, protection, nsects = struct.unpack_from('<QQQQiiI', kernel, at + 24)
            if size != 72 + 80 * nsects or va + vm > 2**64 or offset > len(kernel) or length > len(kernel) - offset:
                raise ValueError('Invalid segment bounds')
            for i in range(nsects):
                address, amount, file_offset = struct.unpack_from('<QQI', kernel, at + 72 + 80 * i + 32)
                flags = struct.unpack_from('<I', kernel, at + 72 + 80 * i + 64)[0]
                sections.append((address, amount, file_offset, flags, protection, va, vm, offset, length))
        elif command == 2:
            if size != 24 or symtab is not None:
                raise ValueError('Invalid or duplicate Mach-O symbol table')
            symtab = struct.unpack_from('<4I', kernel, at + 8)
        at += size
    if at != end or symtab is None:
        raise ValueError('Missing Mach-O symbol table')
    offset, count, strings, string_bytes = symtab
    if offset > len(kernel) or count > (len(kernel) - offset) // 16 or strings > len(kernel) or string_bytes > len(kernel) - strings:
        raise ValueError('Mach-O symbol table outside kernel')
    wanted, found = {name.encode() for name in SYMBOLS}, set()
    for i in range(count):
        index, kind, section, _, value = struct.unpack_from('<IBBHQ', kernel, offset + 16 * i)
        if index >= string_bytes:
            raise ValueError('Symbol name outside string table')
        stop = kernel.find(b'\0', strings + index, strings + string_bytes)
        if stop < 0:
            raise ValueError('Unterminated symbol name')
        name = bytes(kernel[strings + index:stop])
        if name not in wanted or kind & 0xe0 or kind & 0x0f != 0x0f:
            continue
        if not 1 <= section <= len(sections):
            raise ValueError('Invalid GPU symbol section')
        address, amount, file_offset, flags, protection, va, vm, segment_offset, length = sections[section - 1]
        # XNU uses __TEXT_EXEC,__text, which Darwin nm labels S rather than T.
        # Check the actual external N_SECT definition and its executable bytes.
        if (not protection & 4 or not flags & 0x80000400 or value & 3 or
                not address <= value or value - address + 4 > amount or
                not va <= address or address - va + amount > vm or
                file_offset != segment_offset + address - va or
                value - va + 4 > length):
            continue
        if name.decode() in found:
            raise ValueError('Ambiguous GPU function definition')
        found.add(name.decode())
    return found


def check_kernel(kernel):
    if len(kernel) < 32:
        raise ValueError('Truncated GPU kernel')
    if struct.unpack_from('<4I', kernel) != (0xfeedfacf, 0x100000c, 0, 2):
        raise ValueError('Expected ARM64 Mach-O executable')
    missing = [m.decode() for m in MARKERS if m not in kernel]
    defined = linked_functions(kernel)
    missing += [name for name in SYMBOLS if name not in defined]
    if missing:
        raise ValueError('GPU kernel lacks linked implementation: ' + ', '.join(missing))
    return {'chip_id': '0x43051401', 'linked_symbols': list(SYMBOLS),
            'firmware_embedded': False, 'hardware_verified': False,
            'acceleration': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('kernel', type=Path)
    a = p.parse_args()
    try:
        report = check_kernel(a.kernel.read_bytes())
    except (ValueError, OSError) as error:
        p.exit(1, str(error) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
