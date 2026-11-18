"""Actual GPUCC C executor with pinned public policy oracle; no hardware.

The GPL policy extraction is a disposable HOST fixture, never XNU source.
"""
import hashlib
import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from gpu_host_tools import clang_compiler, host_compilers, host_link_flags

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
from ios16_sources import kernel_source


def execute(args):
    result=subprocess.run(args,capture_output=True,timeout=60)
    if result.returncode:
        raise RuntimeError(repr(args)+'\n'+result.stderr[:8192].decode(errors='replace'))
    return result.stdout.decode()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    pins=json.loads((ROOT/'hardware/gpu-gpucc-source.json').read_text())
    texts={}
    for item in pins['files']:
        p=ROOT/'.cache/gpu-gpucc-source'/item['path']
        data=p.read_bytes()
        assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256']
        texts[p.name]=data.decode()
    source=texts['gpucc-pineapple.c'];common=texts['clk-branch.c']
    offsets=[];policies=[];aon=[]
    policy_ids={'BRANCH_HALT':0,'BRANCH_HALT_VOTED':1,'BRANCH_HALT_DELAY':2}
    for branch in pins['branches']:
        match=re.search(r'static struct clk_branch '+branch['name']+r' = \{(.*?)^\};',source,re.S|re.M)
        assert match,branch['name']
        body=match[1]
        halt=int(re.search(r'\.halt_reg\s*=\s*(0x[0-9a-f]+)',body)[1],16)
        enable=int(re.search(r'\.enable_reg\s*=\s*(0x[0-9a-f]+)',body)[1],16)
        policy=re.search(r'\.halt_check\s*=\s*(\w+)',body)[1]
        ops=re.search(r'\.ops\s*=\s*&(\w+)',body)[1]
        is_aon=ops=='clk_branch2_aon_ops'
        assert enable==halt==branch['offset'] and policy==branch['policy'] and is_aon==branch['aon']
        assert re.search(r'\.enable_mask\s*=\s*BIT\(0\)',body)
        assert '.hwcg_' not in body
        offsets.append(halt);policies.append(policy_ids[policy]);aon.append(int(is_aon))
    aon_ops=re.search(r'const struct clk_ops clk_branch2_aon_ops = \{(.*?)^\};',common,re.S|re.M)[1]
    assert '.disable' not in aon_ops and '.enable = clk_branch2_enable' in aon_ops
    assert 'if (br->halt_check == BRANCH_HALT_DELAY || (!enabling && voted))' in common
    assert 'udelay(10)' in common and 'return max(timeout, 200)' in common
    assert 'return (val & BRANCH_CLK_OFF) == 0 ||' in common
    assert 'val == BRANCH_NOC_FSM_STATUS_ON;' in common
    assert not pins['native_source_bound'] and not pins['hardware_validated']
    for path in (ROOT/'ios16').rglob('*'):
        if path.suffix in ('.c','.cpp','.h','.inc') and path.name not in ('giulia_gpu_gpucc.c','giulia_gpu_gpucc.h','giulia_gpu_board_source.c'):
            assert not re.search(r'\bggcb_(?:open|apply)\s*\(',path.read_text()),path
    c=ROOT/'ios16/gpu/giulia_gpu_gpucc.c';header=ROOT/'ios16/gpu/giulia_gpu_gpucc.h'
    outcomes=[]
    with tempfile.TemporaryDirectory(prefix='gpu-gpucc-') as folder:
        tmp=Path(folder)
        oracle='/* GPL-2.0-only: extracted public register/policy constants, HOST ONLY. */\n'
        for name,values in (('offsets',offsets),('policies',policies),('aon',aon)):
            oracle+='static const unsigned source_'+name+'[]={'+','.join(map(str,values))+'};\n'
        (tmp/'gpucc-source-policy.h').write_text(oracle)
        for cc in host_compilers():
            binary=tmp/(Path(cc).name+'-gpucc')
            execute([cc,'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
                *host_link_flags(),'-DGG_HOST_TEST','-I',str(tmp),'-I',str(header.parent),
                str(ROOT/'tests/ios16_gpu_gpucc.c'),str(c),'-o',str(binary)])
            output=execute([str(binary)]).strip();print(Path(cc).name+': '+output,flush=True)
            outcomes.append({'compiler':cc,'output':output})
        # C linkage from C++ and freestanding ARM64 Mach-O definition check.
        obj=tmp/'gpucc.o'
        execute([os.environ.get('CC','cc'),'-std=c11','-DGG_HOST_TEST','-c',str(c),'-o',str(obj)])
        caller=tmp/'caller.cpp'
        caller.write_text('''#include "giulia_gpu_gpucc.h"
#include <cassert>
#include <cstring>
int main(){ggcb_state s={};ggcb_branch b={};
assert(!std::strcmp(ggcb_identity(),GGCB_IDENTITY));
assert(ggcb_describe(0,&b)==GGCB_OK);
assert(ggcb_open(&s,nullptr)==GGCB_ARGUMENT);
assert(ggcb_apply(&s,0,1)==GGCB_USED);ggcb_invalidate(&s);}
''')
        binary=tmp/'caller'
        execute([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-I',str(header.parent),str(caller),str(obj),'-o',str(binary)])
        execute([str(binary)])
        (tmp/header.name).write_text(kernel_source(header.read_text()))
        stage=tmp/'gpucc.cpp'
        stage.write_text('typedef unsigned int uint32_t;\n'+kernel_source(c.read_text()))
        macho=tmp/'gpucc-macho.o'
        execute([os.environ.get('ISA_CXX',clang_compiler(cxx=True)),'--target=arm64-apple-macos',
            '-ffreestanding','-nostdinc++','-std=c++17','-Wall','-Wextra','-Werror','-DGG_HOST_TEST',
            '-c',str(stage),'-o',str(macho)])
        data=macho.read_bytes();assert struct.unpack_from('<I',data)[0]==0xfeedfacf
        at=32;sym=None;sections=[]
        for _ in range(struct.unpack_from('<I',data,16)[0]):
            cmd,size=struct.unpack_from('<II',data,at)
            if cmd==2:sym=struct.unpack_from('<4I',data,at+8)
            if cmd==0x19:
                for i in range(struct.unpack_from('<I',data,at+64)[0]):
                    sections.append(struct.unpack_from('<I',data,at+72+80*i+64)[0])
            at+=size
        assert sym
        offset,count,strings,string_bytes=sym;found=set()
        wanted={b'_ggcb_'+n for n in (b'identity',b'describe',b'open',b'apply',b'invalidate')}
        for i in range(count):
            index,kind,section,_,_=struct.unpack_from('<IBBHQ',data,offset+16*i)
            name=data[strings+index:data.index(b'\0',strings+index,strings+string_bytes)]
            if name in wanted:
                assert kind==0x0f and section and sections[section-1]&0x80000400
                found.add(name)
        assert found==wanted
        if args.out:
            args.out.mkdir(parents=True,exist_ok=True)
            (args.out/'gpucc-arm64-macho.o').write_bytes(data)
            (args.out/'staged-gpucc.cpp').write_text(stage.read_text())
            report={'host_outcomes':outcomes,'arm64_macho_defined_exports':sorted(n.decode() for n in found),
                    'macho_sha256':hashlib.sha256(data).hexdigest(),
                    'source_sha256':hashlib.sha256(c.read_bytes()).hexdigest(),
                    'header_sha256':hashlib.sha256(header.read_bytes()).hexdigest(),
                    'full_xnu_link_performed':False,'native_xnu_header_abi_verified':False,
                    'native_source_bound':False,'hardware_validated':False,
                    'power_only_test_admissible':False,'gpu_acceleration_enabled':False}
            (args.out/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
    print('GPUCC: 5 real C exports linked from C++ and defined in ARM64 Mach-O; no native authority/hardware execution')
    return outcomes


if __name__=='__main__':
    main()
