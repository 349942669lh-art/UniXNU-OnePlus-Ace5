"""Actual RSC C core + hash-pinned Linux register layout oracle; RAM only."""
from pathlib import Path
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_sources import kernel_source

pins = json.loads((ROOT / 'hardware/gpu-rsc-source.json').read_text())
texts = {}
for entry in pins['files']:
    path = ROOT / '.cache/gpu-rsc-source' / entry['path']
    if not path.exists() and '--fetch' in sys.argv:
        with urllib.request.urlopen(entry['url'], timeout=60) as response:
            data = response.read(entry['bytes'] + 1)
        assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    data = path.read_bytes()
    assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256'], path
    texts[Path(entry['path']).name] = data.decode()
source = texts['rpmh-rsc.c']
# Original GPL array fields/constants are used ONLY as a host test oracle.
# No original Linux code, structures or fixtures are included in the XNU unit.
fields = ('RSC_DRV_TCS_OFFSET', 'RSC_DRV_CMD_WAIT_FOR_CMPL', 'RSC_DRV_CONTROL',
          'RSC_DRV_STATUS', 'RSC_DRV_CMD_ENABLE', 'RSC_DRV_CMD_MSGID',
          'RSC_DRV_CMD_ADDR', 'RSC_DRV_CMD_DATA', 'RSC_DRV_CMD_STATUS')
arrays = []
for version in ('2_7', '3_0'):
    match = re.search(r'static u32 rpmh_rsc_reg_offsets_ver_' + version + r'\[\] = \{([^}]+)\};', source)
    assert match
    values = dict(re.findall(r'\[(\w+)\]\s*=\s*(0x[0-9A-Fa-f]+|[0-9]+),', match[1]))
    arrays.append([int(values[name], 0) for name in fields])
def const(name):
    result = re.findall(r'^#define\s+' + name + r'\s+((?:BIT\([0-9]+\))|[0-9]+)\s*$', source, re.M)
    assert len(result) == 1, name
    return 1 << int(result[0][4:-1]) if result[0].startswith('BIT(') else int(result[0])
message = const('CMD_MSGID_LEN') | const('CMD_MSGID_RESP_REQ') | const('CMD_MSGID_WRITE')
done = const('CMD_STATUS_ISSUED') | const('CMD_STATUS_COMPL')
assert const('TCS_AMC_MODE_ENABLE') == 1 << 16 and const('TCS_AMC_MODE_TRIGGER') == 1 << 24
for path in (ROOT / 'ios16').rglob('*'):
    if path.suffix in ('.c', '.cpp', '.h', '.inc') and path.name not in (
            'giulia_gpu_rsc.c', 'giulia_gpu_rsc.h', 'giulia_gpu_rsc_native.cpp'):
        assert not re.search(r'\bggrsc_(open|submit|poll)\s*\(', path.read_text()), path
flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-g']
cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
with tempfile.TemporaryDirectory(prefix='gpu-rsc-') as directory:
    tmp = Path(directory)
    oracle = tmp / 'rsc-source-layout.h'
    oracle.write_text('/* GPL-2.0-only: host-only pinned Linux register layout oracle. */\n' +
                     '\n'.join('static const unsigned source_v' + version + '[]={' + ','.join(map(str, values)) + '};'
                               for version, values in zip(('27', '30'), arrays)) +
                     f'\nstatic const unsigned source_message={message},source_done={done};\n')
    obj = tmp / 'rsc.o'
    subprocess.run([cc, '-std=c11', *flags, '-DGG_HOST_TEST', '-c', str(ROOT / 'ios16/gpu/giulia_gpu_rsc.c'), '-o', str(obj)], check=True)
    binary = tmp / 'test'
    subprocess.run([cc, '-std=c11', *flags, '-I', str(tmp), str(ROOT / 'tests/ios16_gpu_rsc.c'), str(obj), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=60)
    caller = tmp / 'caller.cpp'
    caller.write_text('''#include "giulia_gpu_rsc.h"
#include <cassert>
#include <cstring>
int main(){ggrsc_state s={};uint64_t serial=0;
assert(std::strcmp(ggrsc_identity(),GGRSC_IDENTITY)==0);
assert(ggrsc_open(&s,nullptr,nullptr)==GGRSC_ARGUMENT);
assert(ggrsc_submit(&s,0,&serial)==GGRSC_ARGUMENT);
assert(ggrsc_poll(&s,1)==GGRSC_ARGUMENT);
assert(ggrsc_close(&s)==GGRSC_ARGUMENT);ggrsc_invalidate(&s);}
''')
    subprocess.run([cxx, '-std=c++17', *flags, '-I', str(ROOT / 'ios16/gpu'), str(caller), str(obj), '-o', str(tmp / 'caller')], check=True)
    subprocess.run([str(tmp / 'caller')], check=True, timeout=10)
    # XNU compiles these .c files as C++. This freestanding header fixture is
    # only syntax/linkage evidence, not a full native kernel build/ABI proof.
    (tmp / 'giulia_gpu_rsc.h').write_text(kernel_source((ROOT / 'ios16/gpu/giulia_gpu_rsc.h').read_text()))
    staged = tmp / 'rsc.cpp'
    staged.write_text('typedef unsigned int uint32_t;typedef unsigned long long uint64_t;\n' +
                      kernel_source((ROOT / 'ios16/gpu/giulia_gpu_rsc.c').read_text()))
    macho = tmp / 'rsc-macho.o'
    subprocess.run([os.environ.get('ISA_CXX', 'clang++'), '--target=arm64-apple-macos', '-ffreestanding',
                    '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DGG_HOST_TEST', '-c', str(staged), '-o', str(macho)], check=True)
    data = macho.read_bytes()
    assert struct.unpack_from('<I', data)[0] == 0xfeedfacf
    count, cmd_bytes = struct.unpack_from('<II', data, 16)
    at, symtab, sections = 32, None, []
    for _ in range(count):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 2:
            symtab = struct.unpack_from('<4I', data, at + 8)
        if cmd == 0x19:
            nsects = struct.unpack_from('<I', data, at + 64)[0]
            sections += [struct.unpack_from('<I', data, at + 72 + 80*i + 64)[0] for i in range(nsects)]
        at += size
    assert at == 32 + cmd_bytes and symtab
    offset, count, strings, string_bytes = symtab
    required = {('_ggrsc_' + name).encode() for name in ('identity', 'open', 'submit', 'poll', 'close', 'invalidate')}
    found = set()
    for i in range(count):
        index, kind, section, _, value = struct.unpack_from('<IBBHQ', data, offset + 16*i)
        name = data[strings+index:data.index(b'\0', strings+index, strings+string_bytes)]
        if name in required:
            assert kind == 0x0f and 1 <= section <= len(sections) and sections[section-1] & 0x80000400
            found.add(name)
    assert found == required
print('GPU RPMh RSC: pinned layout/6 C exports/C++ caller/ARM64 Mach-O passed; guarded native executor is the only allowed caller; authority UNBOUND')
