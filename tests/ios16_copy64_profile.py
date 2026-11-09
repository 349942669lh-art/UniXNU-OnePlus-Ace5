"""Verify the ordered, generated display patch composition (host-only)."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROFILE = Path(sys.argv[1]).resolve()
FB = 'iokit/Kernel/arm/SweetFramebuffer.cpp'
HEADER = 'iokit/Kernel/arm/giulia_display.h'
with tempfile.TemporaryDirectory(prefix='copy64-profile-') as temp:
    work = Path(temp)
    for patch in sorted(PROFILE.glob('*.patch')):
        subprocess.run(['git', 'apply', '--whitespace=nowarn',
                        '--include=' + FB, '--include=' + HEADER, str(patch)],
                       cwd=work, check=True, timeout=10)
    fb = (work / FB).read_text()
    header = (work / HEADER).read_text()
    expected = (ROOT / 'ios16/display/giulia_display.h').read_text()
    expected = expected.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
    assert header == expected
    assert fb.count('PE_parse_boot_argn("sweetfb_copy64"') == 1
    assert fb.count('fCopy64 = copy64 == 1;') == 1
    assert fb.count('gd_present_mode((void *)fPixels,') == 1
    assert fb.count('source, destination, fCopy64 ? 1U : 0U);') == 1
    # The telemetry and timing/scheduling are retained, not reimplemented.
    assert 'gfp_present(&fb->fPerf, ns);' in fb
    assert 'fVsync->setTimeoutUS(SWEET_FB_VSYNC_US);' in fb
    assert 'GIULIA FB COPY: packed64=%u (CPU only; no GPU)' in fb
    report = {'passed': True, 'default_packed64': False,
              'framebuffer_sha256': hashlib.sha256((work / FB).read_bytes()).hexdigest(),
              'header_sha256': hashlib.sha256((work / HEADER).read_bytes()).hexdigest(),
              'hardware_tested': False, 'gpu_acceleration': False}
    (PROFILE / 'copy64-host-tests.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
