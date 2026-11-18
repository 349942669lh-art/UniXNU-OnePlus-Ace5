"""Compile production extraction against real libfdt, never a parser mock.

Use --uefi for the actual Mu BaseFdtLib vendored sources. Without --uefi this
uses the system libfdt-dev headers/library. This test is intentionally separate
from the portable GPU runner. Optional --dts/--dtb validates an explicit local
board artifact; it is only a test input, never a runtime capability default.
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--uefi', type=Path)
parser.add_argument('--dts', type=Path)
parser.add_argument('--dtb', type=Path)
args = parser.parse_args()
if args.dts and args.dtb:
    parser.error('Pass only one explicit board input')
compiler = os.environ.get('CC', 'cc')
inputs = [ROOT / 'tests/ios16_gpu_gunyah_handoff.c',
          ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuGunyah.c']
flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
         '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         '-DGG_GUNYAH_FDT_STANDALONE']
libraries = []
if args.uefi:
    vendored = args.uefi / 'Mu_Basecore/MdePkg/Library/BaseFdtLib/libfdt/libfdt'
    if not (vendored / 'libfdt.h').is_file():
        parser.error('UEFI checkout has no initialized Mu BaseFdtLib/libfdt source')
    flags += ['-I', str(vendored)]
    inputs += [vendored / name for name in ('fdt.c', 'fdt_ro.c', 'fdt_rw.c',
        'fdt_sw.c', 'fdt_wip.c', 'fdt_empty_tree.c', 'fdt_addresses.c',
        'fdt_check.c', 'fdt_strerror.c')]
else:
    libraries = ['-lfdt']
with tempfile.TemporaryDirectory(prefix='gpu-gunyah-fdt-') as folder:
    folder = Path(folder)
    binary = folder / 'handoff'
    subprocess.run([compiler, *flags, *map(str, inputs), *libraries, '-o', str(binary)], check=True)
    board = args.dtb
    if args.dts:
        board = folder / 'explicit-board.dtb'
        subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', str(board), str(args.dts)], check=True)
    environment = dict(os.environ)
    environment['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
    # LeakSanitizer is unavailable on the macOS arm64 runner. Keep ASan/UBSan
    # enabled everywhere and explicit leak detection on supported Linux hosts.
    leak_check = '0' if sys.platform == 'darwin' else '1'
    environment['ASAN_OPTIONS'] = ':'.join(filter(None,
        (environment.get('ASAN_OPTIONS', ''), 'detect_leaks=' + leak_check)))
    subprocess.run([str(binary), *([str(board)] if board else [])], check=True, env=environment)
