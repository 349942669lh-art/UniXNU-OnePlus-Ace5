"""Fixed per-boot ARC resource metadata request, with post-EBS cache capture."""
import struct
from ios16_platform import read_nodes
from ios16_gpu_power import add_chosen_request

PROPERTY = 'giulia-gpu-boot-state'
MAGIC = int.from_bytes(b'GBS1', 'little')
REPORT_BYTES = 256


def placeholder():
    return struct.pack('<4I', MAGIC, 1, REPORT_BYTES, 1) + bytes(REPORT_BYTES - 16)


def check_request(blob, mode=None, kernel=None):
    nodes = read_nodes(blob)
    found = [(path, props[PROPERTY]) for path, props in nodes.items() if PROPERTY in props]
    if not found:
        return None
    if mode not in (1, 2, 3):
        raise ValueError('GPU boot state request requires the GPU service')
    if found != [('/device-tree/chosen', placeholder())]:
        raise ValueError('GPU boot state requires one pristine chosen request')
    if nodes['/device-tree'].get('physical-model') != b'OnePlus Ace 5\0':
        raise ValueError('GPU boot state requires the Ace 5 board identity')
    if kernel is not None and b'GIULIA GPU BOOT: valid=' not in kernel:
        raise ValueError('GPU boot state requires its compiled consumer')
    return {'version': 1, 'report_bytes': REPORT_BYTES, 'rails': ['cx.lvl', 'mx.lvl'],
            'post_ebs_cache_copy': True, 'native_authority_granted': False}


def add_request(blob):
    result = add_chosen_request(blob, PROPERTY, placeholder())
    check_request(result, 1)
    return result
