"""Fetch and verify public pins needed by GPU executor host tests."""
import argparse
import hashlib
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check-only', action='store_true')
    args = parser.parse_args()
    count = 0
    for name in ('gpucc', 'rcg', 'pll1', 'gcc', 'cx'):
        pins = json.loads((ROOT / f'hardware/gpu-{name}-source.json').read_text())
        for entry in pins['files']:
            path = ROOT / f'.cache/gpu-{name}-source' / entry['path']
            if path.exists():
                data = path.read_bytes()
            elif args.check_only:
                raise FileNotFoundError(path)
            else:
                with urllib.request.urlopen(entry['url'], timeout=60) as response:
                    data = response.read(entry['bytes'] + 1)
            if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise ValueError('Public executor source pin mismatch: ' + str(path))
            if not path.exists():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            count += 1
    print(f'Verified {count} public GPU executor source pins')


if __name__ == '__main__':
    main()
