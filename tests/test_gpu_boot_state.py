import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import ios16_gpu_boot_state as boot
from ios16_gpu_power import add_request as add_power_request
from ios16_platform import read_nodes

spec = importlib.util.spec_from_file_location('boot_installer', ROOT / 'scripts/enable-gpu-boot-state.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


def node(name, properties=(), children=()):
    fields = [('name', name.encode() + b'\0'), *properties]
    out = struct.pack('<II', len(fields), len(children))
    for key, value in fields:
        out += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return out + b''.join(children)


def tree():
    return node('device-tree', [('physical-model', b'OnePlus Ace 5\0')],
                [node('chosen'), node('cpus', children=[node('cpu0')])])


class BootState(unittest.TestCase):
    def test_optional_request_preserves_other_properties(self):
        original = tree()
        self.assertIsNone(boot.check_request(original))
        result = boot.add_request(original)
        nodes = read_nodes(result)
        self.assertEqual(nodes['/device-tree/chosen'].pop(boot.PROPERTY), boot.placeholder())
        self.assertEqual(nodes, read_nodes(original))
        for mode in (1, 2, 3):
            self.assertFalse(boot.check_request(result, mode, b'GIULIA GPU BOOT: valid=')['native_authority_granted'])

    def test_service_and_compiled_consumer_are_required(self):
        result = boot.add_request(tree())
        for mode in (None, 0, 4):
            with self.assertRaises(ValueError):
                boot.check_request(result, mode)
        with self.assertRaises(ValueError):
            boot.check_request(result, 1, b'old kernel')

    def test_existing_and_stale_records_are_rejected(self):
        result = boot.add_request(tree())
        with self.assertRaises(ValueError):
            boot.add_request(result)
        stale = bytearray(result)
        offset = result.index(boot.placeholder())
        stale[offset + 12] = 3
        with self.assertRaises(ValueError):
            boot.check_request(bytes(stale), 1)

    def test_power_request_remains_independent(self):
        both = boot.add_request(add_power_request(tree()))
        nodes = read_nodes(both)['/device-tree/chosen']
        self.assertIn('giulia-gpu-power', nodes)
        self.assertEqual(nodes[boot.PROPERTY], boot.placeholder())

    def test_loader_installer_is_idempotent_and_spans_ebs(self):
        text = '''#include "Uefi.h"
void Boot(void) {
  Root->Close (Root); Root = NULL;
  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
    Status = gBS->ExitBootServices (mImage, Key);
    if (!EFI_ERROR (Status)) {
      DarwinEnter (ArgsAddress, Entry, Address, WindowSize, Video, VideoBytes);
    }
Fail:
  FreePages();
}
'''
        with tempfile.TemporaryDirectory() as folder:
            driver = Path(folder)
            path = driver / 'DarwinBoot.c'
            path.write_text(text)
            installer.install(driver)
            once = path.read_bytes()
            installer.install(driver)
            self.assertEqual(once, path.read_bytes())
            result = path.read_text()
            self.assertLess(result.index(installer.PREPARE), result.index('gBS->ExitBootServices ('))
            self.assertLess(result.index('gBS->ExitBootServices ('), result.index(installer.FINISH))
            self.assertLess(result.index(installer.FINISH), result.index('      DarwinEnter ('))
            path.write_text(text.replace('if (!EFI_ERROR (Status))', 'if (EFI_ERROR (Status))'))
            with self.assertRaises(ValueError):
                installer.install(driver)

    def test_post_ebs_path_uses_no_firmware_services(self):
        text = (ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuBootStateHandoff.h').read_text()
        finalizer = text[text.index('STATIC VOID GiuliaGpuBootStateFinish'):]
        for forbidden in ('gBS->', '->Methods', 'Mmio', 'Allocate', 'DEBUG('):
            self.assertNotIn(forbidden, finalizer)


if __name__ == '__main__':
    unittest.main()
