"""Fixed GPU PLL1 vs independently pinned public register/rate facts; RAM ONLY."""
import argparse
from fractions import Fraction
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
from ios16_sources import kernel_source


def run(argv):
    result = subprocess.run(argv, capture_output=True, timeout=90)
    if result.returncode:
        raise RuntimeError(repr(argv) + '\n' + result.stdout.decode(errors='replace') +
                           result.stderr.decode(errors='replace'))
    return result.stdout.decode()


def body(text, declaration):
    match = re.search(re.escape(declaration) + r'\s*=\s*\{(.*?)^\};', text, re.S | re.M)
    assert match, declaration
    return match[1]


def source_policy():
    pin = json.loads((ROOT / 'hardware/gpu-pll1-source.json').read_text())
    texts = {}
    for item in pin['files']:
        data = (ROOT / '.cache/gpu-pll1-source' / item['path']).read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
        texts[Path(item['path']).name] = data.decode()
    gpu, pll, header = (texts[n] for n in ('gpucc-pineapple.c', 'clk-alpha-pll.c', 'clk-alpha-pll.h'))
    assert re.search(r'#define clk_alpha_pll_lucid_ole_ops\s+clk_alpha_pll_lucid_evo_ops', header)
    assert re.search(r'#define clk_lucid_ole_pll_configure\(pll, regmap, config\)\s*\\\s*'
                     r'clk_lucid_evo_pll_configure\(pll, regmap, config\)', header)
    desc = body(gpu, 'static struct clk_alpha_pll gpu_cc_pll1')
    base = int(re.search(r'\.offset\s*=\s*(0x[0-9a-f]+)', desc)[1], 16)
    assert 'CLK_ALPHA_PLL_TYPE_LUCID_OLE' in desc and '&vdd_mx' in desc
    assert 'clk_alpha_pll_lucid_ole_ops' in desc
    vco = body(gpu, 'static const struct pll_vco lucid_ole_vco[]')
    assert '{ 249600000, 2100000000, 0 }' in vco
    layout = re.search(r'\[CLK_ALPHA_PLL_TYPE_LUCID_OLE\]\s*=\s*\{(.*?)\n\s*\},', pll, re.S)[1]
    offsets = {k: int(v, 16) for k, v in re.findall(r'\[(PLL_OFF_\w+)\]\s*=\s*(0x[0-9a-f]+)', layout)}
    assert offsets['PLL_OFF_L_VAL'] == 16 and offsets['PLL_OFF_ALPHA_VAL'] == 20
    for key, val in [('PLL_RESET_N', 'BIT(2)'), ('PLL_OUTCTRL', 'BIT(0)'),
                     ('PLL_LOCK_DET', 'BIT(31)'), ('LUCID_EVO_PCAL_NOT_DONE', 'BIT(8)'),
                     ('LUCID_EVO_ENABLE_VOTE_RUN', 'BIT(25)'),
                     ('LUCID_5LPE_ALPHA_PLL_ACK_LATCH', 'BIT(13)'),
                     ('LUCID_5LPE_PLL_LATCH_INPUT', 'BIT(14)'), ('PLL_OUT_MASK', '0x7')]:
        assert re.search(r'#\s*define\s+' + key + r'\s+' + re.escape(val) + r'\s*$', pll, re.M), key
    assert 'for (count = 200; count > 0; count--)' in pll
    assert 'remainder << ALPHA_SHIFT(alpha_width)' in pll
    assert 'return roundup(rate, 1000);' in pll
    assert 'regval & PLL_RESET_N' in pll and 'Wait for 2 reference cycles' in pll
    config = body(gpu, 'static const struct alpha_pll_config gpu_cc_pll1_config')
    values = {k: int(v, 16) for k, v in re.findall(r'\.(\w+)\s*=\s*(0x[0-9a-fA-F]+)', config)}
    fields = [('user_ctl_hi_val', 'USER_CTL_U'), ('config_ctl_val', 'CONFIG_CTL'),
              ('config_ctl_hi_val', 'CONFIG_CTL_U'), ('config_ctl_hi1_val', 'CONFIG_CTL_U1'),
              ('test_ctl_val', 'TEST_CTL'), ('test_ctl_hi_val', 'TEST_CTL_U'),
              ('test_ctl_hi1_val', 'TEST_CTL_U1'), ('test_ctl_hi2_val', 'TEST_CTL_U2')]
    initial = [(base + offsets['PLL_OFF_L_VAL'], values['l'] | values['cal_l'] << 16 |
                values['cal_l_ringosc'] << 24),
               (base + offsets['PLL_OFF_ALPHA_VAL'], values['alpha']),
               (base + offsets['PLL_OFF_USER_CTL'], values['user_ctl_val'] | 7)]
    initial += [(base + offsets['PLL_OFF_' + reg], values[field]) for field, reg in fields]
    table = body(gpu, 'static const struct freq_tbl ftbl_gpu_cc_gmu_clk_src[]')
    rates = [int(hz) * int(div) for hz, div in
             re.findall(r'F\((\d+),\s*P_GPU_CC_PLL1_OUT_MAIN,\s*(\d+),\s*0,\s*0\)', table)]
    assert len(rates) == 2
    plans = []
    for rate in rates:
        # Independent rational arithmetic; NEVER import the implementation.
        ratio = Fraction(rate, 19200000)
        scaled = (ratio * 65536).__floor__()
        l, alpha = divmod(scaled, 65536)
        encoded = Fraction(scaled * 19200000, 65536)
        floor = encoded.__floor__()
        reported = ((floor + 999) // 1000) * 1000
        assert rate - floor < 293 and reported == rate
        plans.append([rate, 19200000, l, alpha, floor, reported, rate - floor])
    assert not pin['native_source_bound'] and not pin['hardware_validated']
    return base, initial, plans


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    base, initial, plans = source_policy()
    source = ROOT / 'ios16/gpu/giulia_gpu_pll1.c'
    header = source.with_suffix('.h')
    wanted = tuple('_ggp1_' + name for name in ('identity', 'describe', 'open', 'start', 'invalidate'))
    for path in (ROOT / 'ios16').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h', '.inc') and path not in (source, header, ROOT / 'ios16/gpu/giulia_gpu_board_source.c'):
            assert not re.search(r'\bggp1_(?:open|start)\s*\(', path.read_text()), path
    outcomes = []
    with tempfile.TemporaryDirectory(prefix='gpu-pll1-') as folder:
        tmp = Path(folder)
        oracle = f'static const unsigned source_base={base}u;\n'
        oracle += ('static const unsigned source_out=1u,source_reset=4u,source_latch=1u<<14,'
                   'source_ack=1u<<13,source_lock=1u<<31;\n')
        oracle += 'static const unsigned source_init[][2]={\n'
        oracle += ',\n'.join('{' + ','.join(str(v) + 'u' for v in row) + '}' for row in initial) + '};\n'
        oracle += 'static const struct ggp1_plan source_plans[]={\n'
        oracle += ',\n'.join('{' + ','.join(str(v) + 'u' for v in row) + '}' for row in plans) + '};\n'
        (tmp / 'pll1-source-policy.h').write_text(oracle)
        for cc in host_compilers():
            binary = tmp / (Path(cc).name + '-pll1')
            run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', *host_link_flags(), '-DGG_HOST_TEST',
                 '-I', str(tmp), '-I', str(header.parent),
                 str(ROOT / 'tests/ios16_gpu_pll1.c'), str(source), '-o', str(binary)])
            output = run([str(binary)]).strip()
            print(cc + ': ' + output, flush=True)
            outcomes.append({'compiler': cc, 'output': output})
        obj = tmp / 'pll1.o'
        run(['cc', '-std=c11', '-DGG_HOST_TEST', '-c', str(source), '-o', str(obj)])
        caller = tmp / 'caller.cpp'
        caller.write_text('''#include "giulia_gpu_pll1.h"
#include <cassert>
#include <cstring>
int main(){ggp1_state s={};ggp1_plan p={};
assert(!std::strcmp(ggp1_identity(),GGP1_IDENTITY));
assert(ggp1_describe(520000000,&p)==GGP1_OK);
assert(ggp1_open(&s,nullptr)==GGP1_ARGUMENT);
assert(ggp1_start(&s,520000000)==GGP1_USED);ggp1_invalidate(&s);}
''')
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(header.parent),
             str(caller), str(obj), '-o', str(tmp / 'caller')])
        run([str(tmp / 'caller')])
        (tmp / header.name).write_text(kernel_source(header.read_text()))
        stage = tmp / 'pll1.cpp'
        stage.write_text('typedef unsigned int uint32_t;\ntypedef unsigned long long uint64_t;\n' +
                         kernel_source(source.read_text()))
        macho = tmp / 'pll1-arm64.o'
        run([os.environ.get('ISA_CXX', clang_compiler(cxx=True)), '--target=arm64-apple-macos',
             '-ffreestanding', '-stdlib=libc++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
             '-DGG_HOST_TEST', '-c', str(stage), '-o', str(macho)])
        spec = importlib.util.spec_from_file_location('gpu_link_checker', ROOT / 'scripts/check-ios16-gpu-kernel.py')
        checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(checker)
        checker.SYMBOLS = wanted
        data = macho.read_bytes()
        assert checker.linked_functions(data) == set(wanted)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / 'pll1-arm64-macho.o').write_bytes(data)
            report = {'host_outcomes': outcomes, 'source_oracle_plans': plans,
                      'arm64_macho_defined_exports': list(wanted),
                      'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
                      'full_xnu_link_performed': False, 'native_xnu_header_abi_verified': False,
                      'native_source_bound': False, 'hardware_validated': False,
                      'power_only_test_admissible': False, 'gpu_acceleration_enabled': False}
            (args.out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('GPU PLL1: 2 source-derived plans; C++ linkage and 5 ARM64 Mach-O definitions; no hardware execution')


if __name__ == '__main__':
    main()
