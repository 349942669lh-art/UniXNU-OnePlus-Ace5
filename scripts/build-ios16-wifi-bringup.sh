#!/usr/bin/env bash
# Build against an existing, prepared Darwin22 UEFI checkout. No device access.
# GIULIA_WIFI_SNAPSHOT=1 substitutes a read-only register dump for failed
# link-training handoffs; decode it with decode-ios16-pcie-snapshot.py.
# GIULIA_WIFI_XNU_LINK=1 retains prepared resources for XNU link training.
# GIULIA_GPU_PACK opts into the pinned local GPU pack; GIULIA_GPU_MODE=1/2/3
# selects host preparation, observation or UEFI CX/clock preparation (default: 1).
set -euo pipefail
if [ "$#" -ne 8 ] && [ "$#" -ne 9 ]; then
  echo 'Usage: build-ios16-wifi-bringup.sh UEFI UEFI_PYTHON KERNEL ADT ROOT_REPORT TRUST_CATALOG CAPTURED_DTS OUTPUT [LOCAL_FIRMWARE_PACK]' >&2
  exit 2
fi
port_root="$(cd "$(dirname "$0")/.." && pwd)"
uefi_root="$(realpath "$1")"
# Preserve a venv interpreter symlink: resolving it selects system Python.
uefi_python="$(realpath -s "$2")"
kernel_input="$(realpath "$3")"
adt_input="$(realpath "$4")"
root_report="$(realpath "$5")"
trust_input="$(realpath "$6")"
dts_input="$(realpath "$7")"
output_dir="$(realpath -m "$8")"
runtime_pack=""
if [ "$#" -eq 9 ]; then runtime_pack="$(realpath "$9")"; fi
gpu_pack="${GIULIA_GPU_PACK:-}"
gpu_mode="${GIULIA_GPU_MODE:-1}"
gpu_hostonly="${GIULIA_GPU_HOSTONLY:-0}"
case "$gpu_hostonly" in 0|1) ;; *) echo 'GIULIA_GPU_HOSTONLY must be 0 or 1' >&2; exit 2;; esac
if [ "$gpu_hostonly" = 1 ] && { [ -z "$gpu_pack" ] || [ "$gpu_mode" != 1 ]; }; then
  echo 'GIULIA_GPU_HOSTONLY requires GPU mode 1 and an explicit pack' >&2
  exit 2
fi
case "$gpu_mode" in 1|2|3) ;; *) echo 'GIULIA_GPU_MODE must be 1, 2 or 3' >&2; exit 2;; esac
if [ -n "$gpu_pack" ]; then gpu_pack="$(realpath "$gpu_pack")"; fi
if [ -n "${GIULIA_GPU_MODE:-}" ] && [ -z "$gpu_pack" ]; then
  echo 'GIULIA_GPU_MODE requires explicit GIULIA_GPU_PACK opt-in' >&2
  exit 2
fi
driver_dir="$uefi_root/UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver"
board_driver="$uefi_root/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe"
python3 - "$port_root" "$kernel_input" "$runtime_pack" "$gpu_pack" <<'PY'
import hashlib,json,re,runpy,sys
from pathlib import Path
root,kernel_path=map(Path,sys.argv[1:3])
wifi_pack,gpu_pack=sys.argv[3:]
kernel=kernel_path.read_bytes()
metadata=json.loads(kernel_path.with_suffix('.json').read_text())
compiled=b'GIULIA GPU: host preparation complete;' in kernel
if (compiled or metadata.get('gpu_runtime_included')) and not gpu_pack:
    raise SystemExit('Compiled GPU driver requires GIULIA_GPU_PACK; GIULIA_GPU_MODE defaults to 1')
if gpu_pack:
    if not compiled:
        raise SystemExit('GIULIA_GPU_PACK requires a kernel compiled with --gpu')
    if b'GIULIA GPU: boot Gunyah resources' not in kernel:
        raise SystemExit('GPU kernel requires the Gunyah boot-resource consumer')
    if __import__('os').environ.get('GIULIA_GPU_MODE')=='3' and b'GIULIA GPU: UEFI power handoff' not in kernel:
        raise SystemExit('GPU mode 3 requires the compiled UEFI power handoff consumer')
    if metadata.get('output_sha256')!=hashlib.sha256(kernel).hexdigest() or metadata.get('boot_args_bytes')!=736:
        raise SystemExit('Kernel handoff metadata mismatch')
    check_kernel=runpy.run_path(str(root/'scripts/check-ios16-gpu-kernel.py'))['check_kernel']
    check_kernel(kernel)
    # Validate the exact insertion order, both pinned packs and header capacity
    # before tests, loader edits or the expensive UEFI build.
    for name,path in (('wifi',wifi_pack),('gpu',gpu_pack)):
        if not path:
            continue
        pin=(root/f'ios16/{name}/giulia_{name}_firmware_pin.h').read_text()
        digest=bytes(int(x,16) for x in re.findall(r'0x([0-9a-f]{2})\b',pin))
        append=runpy.run_path(str(root/f'scripts/embed-{name}-firmware.py'))['append_pack']
        kernel,_=append(kernel,Path(path).read_bytes(),digest)
