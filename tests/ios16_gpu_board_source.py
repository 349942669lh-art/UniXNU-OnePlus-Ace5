"""Execute actual board/PLL1/RCG/GPUCC/GCC/CX C against RAM-only native doubles."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from gpu_host_tools import clang_compiler, host_compilers, host_link_flags

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import kernel_source, include_flags


def run(args):
    p = subprocess.run(args, capture_output=True, timeout=120)
    if p.returncode:
        raise RuntimeError(repr(args) + '\n' + p.stdout.decode(errors='replace') + p.stderr.decode(errors='replace'))
    return p.stdout.decode()


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    # Independent source-derived init (including calibration and analog
    # fields), not values copied from the board implementation.
    pll = module('pll_source_oracle', ROOT / 'tests/ios16_gpu_pll1.py')
    base, initial, plans = pll.source_policy()
    assert base == 0x1000 and len(plans) == 2
    pins = json.loads((ROOT / 'hardware/gpu-gpucc-source.json').read_text())
    offsets = [entry['offset'] for entry in pins['branches']]
    source = ROOT / 'ios16/gpu/giulia_gpu_board_source.c'
    header = source.with_suffix('.h')
    # The composition now calls the lower executors; no automatic caller may
    # initialize or register the new source without implementing its upstream.
    for path in (ROOT / 'ios16').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h', '.inc') and path not in (source, header):
            assert not re.search(r'\b(?:ggbs_init|giulia_gpu_board_source_register)\s*\(', path.read_text()), path
    units = [ROOT / 'ios16/gpu' / ('giulia_gpu_' + name + '.c')
             for name in ('board_source', 'rcg', 'pll1', 'gpucc', 'gcc', 'cx')]
    wanted = ('_ggbs_identity', '_ggbs_init', '_ggbs_export',
              '_giulia_gpu_board_source_register', '_ggbs_invalidate', '_ggbs_status')
    outcomes = []
    with tempfile.TemporaryDirectory(prefix='gpu-board-source-') as directory:
        tmp = Path(directory)
        policy = 'static const unsigned pll_init[][2]={\n'
        policy += ',\n'.join('{' + ','.join(str(v) + 'u' for v in row) + '}' for row in initial) + '};\n'
        policy += 'static const unsigned branch_offsets[]={' + ','.join(map(str, offsets)) + '};\n'
        (tmp / 'board-source-policy.h').write_text(policy)
        for cc in host_compilers():
            binary = tmp / (Path(cc).name + '-board-source')
            run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', *host_link_flags(), '-DGG_HOST_TEST',
                 '-I', str(tmp), *include_flags(ROOT),
                 str(ROOT / 'tests/ios16_gpu_board_source.c'), *map(str, units), '-o', str(binary)])
            output = run([str(binary)]).strip()
            outcomes.append({'compiler': cc, 'output': output})
            print(cc + ': ' + output, flush=True)
        # Compile ALL actual headers together as C++: this would fail on the
        # former GGRC_OK namespace collision, even though separate units linked.
        probe = tmp / 'headers.cpp'
        probe.write_text('#include "giulia_gpu_board_source.h"\n'
                         '#include <cassert>\n'
                         'static_assert(GGRC_OK==0 && GGRCLK_OK==0,"ABI values");\n'
                         'extern "C" int giulia_gpu_power_owner_initialize(){return GGPO_OK;}\n'
                         'extern "C" int giulia_gpu_power_owner_register(void*,const ggpo_source*){return GGPO_OK;}\n'
                         'int main(){struct ggbs_state state={};struct ggpo_source source={};'
                         'struct ggbs_status status={};'
                         'assert(ggbs_identity()!=nullptr);'
                         'assert(ggbs_init(nullptr,nullptr,nullptr)==GGBS_ARGUMENT);'
                         'assert(ggbs_export(&state,&source)==GGBS_UNAVAILABLE);'
                         'assert(giulia_gpu_board_source_register(nullptr,&state)==GGBS_ARGUMENT);'
                         'ggbs_invalidate(&state);assert(ggbs_status(&state,&status)==GGBS_OK && status.lost);return 0;}\n')
        objects = []
        for unit in units:
            obj = tmp / (unit.stem + '.o')
            run(['cc', '-std=c11', '-DGG_HOST_TEST', *include_flags(ROOT), '-c', str(unit), '-o', str(obj)])
            objects.append(str(obj))
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *include_flags(ROOT),
             str(probe), *objects, '-o', str(tmp / 'headers')])
        run([str(tmp / 'headers')])
        # Isolated ARM64 Mach-O build does NOT verify native XNU header ABI.
        gpu = ROOT / 'ios16/gpu'
        for path in gpu.glob('*.h'):
            (tmp / path.name).write_text(kernel_source(path.read_text()))
        for path in (ROOT / 'ios16/platform').glob('*.h'):
            (tmp / path.name).write_text(kernel_source(path.read_text()))
        stage = tmp / 'board.c'
        stage.write_text('typedef unsigned int uint32_t;\ntypedef unsigned long long uint64_t;\n'
                         'typedef __SIZE_TYPE__ size_t;\n#define NULL ((void *)0)\n' + kernel_source(source.read_text()))
        obj = tmp / 'board-arm64.o'
        run([os.environ.get('ISA_CC', clang_compiler()), '--target=arm64-apple-macos',
             '-ffreestanding', '-std=c11', '-Wall', '-Wextra', '-Werror',
             '-DGG_HOST_TEST', '-c', str(stage), '-o', str(obj)])
        checker = module('board_link_checker', ROOT / 'scripts/check-ios16-gpu-kernel.py')
        checker.SYMBOLS = wanted
        data = obj.read_bytes()
        assert checker.linked_functions(data) == set(wanted)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / 'board-source-arm64-macho.o').write_bytes(data)
            result = {'host_outcomes': outcomes, 'arm64_macho_defined_exports': wanted,
                      'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
                      'retained_test_composition': True, 'cold_calibration_supported': False,
                      'upstream_firmware_handoff_implemented': False,
                      'local_gcc_axi_memnoc_dvm_implemented': True,
                      'local_cx_gdsc_implemented': True,
                      'cfg_prepared_by_composition': False,
                      'upstream_voltage_parent_bandwidth_implemented': False,
                      'production_binding_present': False,
                      'native_xnu_header_abi_verified': False, 'full_xnu_link_performed': False,
                      'power_only_test_admissible': False, 'hardware_tested': False,
                      'boot_image_generated': False, 'gpu_acceleration_enabled': False}
            (args.out / 'verification.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Board source composition: actual fixed CX/clock executors; C++ headers and 6 ARM64 definitions; upstream unbound')


if __name__ == '__main__':
    main()
