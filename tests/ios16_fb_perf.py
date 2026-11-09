"""Check the real profile's display timing patch with deterministic host mocks.

Does not build XNU or measure hardware performance. Also validates that the
pre-telemetry framebuffer matches the known booted CI's frozen source exactly.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE = Path(sys.argv[1]).resolve()
PATCH = '0095-giulia-display-timing.patch'
FILES = ['SweetFramebuffer.cpp', 'SweetSurface.cpp', 'SweetSurface.h',
         'giulia_display.h', 'giulia_screen_framebuffer.inc',
         'giulia_display_power.h', 'giulia_display_power.inc']
FB = Path('iokit/Kernel/arm/SweetFramebuffer.cpp')
EXPECTED = '05a9ac8a7854ce4b9c39bb5b245451e5339380f6526d43fdcb31032d639e0ab5'
env = dict(os.environ)
if sys.platform.startswith('linux'):
    env['ASAN_OPTIONS'] = 'handle_segv=0'

def run(command, cwd=None):
    # Inherit output (CI log / caller's file), not an unbounded RAM capture.
    subprocess.run(command, cwd=cwd, env=env, check=True, timeout=30)

def callback(tree):
    text = (tree / FB).read_text()
    start = text.index('void\nAppleCLCD::vsyncFired(')
    end = text.index('\nvoid\nAppleCLCD::addClient(', start)
    return text[start:end]

with tempfile.TemporaryDirectory(prefix='fb-perf-') as work:
    temp = Path(work)
    base = temp / 'baseline'
    base.mkdir()
    for patch in sorted(PROFILE.glob('*.patch')):
        if patch.name in (PATCH, '0096-giulia-packed-panel-copy.patch'):
            continue
        run(['git', 'apply', '--whitespace=nowarn'] +
            ['--include=iokit/Kernel/arm/' + f for f in FILES] + [str(patch)], base)
    assert hashlib.sha256((base / FB).read_bytes()).hexdigest() == EXPECTED
    candidate = temp / 'candidate'
    shutil.copytree(base, candidate)
    run(['git', 'apply', '--check', str(PROFILE / PATCH)], candidate)
    run(['git', 'apply', str(PROFILE / PATCH)], candidate)
    includes = candidate / 'iokit/Kernel/arm'
    template = (ROOT / 'tests/ios16_fb_perf_mock.cpp.in').read_text()
    assert template.count('/* @DISPLAY_CALLBACK@ */') == 1
    for name, tree in [('baseline', base), ('candidate', candidate)]:
        code = temp / (name + '.cpp')
        code.write_text(template.replace('/* @DISPLAY_CALLBACK@ */', callback(tree)))
        binary = temp / (name + '-test')
        command = ['c++', '-std=c++11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                   '-I' + str(includes)]
        if sys.platform.startswith('linux'):
            command += ['-no-pie']
        if name == 'candidate':
            command += ['-DTELEMETRY_CANDIDATE=1']
        run(command + [str(code), '-o', str(binary)])
        run([str(binary)])
    report = {'passed': True, 'baseline_framebuffer_sha256': EXPECTED,
              'patch_sha256': hashlib.sha256((PROFILE / PATCH).read_bytes()).hexdigest(),
              'candidate_behavioral_cases': 6,
              'scope': 'Host ASan/UBSan mocks; not XNU linking, hardware timing or a speedup test.'}
    (PROFILE / 'display-timing-host-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
