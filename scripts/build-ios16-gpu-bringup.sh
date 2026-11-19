#!/usr/bin/env bash
# Use a dedicated prepared Darwin22 UEFI checkout. Does not access a device.
set -euo pipefail
if [ "$#" -ne 8 ]; then
  echo 'Usage: build-ios16-gpu-bringup.sh UEFI UEFI_PYTHON KERNEL ADT ROOT_REPORT TRUST_CATALOG GPU_PACK OUTPUT' >&2
  exit 2
fi
port_root="$(cd "$(dirname "$0")/.." && pwd)"
uefi_root="$(realpath "$1")"
uefi_python="$(realpath -s "$2")"
kernel_input="$(realpath "$3")"
adt_input="$(realpath "$4")"
root_report="$(realpath "$5")"
trust_input="$(realpath "$6")"
gpu_pack="$(realpath "$7")"
output_dir="$(realpath -m "$8")"
gpu_mode="${GIULIA_GPU_MODE:-1}"
gpu_boot_state="${GIULIA_GPU_BOOT_STATE:-0}"
case "$gpu_mode" in 1|2|3) ;; *) echo 'GIULIA_GPU_MODE must be 1, 2 or 3' >&2; exit 2;; esac
case "$gpu_boot_state" in 0|1) ;; *) echo 'GIULIA_GPU_BOOT_STATE must be 0 or 1' >&2; exit 2;; esac
driver_dir="$uefi_root/UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver"
board_driver="$uefi_root/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe"
test ! -e "$output_dir"
test -x "$uefi_python"
"$uefi_python" -c 'import coloredlogs'
python3 - "$driver_dir" <<'PY'
from pathlib import Path
import sys
for name in ('BootArgs.h','DarwinBoot.c'):
    if 'CommandLine[608]' not in (Path(sys.argv[1])/name).read_text():
        raise SystemExit('Requires the prepared Darwin22 loader: '+name)
PY
mkdir -p "$output_dir"
exec > >(tee "$output_dir/host-build.log") 2>&1
cd "$port_root"
bash scripts/test-ios16-gpu.sh "$(dirname "$gpu_pack")/gmu_gen70900.bin"
python3 tests/ios16_gpu_patch.py
python3 tests/ios16_gpu_embed.py "$gpu_pack"
python3 scripts/embed-gpu-firmware.py --kernel "$kernel_input" --pack "$gpu_pack" --out "$output_dir/kernel.macho"
if [ "$gpu_mode" = 3 ]; then
  python3 scripts/prepare-ios16-gpu-power.py --adt "$adt_input" --out "$output_dir/giulia-ios16.adt"
else
  cp "$adt_input" "$output_dir/giulia-ios16.adt"
fi
cp -a uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/. "$board_driver/"
if [ "$gpu_boot_state" = 1 ]; then
  python3 scripts/verify-gpu-boot-state-abi.py --uefi "$uefi_root" > "$output_dir/gpu-boot-state-abi.json"
  python3 scripts/enable-gpu-boot-state.py "$driver_dir"
  python3 - "$output_dir/giulia-ios16.adt" <<'PY'
from pathlib import Path
import sys
sys.path.insert(0,'scripts')
from ios16_gpu_boot_state import add_request
path=Path(sys.argv[1])
path.write_bytes(add_request(path.read_bytes()))
PY
fi
python3 scripts/enable-touch-diagnostics.py "$driver_dir"
if [ "$gpu_mode" = 3 ]; then
  python3 scripts/enable-gpu-power-diagnostics.py "$driver_dir"
fi
python3 scripts/make-ios16-payload.py --kernel "$output_dir/kernel.macho" \
  --adt "$output_dir/giulia-ios16.adt" --trust-catalog "$trust_input" \
  --out "$output_dir/payload" --root-report "$root_report" \
  --default-root --trace-all --enable-touch --touch-gpio --gpu-mode "$gpu_mode"
python3 scripts/verify-payload.py "$output_dir/payload"
python3 tests/ios16_dram_handoff.py --driver "$driver_dir" --adt "$output_dir/giulia-ios16.adt"
python3 - "$output_dir" "$board_driver" <<'PY'
from pathlib import Path
import json,sys
out,driver=map(Path,sys.argv[1:])
r=json.loads((out/'payload/identity.json').read_text())
(driver/'PayloadIdentity.h').write_text(
    f"#define GIULIA_PAYLOAD_BYTES {r['payload_bytes']}U\n"
    f"#define GIULIA_PAYLOAD_CRC32 0x{r['payload_crc32']}U\n"
    '#define GIULIA_TOUCH_CLOCK 1\n#define GIULIA_TOUCH_POWER 1\n#define GIULIA_WIFI_PCIE 0\n'
    + ('#define GIULIA_GPU_POWER 1\n' if r.get('gpu_power_handoff') else ''))
PY
if [ "$gpu_mode" = 3 ]; then
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
  --output "$output_dir/giulia-ios16-gpu.img"
cp "$0" "$output_dir/build-script.sh"
cp -a "$board_driver" "$output_dir/uefi-driver-source"
mkdir "$output_dir/runtime-source"
python3 - "$output_dir/runtime-source" <<'PY'
from pathlib import Path
import shutil,sys
target=Path(sys.argv[1])
for module in ('gpu','common','platform','vendor'):
    shutil.copytree(Path('ios16')/module,target/'ios16'/module)
for name in ('ios16_sources.py','ios16_gpu_patch.py','ios16_platform_patch.py',
             'ios16_modules.py','ios16_module_registry.py'):
    (target/'scripts').mkdir(exist_ok=True)
    shutil.copyfile(Path('scripts')/name,target/'scripts'/name)
PY
cp "$board_driver/GpuPowerReport.h" "$output_dir/runtime-source/giulia_gpu_power_report.h"
echo 'GPU preparation candidate built and checked; hardware and acceleration remain unverified.'
