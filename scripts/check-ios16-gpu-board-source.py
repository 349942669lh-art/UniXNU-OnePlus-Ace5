"""Offline board-source blocker audit. NEVER authorizes a hardware image.

Code/host/link presence cannot prove firmware ownership, native mapping or
durable voltage votes. No report, force flag or user boolean is accepted as
authority. A real upstream and independent current-boot qualification remain
mandatory even if a future activation caller is found.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def audit(root=ROOT, kernel=None):
    names = ('ggbs_init', 'giulia_gpu_board_source_register')
    calls = {name: [] for name in names}
    composition = root / 'ios16/gpu/giulia_gpu_board_source.c'
    for path in sorted((root / 'ios16').rglob('*')):
        if path.suffix not in ('.c', '.cpp') or path == composition:
            continue
        # This is a conservative callsite inventory, NOT a C parser or proof
        # of the semantics/lifetime of any callbacks found.
        for no, line in enumerate(path.read_text().splitlines(), 1):
            for name in names:
                if re.search(r'\b' + name + r'\s*\(', line):
                    calls[name].append({'path': str(path.relative_to(root)), 'line': no})
    blockers = []
    if not all(calls.values()):
        blockers.append('No production upstream initialization/registration caller exists')
    blockers.extend([
        'Authoritative current-XNU-boot handoff/access and DMA/IRQ/SID ownership not verified',
        'Native voltage/CX-GDSC/GCC/CFG/ACD/bandwidth source and session guard not qualified',
        'Candidate-boot calibrated-standby precondition unverified; cold PLL calibration unsupported',
        'Retain-until-reset policy/backing and reset exit have not been qualified on hardware',
    ])
    linked = None
    if kernel is not None:
        try:
            spec = importlib.util.spec_from_file_location('gpu_board_link_check', root / 'scripts/check-ios16-gpu-kernel.py')
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            result = mod.check_kernel(kernel.read_bytes())
            linked = {'passed': True, 'required_definitions': len(result['linked_symbols'])}
        except (OSError, ValueError) as error:
            linked = {'passed': False, 'error': str(error)}
            blockers.append('Requested kernel does not pass current composition link checks')
    return {'scope': 'OFFLINE blocker inventory, NOT a firmware/hardware authority verifier',
            'production_activation_calls': calls,
            'linked_kernel_check': linked,
            'blockers': blockers,
            'upstream_native_contract_verified': False,
            'current_xnu_boot_handoff_verified': False,
            'power_only_test_admissible': False,
            'boot_image_generation_allowed_by_this_audit': False,
            'gpu_acceleration_enabled': False,
            'hardware_access_performed': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel', type=Path)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args()
    report = audit(kernel=args.kernel)
    with args.out.open('x') as f:
        json.dump(report, f, indent=2)
        f.write('\n')
    print(json.dumps(report, indent=2))
    return 1  # An offline inventory cannot issue a power-on authorization.


if __name__ == '__main__':
    raise SystemExit(main())
