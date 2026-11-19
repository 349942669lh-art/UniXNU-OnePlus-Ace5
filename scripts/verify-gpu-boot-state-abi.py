"""Check the exact firmware modules whose private cache layout is captured."""
import argparse
import hashlib
import json
import re
from pathlib import Path

PINS = {
    'RpmhDxe': 'cdebce5c9f96d16e71a0db8f9c4dbf591ed314dd94a6543b08c238902df65267',
    'CmdDbDxe': '5c58d445f356af9f8497ea6f449ba534fd7920b045067cd3b02ad997863077bf',
}


def verify(uefi):
    modules = {}
    for name, expected in PINS.items():
        directory = uefi / 'Binaries/giulia/QcomPkg/Drivers' / name
        files = re.findall(r'^\s*PE32\|([^|\r\n]+)\|', (directory / (name + '.inf')).read_text(), re.M)
        if len(files) != 1:
            raise ValueError('Ambiguous snapshot firmware module ' + name)
        path = (directory / files[0]).resolve()
        if not path.is_relative_to(directory.resolve()):
            raise ValueError('Snapshot firmware path escapes module')
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest != expected:
            raise ValueError('Snapshot firmware layout requires audit for ' + name + ' ' + digest)
        modules[name] = digest
    return {'modules': modules, 'report_bytes': 256, 'cache_copy_after_ebs': True,
            'boot_services_calls_after_ebs': 0, 'hardware_ownership_granted': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uefi', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(verify(args.uefi), indent=2))
