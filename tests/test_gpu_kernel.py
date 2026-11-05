import runpy
import struct
import unittest
from pathlib import Path


MODULE = runpy.run_path(str(Path(__file__).resolve().parents[1] /
                           'scripts/check-ios16-gpu-kernel.py'))
check_kernel = MODULE['check_kernel']


class LinkedGPU(unittest.TestCase):
    def setUp(self):
        self.kernel = bytearray(0x2000)
        struct.pack_into('<8I', self.kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 176, 0, 0)
        self.base = 0xfffffe0007004000
        struct.pack_into('<II16sQQQQiiII', self.kernel, 32, 0x19, 152, b'__TEXT_EXEC',
                         self.base, len(self.kernel), 0, len(self.kernel), 5, 5, 1, 0)
        struct.pack_into('<16s16sQQIIIIIIII', self.kernel, 104, b'__text', b'__TEXT_EXEC',
                         self.base + 0x200, 0x200, 0x200, 2, 0, 0, 0x80000400, 0, 0, 0)
        strings = b'\0' + b'\0'.join(s.encode() for s in MODULE['SYMBOLS']) + b'\0'
        self.strings_at = (0x400 + 16 * len(MODULE['SYMBOLS']) + 255) & ~255
        struct.pack_into('<6I', self.kernel, 184, 2, 24, 0x400, len(MODULE['SYMBOLS']), self.strings_at, len(strings))
        self.kernel[self.strings_at:self.strings_at + len(strings)] = strings
        for i, symbol in enumerate(MODULE['SYMBOLS']):
            struct.pack_into('<IBBHQ', self.kernel, 0x400 + 16 * i,
                             strings.index(symbol.encode()+b'\0'), 0x0f, 1, 0, self.base + 0x200 + 4 * i)
        markers = b'\0'.join(MODULE['MARKERS'])
        markers_at = (self.strings_at + len(strings) + 255) & ~255
        assert markers_at + len(markers) <= len(self.kernel)
        self.kernel[markers_at:markers_at + len(markers)] = markers

    def test_linked_preparation_is_not_acceleration(self):
        result = check_kernel(self.kernel)
        self.assertFalse(result['acceleration'])
        self.assertFalse(result['hardware_verified'])

    def test_legacy_gpu_names_cannot_replace_shared_platform_ownership(self):
        replacements = {
            '_giulia_platform_qmp_available': '_giulia_gpu_qmp_available',
            '_gpr_global_legacy_ipcc_claim': '_giulia_gpu_ipcc_claim',
            '_gpr_global_legacy_ipcc_owns': '_giulia_gpu_ipcc_owns',
            '_gpr_global_legacy_ipcc_release': '_giulia_gpu_ipcc_release',
        }
        for required, obsolete in replacements.items():
            self.assertIn(required, MODULE['SYMBOLS'])
            self.assertNotIn(obsolete, MODULE['SYMBOLS'])
            kernel = bytearray(self.kernel)
            at = kernel.index(required.encode())
            replacement = obsolete.encode() + b'\0'
            kernel[at:at + len(replacement)] = replacement
            with self.subTest(symbol=required), self.assertRaisesRegex(ValueError, required):
                check_kernel(kernel)

    def test_undefined_or_local_controls_do_not_pass_link_verification(self):
        for i, symbol in enumerate(MODULE['SYMBOLS']):
            for kind in (0x01, 0x0e, 0x03, 0xef):
                kernel = bytearray(self.kernel)
                kernel[0x400 + 16 * i + 4] = kind
                with self.subTest(symbol=symbol, kind=kind), self.assertRaises(ValueError):
                    check_kernel(kernel)

    def test_foreign_or_truncated_kernel_is_rejected(self):
        for kernel in (b'', self.kernel[:16], bytes(32) + self.kernel[32:]):
            with self.assertRaises(ValueError):
                check_kernel(kernel)

    def test_display_and_firmware_mapping_are_required(self):
        for marker in MODULE['MARKERS']:
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                check_kernel(self.kernel.replace(marker, bytes(len(marker))))

    def test_data_or_outside_section_is_rejected(self):
        for mode in range(5):
            kernel = bytearray(self.kernel)
            if mode == 0:
                struct.pack_into('<I', kernel, 32 + 60, 1)  # Read-only data segment.
            elif mode == 1:
                struct.pack_into('<I', kernel, 104 + 64, 0)  # No instruction attributes.
            elif mode == 2:
                struct.pack_into('<Q', kernel, 0x400 + 8, self.base + 0x400)
            elif mode == 3:
                kernel[0x400 + 5] = 2  # No such section.
            else:
                struct.pack_into('<I', kernel, 104 + 48, 0x300)  # Inconsistent file mapping.
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_kernel(kernel)

    def test_malformed_macho_bounds_are_rejected(self):
        for mode in range(5):
            kernel = bytearray(self.kernel)
            if mode == 0:
                struct.pack_into('<I', kernel, 20, len(kernel))
            elif mode == 1:
                struct.pack_into('<I', kernel, 184 + 8, len(kernel))
            elif mode == 2:
                struct.pack_into('<I', kernel, 184 + 20, len(kernel))
            elif mode == 3:
                struct.pack_into('<I', kernel, 0x400, 0xffffffff)
            else:
                struct.pack_into('<I', kernel, 32 + 4, 71)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_kernel(kernel)


if __name__ == '__main__':
    unittest.main()
