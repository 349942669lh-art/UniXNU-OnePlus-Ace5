"""Compile real standalone test transports as Mach-O; no hypercalls executed.

An absent optional probe must produce no undefined probe symbol, not merely
a weak reference that some Darwin linkers still reject. Full host tests also
link and execute the negative fixture and check UNAVAILABLE with zero attempts.
"""
from pathlib import Path
import os
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PROBE = b'_ggh_hvc_test_probe_invoke'

def undefined(data):
    assert struct.unpack_from('<4I', data) == (0xfeedfacf, 0x100000c, 0, 1)
    count, size = struct.unpack_from('<II', data, 16)
    at, end, symbols = 32, 32+size, None
    assert count < 128 and end <= len(data)
    for _ in range(count):
        command, amount = struct.unpack_from('<II', data, at)
        assert amount >= 8 and amount % 8 == 0 and at+amount <= end
        if command == 2:
            assert symbols is None and amount == 24
            symbols = struct.unpack_from('<4I', data, at+8)
        at += amount
    assert at == end and symbols is not None
    offset, count, strings, length = symbols
    assert offset+count*16 <= len(data) and strings+length <= len(data)
    result = {}
    for i in range(count):
        index, kind, section, flags, value = struct.unpack_from('<IBBHQ', data, offset+i*16)
        assert index < length
        stop = data.find(b'\0', strings+index, strings+length)
        assert stop >= 0
        if not kind & 0xe0 and kind & 0x0e == 0:
            result[data[strings+index:stop]] = flags
    return result

with tempfile.TemporaryDirectory(prefix='gpu-probe-macho-') as directory:
    for absent in (False, True):
        obj = Path(directory)/f'probe-{int(absent)}.o'
        cmd = [os.environ.get('CC', 'clang'), '--target=arm64-apple-macos',
               '-ffreestanding', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
               '-DGG_GUNYAH_HVC_STANDALONE', '-DGG_GUNYAH_HVC_TEST']
        if absent:
            cmd += ['-DGG_GUNYAH_HVC_NO_PROBE_HOOK']
        subprocess.run([*cmd, '-c', str(ROOT/'ios16/gpu/giulia_gpu_gunyah_hvc.c'),
                        '-o', str(obj)], check=True)
        references = undefined(obj.read_bytes())
        assert b'_ggh_hvc_test_invoke' in references
        assert b'_ggh_hvc_test_doorbell_invoke' in references
        if absent:
            assert PROBE not in references, references
        else:
            assert PROBE in references and not references[PROBE] & 0x40
print('Darwin test probe: explicit hook required when enabled; absent fixture has no undefined probe symbol; no HVC executed')
