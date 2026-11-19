"""Cross-compile the actual generated kernel patch, without executing HVC."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from ios16_gpu_patch import gpu_patch

with tempfile.TemporaryDirectory(prefix='gpu-gunyah-build-') as directory:
    folder = Path(directory)
    gpu_patch(folder, ROOT)
    for name in ('0088-giulia-platform-runtime.patch', '0077-giulia-gpu-runtime.patch'):
        subprocess.run(['git', 'apply', '--include=iokit/Kernel/arm/*',
                        str(folder / name)], cwd=folder, check=True)
    generated = folder / 'iokit/Kernel/arm'
    stub = folder / 'stub'
    # Platform headers moved to ios16/platform/ in the layout split; the GPU
    # headers include them by bare name, so that directory is part of the
    # include set for both the generated tree and the source tree.
    platform_include = str(ROOT / 'ios16' / 'platform')
    (stub / 'pexpert/arm64').mkdir(parents=True)
    (stub / 'sys').mkdir()
    (stub / 'pexpert/arm64/board_config.h').write_text('#define ARM64_BOARD_CONFIG_SWEET 1\n')
    # Only platform headers are stubs. The generated transport, session and
    # assembly are the exact sources included by the real GPU patch emitter.
    (stub / 'sys/param.h').write_text('#include <stdint.h>\n#include <stddef.h>\n')
    (stub / 'sys/systm.h').write_text('')
    for name in ('hvc', 'session', 'share', 'owner'):
        output = folder / f'{name}-darwin.s'
        subprocess.run(['clang++', '--target=arm64-apple-macos', '-ffreestanding',
                        '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O2', '-x', 'c++',
                        '-I', str(stub), '-I', str(generated), '-I', platform_include,
                        '-S', str(generated / f'giulia_gpu_gunyah_{name}.c'), '-o', str(output)], check=True)
        assembly = output.read_text()
        if name == 'hvc':
            assert re.search(r'\bhvc\s+#(?:0x0|0)\b', assembly)
            assert re.search(r'\bhvc\s+#(?:0x6013|24595)\b', assembly)
            assert len(re.findall(r'\bstr\s+x18,\s*\[x19\]', assembly)) == 2
            assert len(re.findall(r'\bldr\s+x18,\s*\[x19\]', assembly)) == 2
            assert len(re.findall(r'\bstr\s+xzr,\s*\[x19\]', assembly)) == 2
            for symbol in ('probe_compatibility','valid', 'send', 'recv', 'doorbell_valid', 'doorbell_read_clean'):
                assert re.search(r'^_ggh_hvc_' + symbol + r':', assembly, re.M)
        elif name == 'session':
            for symbol in ('init', 'init_hvc', 'probe', 'poll', 'notification', 'invalidate', 'copy_snapshot'):
                assert re.search(r'^_ggrs_' + symbol + r':', assembly, re.M)
        elif name == 'share':
            for symbol in ('init', 'run', 'invalidate'):
                assert re.search(r'^_ggsh_' + symbol + r':', assembly, re.M)
        else:
            for symbol in ('init', 'bootstrap', 'query_self', 'probe', 'poll', 'copy_snapshot', 'get_status', 'get_channel', 'invalidate'):
                assert re.search(r'^_ggown_' + symbol + r':', assembly, re.M)
        subprocess.run(['clang', '--target=aarch64-none-elf', '-ffreestanding',
                        '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-DGG_GUNYAH_HVC_STANDALONE', '-DGG_GUNYAH_SESSION_STANDALONE',
                        '-DGG_GUNYAH_SHARE_STANDALONE',
                        '-DGG_GUNYAH_OWNER_STANDALONE',
                        '-I', platform_include,
                        '-S', str(ROOT / f'ios16/gpu/giulia_gpu_gunyah_{name}.c'),
                        '-o', str(folder / f'{name}-aarch64.s')], check=True)
    smoke = folder / 'headers.cpp'
    smoke.write_text('#include <sys/param.h>\n'
                     '#include "giulia_gpu_gunyah_session.h"\n'
                     '#include "giulia_gpu_gunyah_boot.h"\n'
                     '#include "giulia_gpu_gunyah_irq.h"\n'
                     '#include "giulia_gpu_gunyah_share.h"\n'
                     '#include "giulia_gpu_fence_queue.h"\n')
    smoke.write_text(smoke.read_text()+'#include "giulia_gpu_fence_cpu.h"\n')
    smoke.write_text(smoke.read_text()+'#include "giulia_gpu_fence_pmap.h"\n')
    subprocess.run(['clang++', '--target=arm64-apple-macos', '-ffreestanding',
                    '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsyntax-only',
                    '-I', str(stub), '-I', str(generated), '-I', platform_include, str(smoke)], check=True)

print('GPU Gunyah generated kernel C++ and AArch64 C cross-compilation passed; '
      'canonical/legacy HVC instructions, x18 preservation and C linkage verified; no HVC executed')
