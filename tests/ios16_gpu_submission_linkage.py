"""Compile the real C runtime/submit units and call them from C++17.

No XNU headers are faked into a native kernel; this is a host C/C++ ABI
regression, distinct from the required macOS XNU build and post-link checks.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
flags = ['-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=all', '-DGG_HOST_TEST']
with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    objects = []
    for index, name in enumerate(('ios16/gpu/giulia_gpu_runtime.c',
                                  'ios16/gpu/giulia_gpu_submit.c',
                                  'tests/ios16_gpu_submission.c')):
        obj = folder/(str(index)+'.o')
        extra = ['-Dmain=gpu_submission_host_test'] if index == 2 else []
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', *flags, *extra,
                        '-c', str(ROOT/name), '-o', str(obj)], check=True)
        objects.append(str(obj))
    driver = folder/'caller.cpp'
    driver.write_text('''#include "giulia_gpu_submit.h"
#include <cassert>
extern "C" int gpu_submission_host_test(void);
int main() {
    assert(giulia_gpu_runtime_reserve(nullptr,0)==GGRT_ARGUMENT);
    assert(giulia_gpu_runtime_cancel(nullptr)==GGRT_ARGUMENT);
    assert(giulia_gpu_runtime_acquire(nullptr,nullptr,0)==GGRT_ARGUMENT);
    assert(!giulia_gpu_runtime_live(nullptr));
    assert(giulia_gpu_runtime_dma(nullptr)==GGRT_LOST);
    assert(giulia_gpu_runtime_drain(nullptr)==GGRT_LOST);
    assert(giulia_gpu_runtime_release(nullptr)==GGRT_ARGUMENT);
    giulia_gpu_runtime_quarantine(nullptr);
    assert(giulia_gpu_submit_encode(nullptr,0,nullptr,0)==GGSUB_ARGUMENT);
    assert(giulia_gpu_submit_init(nullptr,nullptr,nullptr,nullptr)==GGSUB_ARGUMENT);
    assert(giulia_gpu_submit(nullptr,nullptr,0)==GGSUB_ARGUMENT);
    assert(giulia_gpu_submit_poll(nullptr)==GGSUB_ARGUMENT);
    giulia_gpu_submit_invalidate(nullptr);
    return gpu_submission_host_test();
}
''')
    binary = folder/'linked'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', *flags,
                    '-I', str(ROOT/'ios16/gpu'), str(driver), *objects,
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('All 13 real runtime/submission C exports link and execute from C++17; host-only evidence')
