"""Compile and execute the actual resident XNU coordinator against CPU fixtures."""
from pathlib import Path
import os
import json
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import include_flags

board = json.loads((ROOT / 'hardware/sm8650-gpu.json').read_text())['board']
source = (ROOT / 'ios16/gpu/giulia_gpu_power_owner.cpp').read_text()
scope = source.split('static const struct ggpo_scope powerScope={', 1)[1].split('\n};', 1)[0]
ranges = [(int(a, 16), int(b, 16)) for a, b in
          re.findall(r'\{(0x[0-9a-f]+),(0x[0-9a-f]+)\}', scope)]
expected = [board['gpu_windows']['kgsl_3d0_reg_memory'],
            board['gmu_windows']['gmu'], board['gmu_windows']['gmu_ao_blk_dec0']]
assert ranges == [(r['base'], r['size']) for r in expected] + [
    tuple(board['gpucc_reg']),
    (board['smmu_windows']['base']['base'], board['smmu_windows']['base']['size']),
    (0x171000, 0x1000)]
ids = re.search(r'(0x[0-9a-f]+),\{(\d+),(\d+),(\d+)\},GGPO_PREREQUISITES', scope)
assert ids and int(ids[1], 16) == sum(1 << board['stream_ids'][key] for key in ('user', 'lpac', 'gmu'))
assert [int(ids[i]) for i in (2, 3, 4)] == [board['gpu_interrupts'][1] + 32,
                                                         *[i + 32 for i in board['gmu_interrupts'][1::3]]]

with tempfile.TemporaryDirectory(prefix='gpu-power-owner-') as folder:
    folder = Path(folder)
    flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g', '-pthread', *include_flags(ROOT)]
    runtime = folder / 'runtime.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-DGG_HOST_TEST',
                    '-c', str(ROOT / 'ios16/gpu/giulia_gpu_runtime.c'),
                    '-o', str(runtime)], check=True)
    binary = folder / 'power-owner'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *flags,
                    str(ROOT / 'tests/ios16_gpu_power_owner.cpp'), str(runtime),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
