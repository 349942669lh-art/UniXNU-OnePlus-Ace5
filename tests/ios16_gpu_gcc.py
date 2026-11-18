"""Execute actual GCC C against a pinned source-derived RAM policy oracle."""
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
import urllib.request

from gpu_host_tools import clang_compiler, host_compilers, host_link_flags

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import kernel_source


def run(args):
    result = subprocess.run(args, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(repr(args) + '\n' + result.stdout + result.stderr)
    return result.stdout


def source_policy(fetch=False):
    pins = json.loads((ROOT / 'hardware/gpu-gcc-source.json').read_text())
    texts = {}
    for item in pins['files']:
        path = ROOT / '.cache/gpu-gcc-source' / item['path']
        if not path.exists() and fetch:
            with urllib.request.urlopen(item['url'], timeout=30) as response:
                data = response.read(item['bytes'] + 1)
            if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
                raise ValueError('Downloaded GCC source pin mismatch: ' + item['path'])
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        data = path.read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
        texts[path.name] = data.decode()
    source, common = texts['gcc-pineapple.c'], texts['clk-branch.c']
    assert re.search(r'regmap_update_bits\(regmap, 0x71004, BIT\(0\), BIT\(0\)\)', source)
    policies = {'probe-direct': 0, 'BRANCH_HALT_SKIP': 1,
                'BRANCH_HALT_VOTED': 2, 'BRANCH_HALT_DELAY': 3}
    offsets, selected_policies, hwcg = [], [], []
    for item in pins['branches']:
        if item['policy'] == 'probe-direct':
            offset, policy, mask = 4, 'probe-direct', 0
        else:
            match = re.search(r'static struct clk_branch ' + item['name'] + r' = \{(.*?)^\};', source, re.S | re.M)
            assert match, item['name']
            body = match[1]
            enable = int(re.search(r'\.enable_reg\s*=\s*(0x[0-9a-f]+)', body)[1], 16)
            halt = int(re.search(r'\.halt_reg\s*=\s*(0x[0-9a-f]+)', body)[1], 16)
            assert enable == halt and re.search(r'\.enable_mask\s*=\s*BIT\(0\)', body)
            policy = re.search(r'\.halt_check\s*=\s*(\w+)', body)[1]
            clock = re.search(r'\.hwcg_reg\s*=\s*(0x[0-9a-f]+)', body)
            mask = 0
            if clock:
                assert int(clock[1], 16) == enable
                mask = 1 << int(re.search(r'\.hwcg_bit\s*=\s*(\d+)', body)[1])
            offset = 0x100000 + enable - pins['map_base']
        assert (offset, policy, mask) == (item['offset'], item['policy'], item['hwcg_mask'])
        offsets.append(offset); selected_policies.append(policies[policy]); hwcg.append(mask)
    assert 'br->halt_check == BRANCH_HALT_SKIP || clk_branch_in_hwcg_mode(br)' in common
    assert 'br->halt_check == BRANCH_HALT_DELAY || (!enabling && voted)' in common
    assert 'udelay(10)' in common and 'return max(timeout, 200)' in common
    assert not pins['native_source_bound'] and not pins['hardware_validated']
    return offsets, selected_policies, hwcg


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--fetch-only', action='store_true')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    offsets, policies, hwcg = source_policy(args.fetch or args.fetch_only)
    if args.fetch_only:
        print('GCC source pins verified'); return
    source = ROOT / 'ios16/gpu/giulia_gpu_gcc.c'
    header = source.with_suffix('.h')
    # Only the explicit board composition can open/execute this hardware core.
    for path in (ROOT / 'ios16').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h', '.inc') and path.name not in (
                source.name, header.name, 'giulia_gpu_board_source.c'):
            assert not re.search(r'\bgggc_(?:open|enable)\s*\(', path.read_text()), path
    wanted = tuple('_gggc_' + name for name in ('identity', 'describe', 'open', 'enable', 'invalidate'))
    outcomes = []
    with tempfile.TemporaryDirectory(prefix='gpu-gcc-') as directory:
        tmp = Path(directory)
        # Extracted public policy constants are HOST-only; never staged into XNU.
        oracle = '/* GPL-2.0-only source-derived HOST policy data. */\n'
        for name, values in [('offsets', offsets), ('policies', policies), ('hwcg', hwcg)]:
            oracle += 'static const unsigned source_' + name + '[]={' + ','.join(map(str, values)) + '};\n'
        (tmp / 'gcc-source-policy.h').write_text(oracle)
        for compiler in host_compilers():
            binary = tmp / (Path(compiler).name + '-gcc')
            run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                 *host_link_flags(), '-DGG_HOST_TEST', '-I', str(tmp), '-I', str(header.parent),
                 str(ROOT / 'tests/ios16_gpu_gcc.c'), str(source), '-o', str(binary)])
            output = run([str(binary)]).strip()
            outcomes.append(dict(compiler=compiler, output=output)); print(compiler + ': ' + output, flush=True)
        obj = tmp / 'gcc.o'
        run(['cc', '-std=c11', '-DGG_HOST_TEST', '-c', str(source), '-o', str(obj)])
        caller = tmp / 'caller.cpp'
        caller.write_text('#include "giulia_gpu_gcc.h"\n#include <cassert>\n'
                          'int main(){gggc_state s={};gggc_branch b={};'
                          'assert(gggc_identity()!=nullptr);assert(gggc_describe(0,&b)==GGGC_OK);'
                          'assert(gggc_open(nullptr,nullptr)==GGGC_ARGUMENT);'
                          'assert(gggc_enable(&s,0)==GGGC_USED);gggc_invalidate(&s);return 0;}\n')
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(header.parent),
             str(caller), str(obj), '-o', str(tmp / 'caller')]); run([str(tmp / 'caller')])
        (tmp / header.name).write_text(kernel_source(header.read_text()))
        stage = tmp / 'gcc.c'
        stage.write_text('typedef unsigned int uint32_t;\n' + kernel_source(source.read_text()))
        arm64 = tmp / 'gcc-arm64.o'
        run([clang_compiler(), '--target=arm64-apple-macos', '-ffreestanding', '-std=c11',
             '-Wall', '-Wextra', '-Werror', '-DGG_HOST_TEST', '-c', str(stage), '-o', str(arm64)])
        spec = importlib.util.spec_from_file_location('gcc_link_check', ROOT / 'scripts/check-ios16-gpu-kernel.py')
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        checker.SYMBOLS = wanted
        assert checker.linked_functions(arm64.read_bytes()) == set(wanted)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / 'gcc-arm64-macho.o').write_bytes(arm64.read_bytes())
            (args.out / 'verification.json').write_text(json.dumps(dict(
                host_outcomes=outcomes, arm64_macho_defined_exports=wanted,
                source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                hardware_tested=False, native_xnu_header_abi_verified=False,
                power_only_test_admissible=False), indent=2) + '\n')
    print('GCC policies, callback faults/loss, C++ linkage and 5 ARM64 definitions passed; no hardware accessed')


if __name__ == '__main__':
    main()