PY

test -x "$uefi_python"
"$uefi_python" -c 'import coloredlogs'
test ! -e "$output_dir"
python3 - "$kernel_input" <<'PY'
import os,sys
from pathlib import Path
if os.environ.get('GIULIA_WIFI_XNU_LINK')=='1':
    kernel=Path(sys.argv[1]).read_bytes()
    if any(marker not in kernel for marker in (b'GIULIA WIFI: XNU PCIe link worker',b'sw-ctrl-pdc-route',b'GWL_LINK_TIMEOUT')):
        raise SystemExit('XNU link handoff requires the PCIe worker with PDC-route checks and named errors')
PY
python3 - "$driver_dir" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
for name in ('BootArgs.h','DarwinBoot.c'):
    text=(p/name).read_text()
    if 'CommandLine[608]' not in text:
        raise SystemExit('Requires the prepared Darwin22 loader: '+name)
PY
mkdir -p "$(dirname "$output_dir")"
mkdir "$output_dir"
exec > >(tee "$output_dir/host-build.log") 2>&1
cd "$port_root"
if [ -n "$runtime_pack" ]; then
  python3 scripts/fix-ios16-high-physical-cache.py --kernel "$kernel_input" --check
fi
runtime_checks=()
terminal_args=()
gpu_args=()
wifi_args=()
if [ -n "$runtime_pack" ]; then wifi_args=(--wifi-pcie); fi
if [ -n "$gpu_pack" ]; then gpu_args=(--gpu-mode "$gpu_mode"); fi
if [ "$gpu_hostonly" = 1 ]; then gpu_args+=(--gpu-hostonly); fi
if [ "${GIULIA_DEBUG_TERMINAL:-0}" = 1 ]; then terminal_args+=(--debug-terminal); fi
if [ "${GIULIA_DEBUG_MMIO:-0}" = 1 ]; then terminal_args+=(--debug-mmio); fi
if [ "${GIULIA_WIFI_TERMINAL_WAIT:-0}" = 1 ]; then terminal_args+=(--wifi-terminal-wait); fi
if [ -n "$runtime_pack" ]; then runtime_checks=(--runtime); fi
python3 tests/ios16_wifi_bundle.py --dts "$dts_input" --out "$output_dir/offline-checks.json" "${runtime_checks[@]}"
if [ -n "$gpu_pack" ]; then
  python3 - "$gpu_pack" "$output_dir/gpu-offline-checks.json" <<'PY'
import json,subprocess,sys
from pathlib import Path
checks=[]
for command in (['bash','scripts/test-ios16-gpu.sh'],
                [sys.executable,'tests/ios16_gpu_patch.py'],
                [sys.executable,'tests/ios16_gpu_embed.py',sys.argv[1]]):
    result=subprocess.run(command,capture_output=True,text=True)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    result.check_returncode()
    checks.append({'command':command,'result':'passed','output':result.stdout.strip()})
Path(sys.argv[2]).write_text(json.dumps({'status':'offline-checks-passed','hardware_verified':False,'checks':checks},indent=2)+'\n')
PY
fi
if [ -n "$runtime_pack" ]; then
  wifi_kernel="$output_dir/kernel.macho"
  if [ -n "$gpu_pack" ]; then wifi_kernel="$output_dir/kernel-wifi.macho"; fi
  python3 scripts/embed-wifi-firmware.py --kernel "$kernel_input" --pack "$runtime_pack" --out "$wifi_kernel"
  if [ -n "$gpu_pack" ]; then
    python3 scripts/embed-gpu-firmware.py --kernel "$wifi_kernel" --pack "$gpu_pack" --out "$output_dir/kernel.macho"
  fi
elif [ -n "$gpu_pack" ]; then
  python3 scripts/embed-gpu-firmware.py --kernel "$kernel_input" --pack "$gpu_pack" --out "$output_dir/kernel.macho"
else
  cp "$kernel_input" "$output_dir/kernel.macho"
  cp "${kernel_input%.*}.json" "$output_dir/kernel.json"
fi
if [ -n "$gpu_pack" ]; then
  python3 scripts/prepare-ios16-gpu-gunyah.py --adt "$adt_input" --out "$output_dir/gunyah-request.adt"
  if [ "$gpu_mode" = 3 ]; then
    python3 scripts/prepare-ios16-gpu-power.py --adt "$output_dir/gunyah-request.adt" --out "$output_dir/giulia-ios16.adt"
  else
    cp "$output_dir/gunyah-request.adt" "$output_dir/giulia-ios16.adt"
  fi
