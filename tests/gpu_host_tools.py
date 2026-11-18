"""Compiler selection for GPU executor fixtures on Linux and macOS runners."""
import os
import shutil
import sys


def clang_compiler(cxx=False):
    variable = 'CLANGXX' if cxx else 'CLANG'
    configured = os.environ.get(variable)
    if configured:
        return configured
    names = ('clang++-10', 'clang++') if cxx else ('clang-10', 'clang')
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    raise RuntimeError('GPU executor tests require Clang: ' + ', '.join(names))


def host_compilers():
    return os.environ.get('CC', 'cc'), clang_compiler()


def host_link_flags():
    return [] if sys.platform == 'darwin' else ['-no-pie']
