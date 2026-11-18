"""Run production native executor + transport with explicitly RAM-only doubles.

This does not qualify real board ownership, physical mapping, clocks or IRQs.
No fetching, credentials, private source import or device operations.
"""
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
manifest = json.loads((ROOT / 'hardware/gpu-rsc-native-source.json').read_text())
assert not manifest['production_authority_bound'] and not manifest['hardware_validated']
assert manifest['drv'] == {'base': 0x17a20000, 'bytes': 0x10000, 'id': 2,
                            'tcs_offset': 0xd00, 'active_mask': 7, 'intid': 37}
pins = json.loads((ROOT / 'hardware/gpu-rsc-source.json').read_text())
entry = next(e for e in pins['files'] if e['path'].endswith('/rpmh-rsc.c'))
data = (ROOT / '.cache/gpu-rsc-source' / entry['path']).read_bytes()
assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']
reference = data.decode()
# Independent metadata protocol oracle. Original Linux values are inspected
# only by host tests, never embedded as code/header fragments in production.
for name, value in {'RSC_DRV_ID': 0, 'MAJOR_VER_SHIFT': 16, 'MINOR_VER_SHIFT': 8,
                    'DRV_HW_SOLVER_SHIFT': 24, 'DRV_NUM_TCS_SHIFT': 6,
                    'DRV_NCPT_SHIFT': 27}.items():
    found = re.search(r'^#define\s+' + name + r'\s+(0x[0-9A-Fa-f]+|[0-9]+)\s*$', reference, re.M)
    assert found and int(found[1], 0) == value, name
for name, value in {'DRV_SOLVER_CONFIG': 4, 'DRV_PRNT_CHLD_CONFIG': 12}.items():
    found = re.findall(r'\[' + name + r'\]\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+),', reference)
    assert found and {int(v, 0) for v in found} == {value}, name
for path in (ROOT / 'ios16').rglob('*'):
    if path.suffix in ('.c', '.cpp', '.h', '.inc') and path.name not in (
            'giulia_gpu_rsc_native.cpp', 'giulia_gpu_rsc_native.h'):
        assert not re.search(r'\bgiulia_gpu_rsc_native_(initialize|open|submit|poll)\s*\(', path.read_text()), path
flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-g']
with tempfile.TemporaryDirectory(prefix='gpu-rsc-native-') as folder:
    folder = Path(folder)
    obj = folder / 'rsc.o'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, '-DGG_HOST_TEST',
                    '-c', str(ROOT / 'ios16/gpu/giulia_gpu_rsc.c'), '-o', str(obj)], check=True)
    binary = folder / 'native'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *flags, '-pthread',
                    str(ROOT / 'tests/ios16_gpu_rsc_native.cpp'), str(obj), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
print('GPU native RSC: production call boundary checked; no platform authority supplied or hardware tested')