else
  cp "$adt_input" "$output_dir/giulia-ios16.adt"
fi
cp "$dts_input" "$output_dir/captured-board.dts"
cp -a uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/. "$board_driver/"
cp -a uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe "$output_dir/driver-source"
python3 scripts/enable-touch-diagnostics.py "$driver_dir"
python3 scripts/enable-wifi-diagnostics.py "$driver_dir"
if [ -n "$gpu_pack" ]; then
  python3 scripts/enable-gpu-gunyah-handoff.py "$driver_dir"
fi
if [ -n "$gpu_pack" ] && [ "$gpu_mode" = 3 ]; then
  python3 scripts/enable-gpu-power-diagnostics.py "$driver_dir"
fi
python3 scripts/make-ios16-payload.py --kernel "$output_dir/kernel.macho" \
  --adt "$output_dir/giulia-ios16.adt" --trust-catalog "$trust_input" \
  --out "$output_dir/payload" --root-report "$root_report" \
  --default-root --trace-all "${wifi_args[@]}" --enable-touch --touch-gpio "${terminal_args[@]}" "${gpu_args[@]}"
python3 scripts/verify-payload.py "$output_dir/payload"
python3 - "$output_dir" <<'PY'
from pathlib import Path
import shutil,sys
out=Path(sys.argv[1]);snapshot=out/'runtime-source'
files=[*Path('ios16/wifi').glob('giulia_wifi_*.h'),*Path('ios16/wifi').glob('giulia_wifi_*.c'),Path('ios16/wifi/GiuliaWiFi.cpp'),
       Path('ios16/display/giulia_display.h'),Path('scripts/ios16_display.py')]
files += [*Path('ios16/common').glob('*.h'), *Path('ios16/platform').glob('*'),
          Path('scripts/ios16_sources.py'), Path('scripts/ios16_platform_patch.py'),
          Path('scripts/ios16_modules.py'), Path('scripts/ios16_module_registry.py')]
if (out/'gpu-offline-checks.json').exists():
    files += [Path('ios16/gpu/GiuliaGPU.cpp'),*Path('ios16/gpu').glob('giulia_gpu_*.c'),
              *Path('ios16/gpu').glob('giulia_gpu_*.cpp'),
              *Path('ios16/gpu').glob('giulia_gpu_*.h'),*Path('ios16/vendor').rglob('*'),
              Path('scripts/ios16_gpu_patch.py'),Path('scripts/ios16_gpu_runtime.py'),
              Path('scripts/embed-gpu-firmware.py'),Path('scripts/ios16_gpu_power.py'),
              Path('scripts/ios16_gpu_gunyah.py'),Path('scripts/enable-gpu-gunyah-handoff.py'),
              Path('scripts/prepare-ios16-gpu-gunyah.py'),
              Path('scripts/check-gpu-gunyah-uefi.py'),Path('scripts/ios16_gpu_gunyah_irq_patch.py'),
              Path('ios16/0085-giulia-gunyah-irq.patch'),
              Path('scripts/ios16_gpu_pmap_patch.py'),Path('ios16/0086-giulia-gpu-pmap.patch'),
              Path('scripts/enable-gpu-power-diagnostics.py'),Path('scripts/prepare-ios16-gpu-power.py')]
for source in sorted(set(files)):
    if not source.is_file():continue
    destination=snapshot/source
    destination.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(source,destination)
PY
python3 tests/ios16_dram_handoff.py --driver "$driver_dir" --adt "$output_dir/giulia-ios16.adt"
python3 - "$output_dir" "$board_driver" <<'PY'
from pathlib import Path
import json,os,sys
out,driver=map(Path,sys.argv[1:])
r=json.loads((out/'payload/identity.json').read_text())
(driver/'PayloadIdentity.h').write_text(
    f"#define GIULIA_PAYLOAD_BYTES {r['payload_bytes']}U\n"
    f"#define GIULIA_PAYLOAD_CRC32 0x{r['payload_crc32']}U\n"
    '#define GIULIA_TOUCH_CLOCK 1\n#define GIULIA_TOUCH_POWER 1\n'
    + ('#define GIULIA_WIFI_PCIE 1\n' if r.get('wifi_pcie_handoff') else '#define GIULIA_WIFI_PCIE 0\n')
    + ('#define GIULIA_WIFI_XNU_LINK 1\n' if os.environ.get('GIULIA_WIFI_XNU_LINK')=='1' else '')
    + ('#define GIULIA_GPU_POWER 1\n' if r.get('gpu_power_handoff') else ''))
