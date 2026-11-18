"""Inspect a supplied regular-file Command DB snapshot; no device commands.

Uses the actual C decoder, compiled locally into a temporary host library.
Versions must be explicit; this tool cannot establish snapshot provenance,
current-boot pairing, power ownership, safe voltage or hardware readiness.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
MAX_BYTES=256*1024


class Policy(C.Structure):
    _fields_=[('db_version',C.c_uint32),('arc_version',C.c_uint16)]


class Table(C.Structure):
    _fields_=[('abi',C.c_uint32),('db_version',C.c_uint32),('address',C.c_uint32),
              ('arc_version',C.c_uint16),('raw_count',C.c_uint16),('count',C.c_uint16),
              ('reserved',C.c_uint16),('levels',C.c_uint16*16)]


class Plan(C.Structure):
    _fields_=[('address',C.c_uint32),('selector',C.c_uint32),('logical_level',C.c_uint32)]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--image',type=Path,required=True)
    p.add_argument('--resource',required=True)
    for name in ('db-version','arc-version','min-level','max-level'):
        p.add_argument('--'+name,type=lambda x:int(x,0),required=True)
    args=p.parse_args()
    result={'snapshot_provenance_verified':False,'current_boot_verified':False,
            'hardware_authority':False,'device_access':False,'rpmh_submitted':False,
            'safe_for_flash':False,'units':'ARC logical levels / selector, NOT microvolts'}
    try:
        if not 0<=args.db_version<=0xffffffff or not 0<=args.arc_version<=65535:
            raise ValueError('Version outside wire range')
        if not 0<=args.min_level<=args.max_level<=65535:
            raise ValueError('Logical level range must be 0..65535, not microvolts')
        name=args.resource.encode('ascii')
        if not 1<=len(name)<=8 or any(x<0x21 or x>0x7e for x in name):
            raise ValueError('Resource must be an exact 1..8-byte ASCII key')
        # No character/block devices, FIFOs, symlinks or kernel pseudo-files.
        resolved=args.image.resolve(strict=True)
        if any(resolved==Path(x) or Path(x) in resolved.parents for x in ('/dev','/sys','/proc')):
            raise ValueError('Only ordinary snapshot files may be inspected')
        fd=os.open(args.image,os.O_RDONLY|os.O_NONBLOCK|os.O_NOFOLLOW)
        try:
            s=os.fstat(fd)
            if not stat.S_ISREG(s.st_mode) or not 144<=s.st_size<=MAX_BYTES:
                raise ValueError('Snapshot must be a regular file of 144..262144 bytes')
            with os.fdopen(fd,'rb',closefd=False) as source:
                data=source.read(MAX_BYTES+1)
            if len(data)!=s.st_size:
                raise ValueError('Snapshot file changed during the bounded read')
        finally:
            os.close(fd)
        result['snapshot_sha256']=hashlib.sha256(data).hexdigest()
        with tempfile.TemporaryDirectory(prefix='gpu-arc-inspect-') as directory:
            lib=Path(directory)/('arc.dylib' if sys.platform=='darwin' else 'arc.so')
            subprocess.run([os.environ.get('CC','cc'),'-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-dynamiclib' if sys.platform=='darwin' else '-shared','-fPIC','-DGG_HOST_TEST',
                str(ROOT/'ios16/gpu/giulia_gpu_arc.c'),'-o',str(lib)],check=True,timeout=60,
                stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            decoder=C.CDLL(str(lib));decoder.ggarc_resolve.restype=C.c_int
            decoder.ggarc_resolve.argtypes=[C.c_void_p,C.c_size_t,C.POINTER(Policy),C.c_char_p,C.c_size_t,C.POINTER(Table)]
            decoder.ggarc_select.restype=C.c_int
            decoder.ggarc_select.argtypes=[C.POINTER(Table),C.c_uint32,C.c_uint32,C.POINTER(Plan)]
            image=C.create_string_buffer(data);policy=Policy(args.db_version,args.arc_version);table=Table();plan=Plan()
            code=decoder.ggarc_resolve(image,len(data),C.byref(policy),name,len(name),C.byref(table))
            result['resolve_code']=code
            if code:raise ValueError('Command DB/ARC decode refused; code='+str(code))
            result['resource']=args.resource
            result['table']={'address':hex(table.address),'db_version':table.db_version,
                             'arc_version':table.arc_version,'raw_count':table.raw_count,
                             'levels':list(table.levels[:table.count])}
            code=decoder.ggarc_select(C.byref(table),args.min_level,args.max_level,C.byref(plan))
            result['select_code']=code
            if code:raise ValueError('Logical level interval unsupported; code='+str(code))
            result['plan']={'address':hex(plan.address),'selector':plan.selector,'logical_level':plan.logical_level}
        result['ok']=True
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        result.update(ok=False,error=str(error) if not isinstance(error,subprocess.SubprocessError) else 'Host decoder compilation failed')
    print(json.dumps(result,indent=2))
    return 0 if result['ok'] else 1


if __name__=='__main__':
    sys.exit(main())
