"""Execute actual CX GDSC C against a pinned source-derived RAM policy oracle."""
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
    pins = json.loads((ROOT / 'hardware/gpu-cx-source.json').read_text())
    texts = {}
    for item in pins['files']:
        path = ROOT / '.cache/gpu-cx-source' / item['path']
        if not path.exists() and fetch:
            with urllib.request.urlopen(item['url'], timeout=30) as response:
                data = response.read(item['bytes'] + 1)
            assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        data = path.read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
        texts[path.name] = data.decode()
    source = texts['gdsc-regulator.c']
    def bit(name):
        return 1 << int(re.search(r'#define\s+' + name + r'\s+BIT\((\d+)\)', source)[1])
    dt = texts['pineapple-gdsc.dtsi']
    node = re.search(r'gpu_cc_cx_gdsc:.*?\{(.*?)\n\s*\};', dt, re.S)[1]
    status = re.search(r'gpu_cc_cx_gdsc_hw_ctrl:.*?\{(.*?)\n\s*\};', dt, re.S)[1]
    control = int(re.search(r'reg = <(0x[0-9a-f]+)', node)[1], 16) - pins['gpucc_base']
    hw = int(re.search(r'reg = <(0x[0-9a-f]+)', status)[1], 16) - pins['gpucc_base']
    wait = int(re.search(r'qcom,clk-dis-wait-val = <(\d+)>', node)[1])
    assert 'qcom,retain-regs;' in node and 'qcom,support-cfg-gdscr;' not in node
    overlay = re.search(r'&gpu_cc_cx_gdsc\s*\{(.*?)\n\};', texts['pineapple.dtsi'], re.S)[1]
    assert 'GCC_GPU_CFG_AHB_CLK' in overlay and 'VDD_CX_LEVEL' in overlay
    assert 'regval &= ~(HW_CONTROL_MASK | SW_OVERRIDE_MASK);' in source
    assert 'regval &= ~SW_COLLAPSE_MASK;' in source
    assert 'regval |= RETAIN_FF_ENABLE_MASK;' in source
    assert 'regmap = sc->hw_ctrl;' in source
    assert source.count('ret = poll_gdsc_status(sc, state);') == 2
    polls = int(re.search(r'#define\s+TIMEOUT_US\s+(\d+)', source)[1])
    shift = int(re.search(r'#define\s+CLK_DIS_WAIT_SHIFT\s+\((\d+)\)', source)[1])
    width = int(re.search(r'#define\s+CLK_DIS_WAIT_MASK\s+\((0x[0-9A-Fa-f]+)', source)[1], 16)
    assert (control, hw, wait, polls) == (0x9108, 0x9168, 8, 1500)
    assert not pins['native_source_bound'] and not pins['hardware_validated']
    return [control, hw, bit('HW_CONTROL_MASK') | bit('SW_OVERRIDE_MASK'),
            width << shift, wait << shift, bit('SW_COLLAPSE_MASK'),
            bit('RETAIN_FF_ENABLE_MASK'), bit('PWR_ON_MASK'), polls, 2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--fetch-only', action='store_true')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    policy = source_policy(args.fetch or args.fetch_only)
    if args.fetch_only:
        print('CX source pins verified'); return
    source = ROOT / 'ios16/gpu/giulia_gpu_cx.c'
    header = source.with_suffix('.h')
    # Only the explicit board composition can open/execute this hardware core.
    for path in (ROOT / 'ios16').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h', '.inc') and path.name not in (
                source.name, header.name, 'giulia_gpu_board_source.c'):
            assert not re.search(r'\bggcx_(?:open|enable)\s*\(', path.read_text()), path
    wanted = tuple('_ggcx_' + name for name in ('identity', 'describe', 'open', 'enable', 'invalidate'))
    outcomes = []
    with tempfile.TemporaryDirectory(prefix='gpu-cx-') as directory:
        tmp = Path(directory)
        # Extracted public policy constants are HOST-only; never staged into XNU.
        oracle = '/* GPL-2.0-only source-derived HOST policy data. */\n'
        oracle += 'static const unsigned source_policy[]={' + ','.join(map(str, policy)) + '};\n'
        (tmp / 'cx-source-policy.h').write_text(oracle)
        for compiler in host_compilers():
            binary = tmp / (Path(compiler).name + '-cx')
            run([compiler, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                 *host_link_flags(), '-DGG_HOST_TEST', '-I', str(tmp), '-I', str(header.parent),
                 str(ROOT / 'tests/ios16_gpu_cx.c'), str(source), '-o', str(binary)])
            output = run([str(binary)]).strip()
            outcomes.append(dict(compiler=compiler, output=output)); print(compiler + ': ' + output, flush=True)
        obj = tmp / 'cx.o'
        run(['cc', '-std=c11', '-DGG_HOST_TEST', '-c', str(source), '-o', str(obj)])
        caller = tmp / 'caller.cpp'
        caller.write_text('#include "giulia_gpu_cx.h"\n#include <cassert>\n'
                          'int main(){ggcx_state s={};ggcx_policy b={};'
                          'assert(ggcx_identity()!=nullptr);assert(ggcx_describe(&b)==GGCX_OK);'
                          'assert(ggcx_open(nullptr,nullptr)==GGCX_ARGUMENT);'
                          'assert(ggcx_enable(&s)==GGCX_USED);ggcx_invalidate(&s);return 0;}\n')
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(header.parent),
             str(caller), str(obj), '-o', str(tmp / 'caller')]); run([str(tmp / 'caller')])
        (tmp / header.name).write_text(kernel_source(header.read_text()))
        stage = tmp / 'cx.c'
        stage.write_text('typedef unsigned int uint32_t;\n' + kernel_source(source.read_text()))
        arm64 = tmp / 'cx-arm64.o'
        run([clang_compiler(), '--target=arm64-apple-macos', '-ffreestanding', '-std=c11',
             '-Wall', '-Wextra', '-Werror', '-DGG_HOST_TEST', '-c', str(stage), '-o', str(arm64)])
        spec = importlib.util.spec_from_file_location('gcc_link_check', ROOT / 'scripts/check-ios16-gpu-kernel.py')
        checker = importlib.util.module_from_spec(spec); spec.loader.exec_module(checker)
        checker.SYMBOLS = wanted
        assert checker.linked_functions(arm64.read_bytes()) == set(wanted)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / 'cx-arm64-macho.o').write_bytes(arm64.read_bytes())
            (args.out / 'verification.json').write_text(json.dumps(dict(
                host_outcomes=outcomes, arm64_macho_defined_exports=wanted,
                source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                header_sha256=hashlib.sha256(header.read_bytes()).hexdigest(),
                hardware_tested=False, native_xnu_header_abi_verified=False,
                power_only_test_admissible=False), indent=2) + '\n')
    print('CX policy, callback faults/loss, C++ linkage and 5 ARM64 definitions passed; no hardware accessed')


if __name__ == '__main__':
    main()
