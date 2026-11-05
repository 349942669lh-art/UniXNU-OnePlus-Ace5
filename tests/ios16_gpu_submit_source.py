"""Hermetic wiring checks; optional byte-pinned primary-source audit.

No network requests. --header and --driver audit already downloaded originals,
never treat the manifest or a host test as a hardware-provider proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--header', type=Path)
p.add_argument('--driver', type=Path)
args = p.parse_args()
m = json.loads((ROOT/'hardware/gpu-submit-source.json').read_text())
assert m['commit'] == '453d8875f815bf9dfad9035982861226a4823a6c'
assert m['wire']['message_id'] == 130 and m['wire']['ib_size_unit'] == 'bytes'
assert m['wire']['submit_words'] == 8 and m['wire']['ib_words'] == 3
assert not m['hardware_validated']
for name, path in (('adreno_hfi.h', args.header), ('adreno_gen7_hwsched_hfi.c', args.driver)):
    if path:
        raw = path.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == m['sources'][name], name
        text = raw.decode()
        if name == 'adreno_hfi.h':
            def fields(struct_name):
                body = re.search(r'struct '+struct_name+r' \{(.*?)\} __packed;', text, re.S).group(1)
                return re.findall(r'\b(u32|u64)\s+(\w+)\s*;', body)
            assert fields('hfi_submit_cmd') == [('u32', n) for n in (
                'hdr', 'ctxt_id', 'flags', 'ts', 'profile_gpuaddr_lo',
                'profile_gpuaddr_hi', 'numibs', 'big_ib_gmu_va')]
            assert fields('hfi_issue_ib') == [('u64', 'addr'), ('u32', 'size')]
            assert re.search(r'H2F_MSG_ISSUE_CMD\s*=\s*130\b', text)
            assert re.search(r'#define HFI_CTXT_FLAG_NOTIFY\s+BIT\(5\)', text)
            assert re.search(r'#define HFI_DSP_ID_0\s+3\b', text)
        else:
            assert '#define HFI_DSP_IRQ_BASE 2' in text
            assert 'DISPQ_IRQ_BIT(_idx) BIT((_idx) + HFI_DSP_IRQ_BASE)' in text
            assert 'issue_ib->size = ib->size;' in text
            assert 'queue[write] = 0xfafafafa;' in text
        print('Pinned primary source verified:', name)

runner = (ROOT/'scripts/test-ios16-gpu.sh').read_text()
patch = (ROOT/'scripts/ios16_gpu_patch.py').read_text()
checker = (ROOT/'scripts/check-ios16-gpu-kernel.py').read_text()
service = (ROOT/'ios16/gpu/GiuliaGPU.cpp').read_text()
assert "glob('giulia_gpu_*.*')" in patch
for name in ('runtime', 'submit'):
    assert 'ios16/gpu/giulia_gpu_'+name+'.c' in runner
for symbol in ('runtime_acquire', 'runtime_dma', 'runtime_drain', 'submit_encode', 'submit_poll'):
    assert "'_giulia_gpu_"+symbol+"'" in checker
assert 'runtime_provider_bound=0 submit_provider_bound=0' in service
# No accidental hardware binding or debug command was installed by this change.
assert 'giulia_gpu_runtime_acquire(' not in service
assert 'giulia_gpu_submit(' not in service
assert 'policy.allow_device_writes=0' in (ROOT/'ios16/gpu/giulia_gpu_stack_impl.h').read_text()
print('Native runtime/submission staging, executable-symbol requirements and unbound host-only policy verified')