PY
python3 scripts/verify-wifi-native-abi.py --uefi "$uefi_root" --when-enabled
if [ -n "$gpu_pack" ] && [ "$gpu_mode" = 3 ]; then
  python3 scripts/verify-gpu-power-native-abi.py --uefi "$uefi_root"
fi
cd "$uefi_root"
if ! CLANGPDB_AARCH64_PREFIX=aarch64-linux-gnu- "$uefi_python" build_uefi.py \
     -d giulia -m 0 -r DEBUG > "$output_dir/build.log" 2>&1; then
  tail -40 "$output_dir/build.log"
  exit 1
fi
cd "$port_root"
python3 scripts/verify-image.py "$uefi_root" --output "$output_dir/firmware.json"
cp "$uefi_root/Mu-giulia-0.img" "$output_dir/firmware.img"
python3 scripts/pack-payload.py --firmware "$output_dir/firmware.img" \
  --firmware-report "$output_dir/firmware.json" --payload "$output_dir/payload/payload.img" \
  --payload-report "$output_dir/payload/payload.json" --header-version 3 \
  --output "$output_dir/giulia-ios16-wifi-pcie.img"
cp "$uefi_root/Build/giuliaPkg/DEBUG_CLANGPDB/BUILD_TOOLS_REPORT.json" "$output_dir/"
cp "$driver_dir/GiuliaWifiReport.h" "$driver_dir/GiuliaWifiEncoding.h" "$output_dir/driver-source/"
cp "$board_driver/PayloadIdentity.h" "$output_dir/driver-source/"
cp "$0" "$output_dir/build-script.sh"
python3 - "$output_dir" "$driver_dir" <<'PY'
from pathlib import Path
import hashlib,json,os,subprocess,sys
out,driver=map(Path,sys.argv[1:])
files=[*sorted((out/'driver-source').glob('*')),
       *sorted(Path('ios16/wifi').glob('giulia_wifi_*.h')),Path('ios16/wifi/GiuliaWiFi.cpp'),
       Path('ios16/display/giulia_display.h'),Path('scripts/ios16_display.py'),
       Path('scripts/embed-wifi-firmware.py'),Path('scripts/ios16_wifi_runtime.py'),
       Path('ios16/0081-giulia-pmap-high-physical-cache-sync.patch'),
       Path('scripts/fix-ios16-high-physical-cache.py'),
       out/'kernel.macho',out/'giulia-ios16.adt',out/'captured-board.dts',
       driver/'DarwinBoot.c',Path('scripts/enable-wifi-diagnostics.py'),
       Path('scripts/make-ios16-payload.py'),Path('scripts/verify-image.py'),Path('scripts/verify-wifi-native-abi.py'),
       Path('scripts/pack-payload.py'),Path('scripts/ios16_wifi_pcie.py'),
       Path('scripts/ios16_pcie_diagnostic.py'),Path('scripts/decode-ios16-pcie-snapshot.py')]
if (out/'giulia-ios16.gpu-power.json').is_file():
    files += [out/'giulia-ios16.gpu-power.json',Path('scripts/verify-gpu-power-native-abi.py')]
if (out/'gunyah-request.gpu-gunyah.json').is_file():
    files += [out/'gunyah-request.gpu-gunyah.json']
payload=json.loads((out/'payload/payload.json').read_text())
runtime=payload.get('wifi_runtime')
gpu_runtime=payload.get('gpu_runtime')
files += [*sorted((out/'runtime-source').rglob('*')),out/'kernel.json',out/'gpu-offline-checks.json']
report={'repository_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
        'pcie_failure_snapshot':os.environ.get('GIULIA_WIFI_SNAPSHOT')=='1',
        'pcie_link_training_owner':'XNU' if os.environ.get('GIULIA_WIFI_XNU_LINK')=='1' else 'UEFI',
        'source_files':[{'path':str(p.resolve()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
                        for p in files if p.is_file()],
        'hardware_verified':False,'scan_implemented':bool(runtime),'scan_hardware_verified':False,
        'runtime':runtime,'gpu_runtime':gpu_runtime,'gpu_mode':payload.get('gpu_mode'),
        'gpu_power_handoff':payload.get('gpu_power_handoff'),
        'gpu_hostonly':payload.get('gpu_hostonly',False),
        'gpu_gunyah_handoff':payload.get('gpu_gunyah_handoff'),
        'gpu_hardware_verified':False,'acceleration':False,
        'firmware_digests':{'wifi':runtime['firmware']['sha256'] if runtime else None,
                            'gpu':gpu_runtime['firmware']['sha256'] if gpu_runtime else None},
        'device_commands_executed':False}
(out/'source-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
PY
echo 'Integrated Wi-Fi/GPU bring-up image built; selected runtimes are recorded in source-manifest.json.'
