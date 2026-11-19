"""Build a verified iOS16 kernel/ADT FAT payload for the matching UEFI build."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import tomllib
import zlib
from ios16_platform import check_platform, read_nodes
from ios16_wifi import check_wifi_platform
from ios16_wifi_runtime import check_runtime
from ios16_gpu_runtime import check_runtime as check_gpu_runtime
from ios16_audio_runtime import check_runtime as check_audio_runtime
from ios16_modem import check_contract as check_modem_contract
from ios16_gpu_power import check_request as check_gpu_power_request
from ios16_gpu_boot_state import check_request as check_gpu_boot_state_request
from ios16_gpu_gunyah import check_request as check_gpu_gunyah_request
from ios16_trust_catalog import modules

def sha(p):
    with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--kernel',type=Path,required=True)
    p.add_argument('--adt',type=Path,required=True)
    p.add_argument('--trust-catalog',type=Path,help='Verified official firmware trust catalogue')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--root-report',type=Path,help='Include HFSX boot entry for an already prepared root')
    p.add_argument('--default-root',action='store_true')
    p.add_argument('--debug-terminal',action='store_true',help='Enable root USB PTY device; requires separately staged debug daemon/shell')
    p.add_argument('--debug-mmio',action='store_true',help='Enable explicit low peripheral MMIO commands; requires --debug-terminal')
    p.add_argument('--wifi-terminal-wait',action='store_true',help='Wait for wifi.start before DMA initialization so USB captures failures')
    p.add_argument('--trace-services',action='store_true',help='Sample GUI and first-boot migration services')
    p.add_argument('--trace-launch-services',action='store_true',help='Sample lsd and container services with their blocked clients')
    p.add_argument('--trace-all',action='store_true',help='All processes, byte-preserving stdout/stderr, unlimited periodic snapshots')
    p.add_argument('--enable-touch',action='store_true',help='Start the compiled Ace 5 hardware touch worker')
    p.add_argument('--enable-slider',action='store_true',help='Opt in to the AK09970 worker; requires matched native Hall preparation and subsequent calibration')
    p.add_argument('--touch-gpio',action='store_true',help='Allow GPIO SPI on the observed GPI-only controller')
    p.add_argument('--wifi-pcie',action='store_true',help='Opt-in WCN7850 power/PCIe enumeration in the matching UEFI; a runtime kernel also requires locally embedded firmware')
    p.add_argument('--gpu-mode',type=int,choices=(1,2,3),help='1: host preparation; 2: gated observation; 3: explicit UEFI CX/clock preparation with a fixed ADT report')
    p.add_argument('--gpu-hostonly',action='store_true',help='Restrict GPU mode 1 to host-memory preparation and CPU-only status/stack commands')
    p.add_argument('--audio-prepare',action='store_true',help='Validate local audio pack and start coordinator; does not establish native platform ownership')
    p.add_argument('--modem-mode',choices=('diagnostic','managed'),help='Enable the unified modem service; managed requires native provider and matching cellular root policy')
    a=p.parse_args()
    if a.gpu_hostonly and a.gpu_mode != 1:
        p.error('--gpu-hostonly requires --gpu-mode 1')
    if a.trace_launch_services:a.trace_services=True
    if a.trace_all:a.trace_services=True
    if a.default_root and not a.root_report: p.error('Default root requires a root report')
    if a.debug_mmio and not a.debug_terminal:p.error('--debug-mmio requires --debug-terminal')
    if a.wifi_terminal_wait and (not a.debug_terminal or not a.wifi_pcie):
        p.error('--wifi-terminal-wait requires --debug-terminal and --wifi-pcie')
    if a.out.exists():raise ValueError('Refusing to overwrite output')
    report=json.loads(a.kernel.with_suffix('.json').read_text())
    if report['output_sha256']!=sha(a.kernel) or report['boot_args_bytes']!=736:
        raise ValueError('Kernel handoff verification missing')
    platform_match=check_platform(a.adt.read_bytes(),a.kernel.read_bytes())
    if a.gpu_hostonly and b'giulia_gpu_hostonly\0' not in a.kernel.read_bytes():
        raise ValueError('Kernel lacks the GPU host-only boot policy')
    if a.enable_slider and (not a.root_report or not platform_match.get('gui') or
                            b'GIULIA SLIDER: handoff-missing gh=' not in a.kernel.read_bytes()):
        raise ValueError('Slider requires a GUI root and the compiled guarded AK09970 worker')
    gpu_runtime=check_gpu_runtime(a.kernel.read_bytes(),report)
    audio_runtime=check_audio_runtime(a.kernel.read_bytes(),report)
    modem_root=json.loads(a.root_report.read_text()) if a.root_report else None
    modem_contract=check_modem_contract(a.kernel.read_bytes(),report,a.adt.read_bytes(),modem_root,a.modem_mode)
    if a.modem_mode and (not a.root_report or not platform_match.get('gui')):
        raise ValueError('Modem service requires the matching GUI root entry')
    if a.audio_prepare and (not audio_runtime or not a.root_report or not platform_match.get('gui')):
        raise ValueError('Audio preparation requires a GUI root and matching embedded audio firmware')
    gpu_power=check_gpu_power_request(a.adt.read_bytes(),a.gpu_mode,a.kernel.read_bytes())
    gpu_boot_state=check_gpu_boot_state_request(a.adt.read_bytes(),a.gpu_mode,a.kernel.read_bytes())
    gpu_gunyah=check_gpu_gunyah_request(a.adt.read_bytes(),a.gpu_mode,a.kernel.read_bytes())
    if a.gpu_mode and (not gpu_runtime or not platform_match.get('gui') or
                       (not a.root_report and not a.gpu_hostonly)):
        raise ValueError('GPU mode requires a GUI root entry and matching embedded GPU firmware')
    wifi=check_wifi_platform(read_nodes(a.adt.read_bytes()))
    wifi_runtime=check_runtime(a.kernel.read_bytes(),report)
    if wifi_runtime and not a.wifi_pcie:
        raise ValueError('Native Wi-Fi runtime requires the matching PCIe handoff')
    if a.wifi_pcie and not wifi:
        raise ValueError('PCIe bring-up requires the Qualcomm wlan ADT declaration')
    if wifi and (not a.root_report or not a.trace_all):
        raise ValueError('Wi-Fi service bring-up requires a root entry and --trace-all')
    catalog=None
    if a.trust_catalog:
        catalog=json.loads((a.trust_catalog.parent/'trust-catalog.json').read_text())
        pinned=json.loads((Path(__file__).resolve().parents[1]/'ios16/firmware.json').read_text())
        parsed=modules(a.trust_catalog.read_bytes())
        if (catalog['sha256']!=sha(a.trust_catalog) or catalog['source_ipsw_sha256']!=pinned['sha256']
                or len(catalog['modules'])!=len(parsed) or not a.root_report
                or not platform_match.get('gui')
                or b'GIULIA AMFI: trust catalogue ready;' not in a.kernel.read_bytes()):
            raise ValueError('Official trust catalogue/kernel binding failed')
    if a.trace_services and (not a.root_report or not platform_match.get('gui')):
        raise ValueError('Service sampling requires the GUI kernel and root entry')
    if a.enable_touch and (not a.root_report or not platform_match.get('gui') or b'GIULIA TOUCH: controller result=' not in a.kernel.read_bytes()):
        raise ValueError('Touch requires a GUI kernel containing the Ace 5 hardware driver')
    if a.touch_gpio and (not a.enable_touch or b'GIULIA TOUCH: GPIO SPI mode 0 active;' not in a.kernel.read_bytes()):
        raise ValueError('GPIO SPI requires the corresponding kernel driver and --enable-touch')
    args='-v serial=3 debug=0x14e cpus=1 rd=vermeerufs0 vermeer_usb=1 vermeer_ufs_highdma=1'
    if a.debug_terminal:
        if b'GIULIA SHELL: /dev/giulia-debug ready;' not in a.kernel.read_bytes():
            raise ValueError('Debug terminal requires protocol 2 kernel')
        args+=' giulia_debug=1'
    if a.debug_mmio:args+=' giulia_mmio=1'
    if a.wifi_terminal_wait:
        if b'GIULIA WIFI: awaiting wifi.start before DMA setup' not in a.kernel.read_bytes():
            raise ValueError('Kernel lacks the Wi-Fi terminal start gate')
        args+=' giulia_wifi_wait=1'
    if a.gpu_hostonly:args+=' giulia_gpu=1 giulia_gpu_hostonly=1'
    probe=args+' giulia_ufs_probe=1'
    config=('version = 1\n[boot]\ntimeout = 3\ndefault = "xnu"\n[kits]\nautoload = false\n'
            '[[entry]]\nid = "xnu"\ntitle = "Ace 5 iOS16 UFS + USB"\ntype = "xnu"\n'
            'path = "Payloads/kernel.macho"\nargs = '+json.dumps(probe)+'\n')
    if a.root_report:
        root=json.loads(a.root_report.read_text())
        ident=root['hfs_finder_id']
        if root['bytes']!=16*1024**3 or root['block_size']!=4096 or len(ident)!=16 or int(ident,16)==0:
            raise ValueError('Unexpected HFSX root identity')
        # The pinned DEVELOPMENT kernel is built with XNU_TARGET_OS_OSX. Its
        # exec platform gate requires this existing development option for
        # native iOS executables (OS_REASON_EXEC / WRONG_PLATFORM == 9 / 14).
        # CodeDirectory/page validation is unchanged; this is not cs_enforcement_disable.
        if b'unentitled_ios_sim_launch\0' not in a.kernel.read_bytes():
            raise ValueError('Kernel lacks the iOS platform-launch development option')
        # The original launchd_cache_loader exposes this separate development
        # mode for its job cache when no Apple AMFI IOService is available.
        root_args=args+f' vermeer_ufs_rw=1 vermeer_root_uuid={ident} unentitled_ios_sim_launch=1 launchd_unsecure_cache=1'
        if platform_match.get('gui'):
            # The GUI driver mutes only ordinary screen text after its first
            # presentation. USB logging and panic text remain available.
            root_args+=' sweetfb_hz=30 sweetfb_noconsole=1'
        if a.trace_services:
            if b'sweetsample\0' not in a.kernel.read_bytes():
                raise ValueError('Kernel lacks the process sampler')
            # proc_name copies p_comm[MAXCOMLEN+1]; XNU MAXCOMLEN is 16.
            # The sampler's larger destination buffer does not recover p_name.
            sample_names='SpringBoard,backboardd,com.apple.datami,com.apple.migrat,appstored,installd,installcoordinat,accountsd,securityd,cfprefsd'
            if a.trace_launch_services:
                sample_names='SpringBoard,backboardd,lsd,containermanager,installd,installcoordinat,accountsd,securityd,cfprefsd,appstored'
            if a.trace_all:
                if b'GIULIA LOG: launchd full activity/stderr logging enabled;' not in a.kernel.read_bytes():
                    raise ValueError('Full logging kernel required')
                sample_names='*'
            if len(sample_names)>=128:raise ValueError('Service selector buffer exceeded')
            root_args+=' sweetsample='+sample_names
            # The /dev/null tee uses proc_selfname (p_name), unlike proc_name.
            stderr_names='SpringBoard,backboardd,com.apple.datamigrator,com.apple.migrationpluginwrappe'
            if a.trace_launch_services:
                stderr_names='SpringBoard,backboardd,lsd,containermanagerd,installd,installcoordinationd,accountsd,securityd'
            if a.trace_all:stderr_names='*'
            root_args+=' sweetnull='+stderr_names
            if a.trace_all:root_args+=' giulia_bootlog=1'
        if a.enable_touch:root_args+=' giulia_touch=1'
        if a.touch_gpio:root_args+=' giulia_touch_gpio=1'
        if a.gpu_mode and not a.gpu_hostonly:root_args+=' giulia_gpu='+str(a.gpu_mode)
        if a.audio_prepare:root_args+=' giulia_audio=1'
        if a.enable_slider:root_args+=' giulia_slider=1'
        if a.modem_mode:root_args+=' giulia_modem='+str(1 if a.modem_mode=='diagnostic' else 2)
        config+=('[[entry]]\nid = "ios16"\ntitle = "Ace 5 iOS16 HFSX root"\ntype = "xnu"\n'
                 'path = "Payloads/kernel.macho"\nargs = '+json.dumps(root_args)+'\n')
        if a.default_root:config=config.replace('default = "xnu"','default = "ios16"')
    for e in tomllib.loads(config)['entry']:
        if len(e['args'].encode('ascii'))>=608:raise ValueError('Boot line too long')
    a.out.mkdir(parents=True)
    image=a.out/'payload.img'
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1',TZ='UTC')
    with tempfile.TemporaryDirectory() as td:
        stage=Path(td);payload=stage/'EFI/UniXNU/Payloads';payload.mkdir(parents=True)
        shutil.copyfile(a.kernel,payload/'kernel.macho')
        shutil.copyfile(a.adt,payload/'kernel.macho.adt')
        if a.trust_catalog:shutil.copyfile(a.trust_catalog,payload/'kernel.macho.trustcache')
        (payload.parent/'unixnu.toml').write_text(config)
        for path in [stage,*stage.rglob('*')]:os.utime(path,(315532800,315532800))
        subprocess.run(['mformat','-C','-F','-T','131072','-v','GIULIAESP','-N','47495531','-i',str(image),'::'],env=env,check=True)
        subprocess.run(['mcopy','-i',str(image),'-s','-Q','-m',str(stage/'EFI'),'::/'],env=env,check=True)
    data=image.read_bytes()
    if data[82:90]!=b'FAT32   ' or struct.unpack_from('<I',data,32)[0]*512!=len(data):raise ValueError('FAT geometry')
    result={'payload_bytes':len(data),'payload_sha256':sha(image),'payload_crc32':f'{zlib.crc32(data):08x}',
            'payload_kernel_sha256':sha(a.kernel),'kernel_sha256':sha(a.kernel),'adt_sha256':sha(a.adt),
            'kernel_args':probe,'dram_handoff':'darwinboot-allocated-window','platform_match':platform_match,
            'hardware_tested':False,'purpose':'iOS16 XNU: UFS read probe and native USB console; no filesystem writes',
            'debug_terminal_enabled':a.debug_terminal,'debug_mmio_enabled':a.debug_mmio,
            'wifi_terminal_wait':a.wifi_terminal_wait}
    if a.root_report:
        result.update(root_sha256=root['sha256'],root_finder_id=ident,default_entry='ios16' if a.default_root else 'xnu',
                      root_kernel_args=root_args,ios_platform_launch_compat=True,launchd_job_cache_compat=True,
                      purpose='iOS16 HFSX root mount and UFS/USB diagnostic entries; use after manual userdata flash')
        if a.trace_services:
            result['trace_services']=True
            result['service_sample_names']=sample_names
            result['service_stderr_trace']=True
            result['service_stderr_names']=stderr_names
            result['service_trace_profile']='all' if a.trace_all else ('launchservices' if a.trace_launch_services else 'migration')
            if a.trace_all:result['complete_boot_logging']=True
        if a.enable_touch:
            result['physical_touch_worker']=True
            result['touch_clock_handoff']=True
        if a.touch_gpio:result['touch_gpio']=True
        if a.enable_slider:
            result['physical_slider_worker']=True
            result['hall_prepare_handoff']=True
    if catalog:
        result['trust_catalog']={k:catalog[k] for k in ('sha256','bytes','build','source_ipsw_sha256','unique_hashes')}
    if wifi:result['wifi_bringup']=wifi
    if wifi_runtime:result['wifi_runtime']=wifi_runtime
    if gpu_runtime:result['gpu_runtime']=gpu_runtime
    if audio_runtime:result['audio_runtime']=audio_runtime
    if modem_contract:
        result['modem_contract']=modem_contract
        result['modem_root_policy']=(modem_root or {}).get('cellular_policy',{})
    if a.modem_mode:result['modem_mode']=a.modem_mode
    if a.audio_prepare:result['audio_prepare']=True
    if a.gpu_mode:result['gpu_mode']=a.gpu_mode
    if a.gpu_hostonly:result['gpu_hostonly']=True
    if gpu_power:result['gpu_power_handoff']=gpu_power
    if gpu_boot_state:result['gpu_boot_state_handoff']=gpu_boot_state
    if gpu_gunyah:result['gpu_gunyah_handoff']=gpu_gunyah
    if a.wifi_pcie:
        result['wifi_pcie_handoff']=True
        result['wifi_pcie_report_version']=5
        result['wifi_cesta_handoff']=True
        result['wifi_stock_handoff']=True
    (a.out/'payload.json').write_text(json.dumps(result,indent=2)+'\n')
    (a.out/'identity.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
