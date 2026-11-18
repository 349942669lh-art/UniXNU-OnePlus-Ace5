"""Catch Linux-only compiler assumptions before dispatching the macOS build."""
import unittest
from unittest.mock import patch
import gpu_host_tools as tools


class HostTools(unittest.TestCase):
    def test_darwin_uses_available_clang(self):
        with patch.dict(tools.os.environ, {}, clear=True), patch.object(tools.shutil, 'which', side_effect=lambda name: '/usr/bin/clang' if name == 'clang' else None):
            self.assertEqual(tools.host_compilers(), ('cc', '/usr/bin/clang'))

    def test_explicit_compilers(self):
        with patch.dict(tools.os.environ, {'CC': 'gcc-custom', 'CLANG': 'clang-custom'}, clear=True):
            self.assertEqual(tools.host_compilers(), ('gcc-custom', 'clang-custom'))

    def test_cxx_fallback(self):
        with patch.dict(tools.os.environ, {}, clear=True), patch.object(tools.shutil, 'which', side_effect=lambda name: '/usr/bin/clang++' if name == 'clang++' else None):
            self.assertEqual(tools.clang_compiler(cxx=True), '/usr/bin/clang++')

    def test_missing_clang_is_an_error(self):
        with patch.dict(tools.os.environ, {}, clear=True), patch.object(tools.shutil, 'which', return_value=None):
            with self.assertRaises(RuntimeError):
                tools.clang_compiler()

    def test_platform_link_flags(self):
        with patch.object(tools.sys, 'platform', 'darwin'):
            self.assertEqual(tools.host_link_flags(), [])
        with patch.object(tools.sys, 'platform', 'linux'):
            self.assertEqual(tools.host_link_flags(), ['-no-pie'])
