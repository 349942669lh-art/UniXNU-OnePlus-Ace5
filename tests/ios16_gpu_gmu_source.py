"""Pinned vendor FIELD_PREP/register oracle vs actual GMU C configuration.

All register access goes to the existing RAM fixture. No firmware execution.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

from gpu_host_tools import host_link_flags

ROOT=Path(__file__).resolve().parents[1]


def main():
    pins=json.loads((ROOT/'hardware/sm8650-gpu.json').read_text())['vendor_sources']
    texts={}
    for name in ('adreno_gen7_gmu.c','gen7_reg.h'):
        matches=[p for p in pins if p['file']==name];assert len(matches)==1
        data=(ROOT/'.cache/gpu-source'/name).read_bytes()
        assert hashlib.sha256(data).hexdigest()==matches[0]['sha256'],name
        texts[name]=data.decode()
    source=texts['adreno_gen7_gmu.c']
    expression=re.search(r'gmu_core_regwrite\(device,\s*GEN7_GMU_AHB_FENCE_RANGE_0,\s*'
        r'BIT\((\d+)\)\s*\|\s*FIELD_PREP\(GENMASK\((\d+),\s*(\d+)\),\s*(0x[\da-f]+)\)\s*'
        r'\|\s*FIELD_PREP\(GENMASK\((\d+),\s*(\d+)\),\s*(0x[\da-f]+)\)\);',source,re.I)
    assert expression,'Pinned source expression drift'
    bit,hi1,lo1,val1,hi2,lo2,val2=expression.groups()
    value=1<<int(bit)
    for hi,lo,raw in ((hi1,lo1,val1),(hi2,lo2,val2)):
        hi,lo,n=int(hi),int(lo),int(raw,16)
        assert 0<=lo<=hi<32 and n<1<<(hi-lo+1)
        value|=n<<lo
    mappings={
      'GGB_FAL_INTF':'GEN7_GPU_GMU_CX_GMU_CX_FAL_INTF',
      'GGB_FALNEXT_INTF':'GEN7_GPU_GMU_CX_GMU_CX_FALNEXT_INTF',
      'GGB_BOOT_CONFIG':'GEN7_GMU_CM3_BOOT_CONFIG',
      'GGB_FW_INIT_RESULT':'GEN7_GMU_CM3_FW_INIT_RESULT',
      'GGB_CM3_CFG':'GEN7_GMU_CM3_CFG',
      'GGB_QTBL_INFO':'GEN7_GMU_HFI_QTBL_INFO',
      'GGB_QTBL_ADDR':'GEN7_GMU_HFI_QTBL_ADDR',
      'GGB_HFI_STATUS':'GEN7_GMU_HFI_CTRL_STATUS',
      'GGB_HFI_INIT':'GEN7_GMU_HFI_CTRL_INIT',
      'GGB_GENERAL_8':'GEN7_GMU_GENERAL_8',
      'GGB_GENERAL_9':'GEN7_GMU_GENERAL_9',
      'GGB_GENERAL_10':'GEN7_GMU_GENERAL_10',
      'GGB_GENERAL_11':'GEN7_GMU_GENERAL_11',
      'GGB_AHB_FENCE_RANGE':'GEN7_GMU_AHB_FENCE_RANGE_0'}
    comparisons=[]
    for ours,vendor in mappings.items():
        found=re.findall(r'^#define\s+'+vendor+r'\s+(0x[0-9a-fA-F]+)\s*$',texts['gen7_reg.h'],re.M)
        assert len(found)==1,vendor
        comparisons.append((ours,int(found[0],16)))
    fixture=ROOT/'tests/ios16_gpu_gmu_boot.c'
    body='''/* HOST fixture. Oracle values from hash-pinned GPL vendor source. */
#define main original_fixture_main
#include "'''+str(fixture)+'''"
#undef main
int main(void) {
'''
    for ours,expected in comparisons:
        body+=f' if ({ours} != {expected}u) {{ fprintf(stderr,"register offset mismatch: {ours}\\n"); return 1; }}\n'
    body+=f''' if ((uint32_t)GGB_AHB_FENCE_VALUE != 0x{value:08x}u) {{
 fprintf(stderr,"AHB fence mismatch: actual=0x%08x source=0x{value:08x}\\n",(unsigned)GGB_AHB_FENCE_VALUE); return 1; }}
 struct fixture f; fresh(&f); f.s.attempted=1;
 struct gg_gmu_ops o=make_ops(&f);
 assert(ggb_configure(&o,&f.s,&f.c)==GGM_OK);
 assert(f.regs[GGB_AHB_FENCE_RANGE]==0x{value:08x}u);
 printf("GMU source oracle: {len(comparisons)} offsets and actual fence write 0x{value:08x} verified in RAM\\n");
 return original_fixture_main();
}}
'''
    with tempfile.TemporaryDirectory(prefix='gmu-source-') as d:
        tmp=Path(d);c=tmp/'oracle-check.c';exe=tmp/'check';c.write_text(body)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g',
            *host_link_flags(),'-fsanitize=address,undefined','-fno-sanitize-recover=all',str(c),'-o',str(exe)],check=True,timeout=60)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
        print(result.stdout,end='')
        if result.returncode:
            print(result.stderr,end='')
            raise SystemExit(result.returncode)


if __name__=='__main__':
    main()
