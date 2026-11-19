"""Compile the real UEFI snapshot producer with RAM-only protocol fixtures."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_boot_state import placeholder

headers = ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
with tempfile.TemporaryDirectory(prefix='gpu-boot-state-') as folder:
    folder = Path(folder)
    binary = folder / 'producer'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-I', str(headers), str(ROOT / 'tests/ios16_gpu_boot_state.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
    request = folder / 'request.bin'
    subprocess.run([str(binary), '--placeholder', str(request)], check=True, timeout=10)
    assert request.read_bytes() == placeholder()
    caller = folder / 'caller.cpp'
    caller.write_text('''#include "GpuBootState.h"
static_assert(sizeof(gbs_report)==256,"report ABI");
int main(){gbs_report r={};gbs_init(&r);return gbs_requested(&r)?0:1;}
''')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(headers), str(caller), '-o', str(folder / 'caller')], check=True)
    subprocess.run([str(folder / 'caller')], check=True, timeout=10)
print('GPU boot state C/Python wire ABI and C++ consumer passed')
