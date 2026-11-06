#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 tests/ios16_gpu_vendor.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/ios16_gpu_firmware.c -o "$tmp/firmware"
"$tmp/firmware" "$@"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
    tests/ios16_gpu_runtime.c -o "$tmp/runtime"
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    "$tmp/runtime" "$(dirname "$1")/firmware.pack"
else
    "$tmp/runtime"
fi
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -pthread \
    tests/ios16_gpu_terminal.c -o "$tmp/terminal"
"$tmp/terminal"
python3 tests/ios16_gpu_terminal_profile.py
python3 tests/ios16_gpu_service.py
python3 tests/ios16_gpu_service_layout.py
python3 tests/ios16_gpu_platform_runtime.py
python3 tests/ios16_gpu_mapping_runtime.py
python3 tests/ios16_gpu_dev_runtime.py
python3 tests/ios16_gpu_ipcc_clients.py
python3 tests/ios16_gpu_ipc_raw.py
python3 tests/ios16_gpu_watch_boundary.py
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    python3 tests/ios16_gpu_host_memory.py "$(dirname "$1")/firmware.pack"
else
    python3 tests/ios16_gpu_host_memory.py
fi
python3 tests/ios16_gpu_cache.py
python3 tests/ios16_gpu_probe.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_scm.c -o "$tmp/scm"
"$tmp/scm"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_mapping.c ios16/vendor/ArmSmmu4K/ArmSmmu4K.c -o "$tmp/mapping"
"$tmp/mapping"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_memstore.c -o "$tmp/memstore"
"$tmp/memstore"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_ipc.c ios16/vendor/ArmSmmu4K/ArmSmmu4K.c -o "$tmp/ipc"
"$tmp/ipc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
    tests/ios16_gpu_ipcc_owner.c -o "$tmp/ipcc-owner"
"$tmp/ipcc-owner"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_fence.c -o "$tmp/fence"
"$tmp/fence"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_fence_mapping.c -o "$tmp/fence-mapping"
"$tmp/fence-mapping"
cc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
    tests/ios16_gpu_fence_provider.c -o "$tmp/fence-provider"
"$tmp/fence-provider"
for name in fence_queue gunyah_queries gunyah_rm gunyah_memory; do
    cc -std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation \
        -fsanitize=address,undefined -fno-sanitize-recover=all \
        "tests/ios16_gpu_$name.c" -o "$tmp/$name"
    "$tmp/$name"
done
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_GUNYAH_HVC_STANDALONE -DGG_GUNYAH_HVC_TEST \
    tests/ios16_gpu_gunyah_hvc.c ios16/gpu/giulia_gpu_gunyah_hvc.c -o "$tmp/gunyah-hvc"
"$tmp/gunyah-hvc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_GUNYAH_HVC_STANDALONE -DGG_GUNYAH_HVC_TEST -DGG_GUNYAH_HVC_NO_PROBE_HOOK \
    tests/ios16_gpu_gunyah_hvc.c ios16/gpu/giulia_gpu_gunyah_hvc.c -o "$tmp/gunyah-hvc-no-probe"
"$tmp/gunyah-hvc-no-probe"
python3 tests/ios16_gpu_probe_optional.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_GUNYAH_SESSION_STANDALONE -DGG_GUNYAH_HVC_STANDALONE -DGG_GUNYAH_HVC_TEST \
    -DGG_GUNYAH_HVC_NO_PROBE_HOOK \
    tests/ios16_gpu_gunyah_session.c ios16/gpu/giulia_gpu_gunyah_session.c \
    ios16/gpu/giulia_gpu_gunyah_hvc.c -o "$tmp/gunyah-session"
"$tmp/gunyah-session"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_GUNYAH_OWNER_STANDALONE -DGG_GUNYAH_SESSION_STANDALONE -DGG_GUNYAH_HVC_STANDALONE -DGG_GUNYAH_HVC_TEST \
    tests/ios16_gpu_gunyah_owner.c ios16/gpu/giulia_gpu_gunyah_owner.c \
    ios16/gpu/giulia_gpu_gunyah_session.c ios16/gpu/giulia_gpu_gunyah_hvc.c -o "$tmp/gunyah-owner"
"$tmp/gunyah-owner"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_GUNYAH_SHARE_STANDALONE -DGG_HOST_TEST \
    tests/ios16_gpu_gunyah_share.c ios16/gpu/giulia_gpu_gunyah_share.c -o "$tmp/gunyah-share"
"$tmp/gunyah-share"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_gunyah_irq.c -o "$tmp/gunyah-irq"
"$tmp/gunyah-irq"
python3 tests/ios16_gpu_gunyah_irq_platform.py
python3 tests/ios16_gpu_gunyah_build.py
python3 tests/ios16_gpu_gunyah_loader.py
python3 tests/ios16_gpu_gunyah_runtime.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
    tests/ios16_gpu_fence_cpu.c -o "$tmp/fence-cpu"
"$tmp/fence-cpu"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_fence_pmap.c -o "$tmp/fence-pmap"
"$tmp/fence-pmap"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_pmap.c -o "$tmp/gpu-pmap"
"$tmp/gpu-pmap"
python3 tests/ios16_gpu_pmap_patch_static.py
python3 tests/ios16_gpu_pmap_platform.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_FENCE_NATIVE_STANDALONE \
    tests/ios16_gpu_fence_native.c ios16/gpu/giulia_gpu_fence_native.c -o "$tmp/fence-native"
