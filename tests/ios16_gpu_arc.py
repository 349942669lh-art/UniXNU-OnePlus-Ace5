"""Build real decoder + pinned original Linux host oracle; no hardware access."""
from pathlib import Path
import json
import re
import os
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_sources import kernel_source
for path in (ROOT/'ios16').rglob('*'):
    if path.suffix in ('.c','.cpp','.h','.inc') and not path.name.startswith('giulia_gpu_arc.'):
        assert not re.search(r'\bggarc_(resolve|select)\s*\(',path.read_text()),path
flags=['-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-g']
cc,cxx=os.environ.get('CC','cc'),os.environ.get('CXX','c++')
with tempfile.TemporaryDirectory(prefix='gpu-arc-') as directory:
    tmp=Path(directory)
    oracle=tmp/'oracle.c'
    subprocess.run([sys.executable,str(ROOT/'scripts/make-gpu-arc-oracle.py'),'--out',str(oracle)],check=True)
    obj=tmp/'oracle.o'
    # Original Linux unsigned-char strncpy argument; no production warnings suppressed.
    subprocess.run([cc,'-std=c11',*flags,'-Wno-pointer-sign','-c',str(oracle),'-o',str(obj)],check=True)
    core=tmp/'arc.o'
    subprocess.run([cc,'-std=c11',*flags,'-DGG_HOST_TEST','-c',str(ROOT/'ios16/gpu/giulia_gpu_arc.c'),'-o',str(core)],check=True)
    binary=tmp/'arc'
    subprocess.run([cc,'-std=c11',*flags,str(ROOT/'tests/ios16_gpu_arc.c'),str(core),str(obj),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True,timeout=60)
    caller=tmp/'caller.cpp'
    caller.write_text('''#include "giulia_gpu_arc.h"
#include <cassert>
#include <cstring>
int main(){ggarc_table t={};ggarc_plan p={};
assert(std::strcmp(ggarc_identity(),GGARC_IDENTITY)==0);
assert(ggarc_resolve(nullptr,0,nullptr,nullptr,0,&t)==GGARC_ARGUMENT);
assert(ggarc_select(&t,0,1,&p)==GGARC_ARGUMENT);}
''')
    caller_bin=tmp/'caller'
    subprocess.run([cxx,'-std=c++17',*flags,'-I',str(ROOT/'ios16/gpu'),str(caller),str(core),'-o',str(caller_bin)],check=True)
    subprocess.run([str(caller_bin)],check=True)
    # Real module is compiled as C++ by XNU. Use staged integer types/headers
    # only for this syntax/Mach-O check, NOT as native XNU header/ABI evidence.
    (tmp/'giulia_gpu_arc.h').write_text(kernel_source((ROOT/'ios16/gpu/giulia_gpu_arc.h').read_text()))
    staged=tmp/'arc.cpp'
    staged.write_text('typedef unsigned char uint8_t;typedef unsigned short uint16_t;\n'
                      'typedef unsigned int uint32_t;typedef __SIZE_TYPE__ size_t;\n'
                      +kernel_source((ROOT/'ios16/gpu/giulia_gpu_arc.c').read_text()))
    macho=tmp/'arc-macho.o'
    subprocess.run([os.environ.get('ISA_CXX','clang++'),'--target=arm64-apple-macos','-ffreestanding',
                    '-std=c++17','-Wall','-Wextra','-Werror','-DGG_HOST_TEST','-c',str(staged),'-o',str(macho)],check=True)
    sys.path.insert(0,str(ROOT/'tests'))
    # Parse symbols independently: all three must be external executable N_SECT,
    # not undefined strings or C++-mangled declarations.
    import struct
    data=macho.read_bytes();assert struct.unpack_from('<I',data)[0]==0xfeedfacf
    ncmds,command_bytes=struct.unpack_from('<II',data,16);at=32;symtab=None;sections=[]
    for _ in range(ncmds):
        command,size=struct.unpack_from('<II',data,at)
        if command==2:symtab=struct.unpack_from('<4I',data,at+8)
        if command==0x19:
            nsects=struct.unpack_from('<I',data,at+64)[0]
            for i in range(nsects):
                section=at+72+80*i
                sections.append(struct.unpack_from('<I',data,section+64)[0])
        at+=size
    assert at==32+command_bytes and symtab
    offset,count,strings,string_bytes=symtab;found=set()
    for i in range(count):
        index,kind,section,_,value=struct.unpack_from('<IBBHQ',data,offset+16*i)
        name=data[strings+index:data.index(b'\0',strings+index,strings+string_bytes)]
        if name in (b'_ggarc_identity',b'_ggarc_resolve',b'_ggarc_select'):
            assert kind==0x0f and 1<=section<=len(sections) and sections[section-1]&0x80000400
            found.add(name)
    assert len(found)==3
print('GPU ARC: all 3 real C exports called from C++ and defined in ARM64 Mach-O; no hardware authority/transport')

# Offline CLI exercises the same actual C decoder; all inputs synthetic files.
with tempfile.TemporaryDirectory(prefix='gpu-arc-cli-') as directory:
    import struct
    tmp=Path(directory);image=tmp/'synthetic-cmd-db.bin'
    blob=bytearray(256);struct.pack_into('<I4s',blob,0,1,bytes.fromhex('db30030c'))
    struct.pack_into('<8H',blob,8,3,0,24,1,0x100,0,0,0)
    struct.pack_into('<8sIIIHH',blob,144,b'mxa.lvl',0,0,0x30000,16,0)
    struct.pack_into('<8H',blob,168,0,16,48,80,128,0,0,0);image.write_bytes(blob)
    command=[sys.executable,str(ROOT/'scripts/inspect-ios16-gpu-arc.py'),'--image',str(image),
             '--resource','mxa.lvl','--db-version','1','--arc-version','0x100',
             '--min-level','72','--max-level','100']
    def call(args,ok):
        result=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=90)
        assert result.returncode==(0 if ok else 1),result.stderr
        report=json.loads(result.stdout);assert report['ok']==ok
        for key in ('snapshot_provenance_verified','current_boot_verified','hardware_authority',
                    'device_access','rpmh_submitted','safe_for_flash'):
            assert report[key] is False
        return report
    good=call(command,True)
    assert good['plan']=={'address':'0x30000','selector':3,'logical_level':80}
    bad=list(command);bad[bad.index('--db-version')+1]='2'
    assert call(bad,False)['resolve_code']==4
    bad=list(command);bad[bad.index('--max-level')+1]='600000'
    assert 'not microvolts' in call(bad,False)['error']
    bad=list(command);bad[bad.index('--image')+1]='/dev/null'
    assert 'ordinary snapshot' in call(bad,False)['error']
    bad=list(command);bad[bad.index('--image')+1]=str(tmp)
    assert 'regular file' in call(bad,False)['error']
print('GPU ARC offline CLI: 5 synthetic-file cases passed; no device commands or power votes')
