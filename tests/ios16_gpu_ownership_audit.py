"""Check the static board audit against source IDs and pinned metadata.

--source-dir additionally hashes downloaded originals; never downloads files,
queries a phone, calls a provider or turns the audit into a hardware grant.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-dir', type=Path)
args = parser.parse_args()
audit = json.loads((ROOT/'hardware/gpu-runtime-ownership-audit.json').read_text())
board = json.loads((ROOT/'hardware/sm8650-gpu.json').read_text())
assert audit['schema'] == 1
for field in ('runtime_provider_bound', 'submit_provider_bound',
              'hardware_validated', 'power_only_test_admissible'):
    assert audit[field] is False, field

source_meta = {source['file']: source for source in board['vendor_sources']}
assert {s['file'] for s in audit['sources']} == {
    'adreno_gen7_gmu.c', 'adreno_gen7_hwsched.c', 'gpucc-pineapple.c'}
for source in audit['sources']:
    original = source_meta[source['file']]
    assert source['url'] == original['url']
    assert source['sha256'] == original['sha256']
    if args.source_dir is not None:
        raw = (args.source_dir/source['file']).read_bytes()
        assert len(raw) == source['bytes']
        assert hashlib.sha256(raw).hexdigest() == source['sha256']
        print('Pinned power/clock primary source verified:', source['file'])

header = (ROOT/'ios16/gpu/giulia_gpu_power_control.h').read_text()
body = re.search(r'enum gg_resource\s*\{(.*?)\}', header, re.S).group(1)
body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
names = [name.strip() for name in body.split(',')]
assert names[-1] == 'GGR_COUNT'
assert [r['enum'] for r in audit['power_resources']] == names[:-1]
policy = (ROOT/'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/'
               'GiuliaInitrdDxe/GpuPowerPolicy.h').read_text()
for i, resource in enumerate(audit['power_resources']):
    assert resource['id'] == i
    assert '"'+resource['firmware_name']+'"' in policy
    expected = {'GGR_GMU_RATE': 625000000, 'GGR_HUB_RATE': 200000000}.get(resource['enum'], 1)
    assert resource['requested_value'] == expected

irq = audit['irq_description']
assert irq['gpu_spi'] == board['board']['gpu_interrupts'][1]
assert irq['gmu_spis'] == board['board']['gmu_interrupts'][1::3]
assert irq['registered_or_owned'] is False and irq['rm_irq_is_gpu_irq'] is False
assert audit['resource_exclusion']['nonsecure_sids'] == [0, 1, 5]
assert audit['resource_exclusion']['envelope_is_hardware_grant'] is False
assert audit['resource_exclusion']['gpu_mmio_envelope']['access_to_holes'] is False
assert audit['resource_exclusion']['secure_sid_2_acquired'] is False
assert audit['unchanged_device_policy']['host_only'] is True
assert not any(value for key, value in audit['unchanged_device_policy'].items()
               if key != 'host_only')
assert len(audit['blocked_on']) == 3
print('Static ownership audit: 11 resource IDs/names, pinned metadata, IRQ distinction and unavailable-provider boundary agree; no hardware proof')