"$tmp/fence-native"
python3 tests/ios16_gpu_fence_native_build.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_gmu_boot.c -o "$tmp/gmu-boot"
"$tmp/gmu-boot"
python3 tests/ios16_gpu_gmu_adapter.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_platform.c -o "$tmp/platform"
if [ "$#" = 1 ]; then
    "$tmp/platform" "$(dirname "$1")/gen70900_zap.mbn"
else
    "$tmp/platform"
fi
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    tests/ios16_gpu_hfi.c -o "$tmp/hfi"
"$tmp/hfi"
for name in prepare consumer; do
    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
        "tests/ios16_gpu_power_$name.c" -o "$tmp/power-$name"
    "$tmp/power-$name"
done
python3 tests/ios16_gpu_power_handoff.py
python3 tests/ios16_gpu_qmp_transport.py
vendor=ios16/vendor/ArmSmmu4K
sources=("$vendor/ArmSmmu4K.c" "$vendor/ArmSmmu4KContext.c" "$vendor/ArmSmmu4KProgram.c" "$vendor/ArmSmmu4KStreams.c")
python3 scripts/make-gpu-smmu-oracle.py --out "$tmp/source-oracle.c"
python3 - "$vendor/ArmSmmu4KMmio.c" "$tmp/mmio-host.c" <<'PY'
from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text()
arch='#if !defined(__aarch64__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__'
barrier='__asm__ volatile("dsb sy":::"memory");'
assert source.count(arch)==1 and source.count(barrier)==1
# Host-only normal-RAM fixture: address checks/accessors stay original, while
# DSB becomes a host memory barrier. Never included in a target kernel.
Path(sys.argv[2]).write_text(source.replace(arch,'#if 0').replace(barrier,'__sync_synchronize();'))
PY
clang --target=aarch64-none-elf -ffreestanding -O2 -S "$vendor/ArmSmmu4KMmio.c" -o "$tmp/mmio-aarch64.s"
python3 - "$tmp/mmio-aarch64.s" <<'PY'
from pathlib import Path
import re,sys
text=Path(sys.argv[1]).read_text()
assert re.search(r'\bdsb\s+sy\b',text)
assert re.search(r'\bldr\s+x\d+,\s*\[',text) and re.search(r'\bstr\s+x\d+,\s*\[',text)
print('Original AArch64 MMIO source emits DSB and 64-bit TTBR accesses; host tests use a labeled RAM-only barrier shim')
PY
for name in tables context program streams; do
    clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -O1 -I "$vendor" \
        "$vendor/test_$name.c" "${sources[@]}" "$tmp/source-oracle.c" "$tmp/mmio-host.c" -o "$tmp/$name"
    "$tmp/$name"
done
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -O1 -DGG_HOST_TEST \
    tests/ios16_gpu_control.c ios16/gpu/giulia_gpu_control.c "${sources[@]}" -o "$tmp/control"
if [ "$#" = 1 ] && [ -f "$(dirname "$1")/firmware.pack" ]; then
    "$tmp/control" "$(dirname "$1")/firmware.pack" | tee "$tmp/control.log"
else
    "$tmp/control" | tee "$tmp/control.log"
fi
python3 tests/ios16_gpu_log.py "$tmp/control.log"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_submission.c ios16/gpu/giulia_gpu_runtime.c \
    ios16/gpu/giulia_gpu_submit.c -o "$tmp/submission"
"$tmp/submission"
python3 tests/ios16_gpu_submit_source.py
python3 tests/ios16_gpu_ownership_audit.py
python3 tests/ios16_gpu_submission_linkage.py
python3 tests/ios16_gpu_power_owner.py
python3 tests/ios16_gpu_parents.py
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_stack.c ios16/gpu/giulia_gpu_stack.c -o "$tmp/stack"
"$tmp/stack"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_hfi_alloc.c -o "$tmp/hfi-alloc"
"$tmp/hfi-alloc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_hfi_alloc_pool.c -o "$tmp/hfi-pool"
"$tmp/hfi-pool"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_hfi_pump.c -o "$tmp/hfi-pump"
"$tmp/hfi-pump"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST tests/ios16_gpu_share_snapshot.c -o "$tmp/share-snapshot"
"$tmp/share-snapshot"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DGG_HOST_TEST -DGG_GUNYAH_SHARE_STANDALONE \
    tests/ios16_gpu_share_runtime.c ios16/gpu/giulia_gpu_gunyah_share.c -o "$tmp/share-runtime"
"$tmp/share-runtime"
python3 tests/ios16_gpu_stack_contract.py
python3 tests/ios16_gpu_stack_linkage.py
python3 tests/ios16_gpu_hfi_wiring.py
c++ -x c++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only \
    -include ios16/gpu/giulia_gpu_pack.h -include ios16/gpu/giulia_gpu_memory.h \
    -include ios16/gpu/giulia_gpu_power.h -include ios16/gpu/giulia_gpu_control.h \
    -include ios16/gpu/giulia_gpu_stack.h -include ios16/gpu/giulia_gpu_hfi_alloc.h \
    -include ios16/gpu/giulia_gpu_hfi_alloc_pool.h -include ios16/gpu/giulia_gpu_hfi_pump.h /dev/null
python3 -m unittest discover -s tests -p 'test_gpu.py' -v

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/ios16_gpu_dev.c -o "$tmp/dev"
"$tmp/dev"
