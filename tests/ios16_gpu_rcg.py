"""Fixed GPU RCG executor vs hash-pinned public source. HOST RAM ONLY."""
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
from ios16_sources import kernel_source


def run(argv):
    result = subprocess.run(argv, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(repr(argv) + '\n' + result.stdout.decode(errors='replace') +
                           result.stderr.decode(errors='replace'))
    return result.stdout.decode()


def body(text, declaration):
    match = re.search(re.escape(declaration) + r'\s*=\s*\{(.*?)^\};', text, re.S | re.M)
    assert match, declaration
    return match[1]


def source_plans():
    pin = json.loads((ROOT / 'hardware/gpu-rcg-source.json').read_text())
    texts = {}
    for item in pin['files']:
        data = (ROOT / '.cache/gpu-rcg-source' / item['path']).read_bytes()
        assert len(data) == item['bytes'] and hashlib.sha256(data).hexdigest() == item['sha256']
        texts[Path(item['path']).name] = data.decode()
    gpu, rcg, header = (texts[n] for n in ('gpucc-pineapple.c', 'clk-rcg2.c', 'clk-rcg.h'))
    assert re.search(r'#define F\(f, s, h, m, n\) \{ \(f\), \(s\), \(2 \* \(h\) - 1\)', header)
    for key, value in [('CFG_REG', '0x4'), ('CFG_SRC_SEL_SHIFT', '8'),
                       ('CFG_MODE_SHIFT', '12'), ('CMD_UPDATE', 'BIT(0)'),
                       ('CMD_ROOT_EN', 'BIT(1)'), ('CMD_ROOT_OFF', 'BIT(31)'),
                       ('CFG_HW_CLK_CTRL_MASK', 'BIT(20)')]:
        assert re.search(r'#define\s+' + key + r'\s+' + re.escape(value) + r'\s*$', rcg, re.M), key
    assert 'return max(timeout, 500);' in rcg and 'udelay(1);' in rcg
    assert 'if (!(cmd & CMD_UPDATE))' in rcg
    # Check safe-enable mechanics without copying the vendor implementation.
    assert 'clk_rcg2_set_force_enable(hw);' in rcg and 'clk_rcg2_clear_force_enable(hw);' in rcg
    safe = body(rcg, 'static struct freq_tbl cxo_f')
    xo = int(re.search(r'\.freq\s*=\s*(\d+)', safe)[1])
    assert int(re.search(r'\.pre_div\s*=\s*(\d+)', safe)[1]) == 1
    mask = ((1 << 5) - 1) | (7 << 8) | (3 << 12) | (1 << 20)
    plans, offsets = [], []
    for root, suffix in enumerate(('gmu', 'hub', 'ff')):
        name = 'gpu_cc_' + suffix + '_clk_src'
        desc = body(gpu, 'static struct clk_rcg2 ' + name)
        cmd = int(re.search(r'\.cmd_rcgr\s*=\s*(0x[0-9a-f]+)', desc)[1], 16)
        offsets.append(cmd)
        assert re.search(r'\.mnd_width\s*=\s*0', desc)
        assert re.search(r'\.hid_width\s*=\s*5', desc)
        assert '.enable_safe_config = true' in desc and '.flags = HW_CLK_CTRL_MODE' in desc
        assert 'CLK_SET_RATE_PARENT' in desc
        map_name = re.search(r'\.parent_map\s*=\s*(\w+)', desc)[1]
        parent_map = body(gpu, 'static const struct parent_map ' + map_name + '[]')
        muxes = {key: int(value) for key, value in re.findall(r'\{\s*(P_\w+),\s*(\d+)\s*\}', parent_map)}
        table = body(gpu, 'static const struct freq_tbl ftbl_' + name + '[]')
        entries = [(int(hz), parent, int(div)) for hz, parent, div in
                   re.findall(r'F\((\d+),\s*(P_\w+),\s*(\d+),\s*0,\s*0\)', table)]
        assert entries
        if not any(hz == xo for hz, _, _ in entries):
            entries.insert(0, (xo, 'P_BI_TCXO', 1))
        for hz, parent, div in entries:
            parent_id = {'P_BI_TCXO': 0, 'P_GPLL0_OUT_MAIN': 1,
                         'P_GPU_CC_PLL1_OUT_MAIN': 2}[parent]
            hid = 2 * div - 1
            parent_hz = hz * (hid + 1) // 2
            plans.append([root, hz, cmd, mask, (1 << 20) | (muxes[parent] << 8) | hid,
                          parent_id, parent_hz])
    assert len(plans) == 8
    assert not pin['native_source_bound'] and not pin['power_only_test_admissible']
    return plans, offsets, mask


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    plans, offsets, mask = source_plans()
    source = ROOT / 'ios16/gpu/giulia_gpu_rcg.c'
    header = source.with_suffix('.h')
    wanted = tuple('_ggrc_' + n for n in ('identity', 'describe', 'open', 'apply', 'invalidate'))
    for path in (ROOT / 'ios16').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h', '.inc') and path not in (source, header, ROOT / 'ios16/gpu/giulia_gpu_board_source.c'):
            assert not re.search(r'\bggrc_(?:open|apply)\s*\(', path.read_text()), path
    outcomes = []
    with tempfile.TemporaryDirectory(prefix='gpu-rcg-') as folder:
        tmp = Path(folder)
        oracle = '/* Extracted public register facts, disposable HOST test input. */\n'
        oracle += 'static const unsigned source_mask=' + str(mask) + 'u;\n'
        oracle += 'static const unsigned source_offsets[]={' + ','.join(map(str, offsets)) + '};\n'
        oracle += 'static const struct ggrc_plan source_plans[]={\n'
        oracle += ',\n'.join('{' + ','.join(str(v) + 'u' for v in p) + '}' for p in plans) + '};\n'
        (tmp / 'rcg-source-policy.h').write_text(oracle)
        for cc in host_compilers():
            binary = tmp / (Path(cc).name + '-rcg')
            run([cc, '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                 '-fno-omit-frame-pointer', *host_link_flags(), '-DGG_HOST_TEST',
                 '-I', str(tmp), '-I', str(header.parent),
                 str(ROOT / 'tests/ios16_gpu_rcg.c'), str(source), '-o', str(binary)])
            result = run([str(binary)]).strip()
            print(cc + ': ' + result, flush=True)
            outcomes.append({'compiler': cc, 'output': result})
        obj = tmp / 'rcg.o'
        run(['cc', '-std=c11', '-DGG_HOST_TEST', '-c', str(source), '-o', str(obj)])
        caller = tmp / 'caller.cpp'
        caller.write_text('''#include "giulia_gpu_rcg.h"
#include <cassert>
#include <cstring>
int main(){ggrc_state s={};ggrc_plan p={};
assert(!std::strcmp(ggrc_identity(),GGRCLK_IDENTITY));
assert(ggrc_describe(0,260000000,&p)==GGRCLK_OK);
assert(ggrc_open(&s,nullptr)==GGRCLK_ARGUMENT);
assert(ggrc_apply(&s,0,260000000)==GGRCLK_USED);ggrc_invalidate(&s);}
''')
        run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(header.parent),
             str(caller), str(obj), '-o', str(tmp / 'caller')])
        run([str(tmp / 'caller')])
        (tmp / header.name).write_text(kernel_source(header.read_text()))
        stage = tmp / 'rcg.cpp'
        stage.write_text('typedef unsigned int uint32_t;\n' + kernel_source(source.read_text()))
        macho = tmp / 'rcg-arm64.o'
        run([os.environ.get('ISA_CXX', clang_compiler(cxx=True)), '--target=arm64-apple-macos',
             '-ffreestanding', '-stdlib=libc++', '-std=c++17',
             '-Wall', '-Wextra', '-Werror', '-DGG_HOST_TEST', '-c', str(stage), '-o', str(macho)])
        spec = importlib.util.spec_from_file_location('gpu_link_checker', ROOT / 'scripts/check-ios16-gpu-kernel.py')
        checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(checker)
        checker.SYMBOLS = wanted
        data = macho.read_bytes()
        assert checker.linked_functions(data) == set(wanted)
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / 'rcg-arm64-macho.o').write_bytes(data)
            report = {'host_outcomes': outcomes, 'source_oracle_plans': plans,
                      'arm64_macho_defined_exports': list(wanted),
                      'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                      'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
                      'full_xnu_link_performed': False, 'native_xnu_header_abi_verified': False,
                      'native_source_bound': False, 'hardware_validated': False,
                      'power_only_test_admissible': False, 'gpu_acceleration_enabled': False}
            (args.out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('GPU RCG: 8 source-derived plans; C++ linkage and 5 ARM64 Mach-O definitions; no hardware execution')


if __name__ == '__main__':
    main()
