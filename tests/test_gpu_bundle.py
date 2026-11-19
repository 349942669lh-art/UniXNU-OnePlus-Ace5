"""Exercise unified packaging with synthetic firmware and a fake UEFI builder."""
from pathlib import Path as _ModulePath
import sys as _module_sys
_module_sys.path.insert(0, str(_ModulePath(__file__).resolve().parents[1] / "scripts"))
from ios16_sources import source_path
import gzip
import hashlib
import json
import os
import runpy
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('bash'), 'Linux/WSL shell packaging')
class UnifiedGpuBundle(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='giulia-gpu-bundle-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / 'repo'
        for name in ('scripts', 'tests', 'ios16', 'ios16/wifi', 'ios16/gpu', 'ios16/display', 'ios16/common', 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'):
            (self.repo / name).mkdir(parents=True)
        for name in ('build-ios16-wifi-bringup.sh', 'embed-wifi-firmware.py', 'embed-gpu-firmware.py',
                     'prepare-ios16-gpu-power.py', 'ios16_gpu_power.py', 'ios16_platform.py',
                     'prepare-ios16-gpu-gunyah.py','ios16_gpu_gunyah.py',
                     'check-ios16-gpu-kernel.py'):
            shutil.copyfile(ROOT / 'scripts' / name, self.repo / 'scripts' / name)
        self.packs = {}
        self.digests = {}
        for name, magic, size in (('wifi', b'GWFWPK1\0', 128), ('gpu', b'GGFWPK1\0', 4096)):
            pack = bytearray(size)
            pack[:8] = magic
            struct.pack_into('<I', pack, 16, size)
            path = self.root / (name + '.pack')
            path.write_bytes(pack)
            self.packs[name] = path
            self.digests[name] = hashlib.sha256(pack).digest()
            (self.repo / f'ios16/{name}/giulia_{name}_firmware_pin.h').write_text(
                ','.join(f'0x{x:02x}' for x in self.digests[name]))
        self.uefi = self.root / 'uefi'
        self.driver = self.uefi / 'UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver'
        self.driver.mkdir(parents=True)
        (self.uefi / 'Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe').mkdir(parents=True)
        for name in ('BootArgs.h', 'DarwinBoot.c', 'GiuliaWifiReport.h', 'GiuliaWifiEncoding.h'):
            (self.driver / name).write_text('CommandLine[608]')
        build = self.uefi / 'Build/giuliaPkg/DEBUG_CLANGPDB'
        build.mkdir(parents=True)
        (build / 'BUILD_TOOLS_REPORT.json').write_text('{}')
        (self.uefi / 'Mu-giulia-0.img').write_bytes(b'firmware-fixture')
        self.interpreter = self.root / 'uefi-python'
        self.interpreter.write_text('#!/bin/sh\nif [ "$1" = -c ]; then exit 0; fi\nprintf UEFI_BUILD_REACHED\n')
        self.interpreter.chmod(0o755)
        self.inputs = []
        for name in ('board.adt', 'root.json', 'trust.bin', 'board.dts'):
            path = self.root / name
            path.write_text('{}')
            self.inputs.append(str(path))
        def node(name, values=(), children=()):
            values=[('name',name.encode()+b'\0'),*values]
            return (struct.pack('<II',len(values),len(children)) +
                b''.join(struct.pack('<32sI',k.encode(),len(v))+v+bytes(-len(v)%4) for k,v in values)+b''.join(children))
        Path(self.inputs[0]).write_bytes(node('device-tree',[('physical-model',b'OnePlus Ace 5\0')],[node('chosen')]))
        overlay = self.repo / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
        (overlay / 'Driver.c').write_text('fixture')
        for name in ('GiuliaWiFi.cpp', 'GiuliaGPU.cpp', 'giulia_display.h', 'giulia_sha256.h',
                     'giulia_gpu_control.c', 'giulia_gpu_control.h',
                     'giulia_gpu_power_owner.cpp'):
            (source_path(self.repo, name)).write_text(name)
        vendor = self.repo / 'ios16/vendor/ArmSmmu4K'
        vendor.mkdir(parents=True)
        (vendor / 'ArmSmmu4K.c').write_text('vendor-source')
        (vendor / 'SOURCE.json').write_text('{}')
        for name in ('ios16_display.py', 'ios16_gpu_patch.py', 'ios16_gpu_runtime.py'):
            (self.repo / 'scripts' / name).write_text('# source fixture\n')
        for name in ('fix-ios16-high-physical-cache.py', 'enable-touch-diagnostics.py',
                     'enable-wifi-diagnostics.py', 'verify-payload.py', 'verify-wifi-native-abi.py',
                     'enable-gpu-power-diagnostics.py', 'verify-gpu-power-native-abi.py',
                     'enable-gpu-gunyah-handoff.py'):
            (self.repo / 'scripts' / name).write_text('')
        for name in ('ios16_dram_handoff.py', 'ios16_gpu_patch.py', 'ios16_gpu_embed.py'):
            (self.repo / 'tests' / name).write_text('')
        (self.repo / 'scripts/test-ios16-gpu.sh').write_text('#!/bin/sh\nexit 0\n')
        (self.repo / 'tests/ios16_wifi_bundle.py').write_text(
            "import pathlib,sys\npathlib.Path(sys.argv[sys.argv.index('--out')+1]).write_text('{}')\n")
        (self.repo / 'scripts/verify-image.py').write_text(
            "import pathlib,sys\npathlib.Path(sys.argv[sys.argv.index('--output')+1]).write_text('{}')\n")
        (self.repo / 'scripts/pack-payload.py').write_text(
            "import pathlib,sys\npathlib.Path(sys.argv[sys.argv.index('--output')+1]).write_bytes(b'image')\n")
        (self.repo / 'scripts/make-ios16-payload.py').write_text('''import json,sys
from pathlib import Path
args=sys.argv[1:]
out=Path(args[args.index('--out')+1]);out.mkdir()
metadata=json.loads(Path(args[args.index('--kernel')+1]).with_suffix('.json').read_text())
report={}
if metadata.get('wifi_firmware'):report['wifi_runtime']={'firmware':metadata['wifi_firmware']}
if '--wifi-pcie' in args:report['wifi_pcie_handoff']=True
if '--gpu-hostonly' in args:report['gpu_hostonly']=True
if metadata.get('gpu_firmware'):report['gpu_runtime']={'firmware':metadata['gpu_firmware'],'acceleration':False}
if '--gpu-mode' in args:report['gpu_mode']=int(args[args.index('--gpu-mode')+1])
if report.get('gpu_mode'):
 from ios16_gpu_gunyah import check_request
 report['gpu_gunyah_handoff']=check_request(Path(args[args.index('--adt')+1]).read_bytes(),report['gpu_mode'])
if report.get('gpu_mode')==3:
 from ios16_gpu_power import check_request
 report['gpu_power_handoff']=check_request(Path(args[args.index('--adt')+1]).read_bytes(),3)
(out/'payload.json').write_text(json.dumps(report))
(out/'identity.json').write_text(json.dumps({**report,'payload_bytes':64,'payload_crc32':'12345678'}))
(out/'payload.img').write_bytes(b'payload')
''')
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        subprocess.run(['git', '-C', str(self.repo), '-c', 'user.name=Fixture', '-c',
                        'user.email=fixture@example.invalid', 'commit', '--allow-empty', '-qm', 'Fixture'], check=True)
        self.kernel = self.root / 'kernel.macho'
        self.output = self.root / 'output'
        self.write_kernel(gpu=True)

    def write_kernel(self, gpu):
        kernel = bytearray(0x8000)
        struct.pack_into('<8I', kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 1, 152, 0, 0)
        struct.pack_into('<II16sQQQQiiII', kernel, 32, 0x19, 152, b'__TEXT',
                         0xfffffe0007004000, 0x8000, 0, 0x8000, 5, 5, 1, 0)
        struct.pack_into('<16s16sQQIIIIIIII', kernel, 104, b'__text', b'__TEXT',
                         0xfffffe0007008000, 0x4000, 0x4000, 14, 0, 0, 0, 0, 0, 0)
        markers = (self.digests['wifi'] + b'GIULIA WIFI: PCIe/MHI/QMI/WMI ready;\0'
                   b'GIULIA WIFI: firmware pack mapped read-only and digest verified\0'
                   b'GIULIA FB: native panel 1264x2780 full coverage;\0')
        if gpu:
            markers += (self.digests['gpu'] + b'GIULIA GPU: host preparation complete;\0'
                        b'GIULIA GPU: firmware pack mapped read-only and digest verified\0'
                        b'GIULIA GPU: UEFI power handoff\0'
                        b'GIULIA GPU: boot Gunyah resources\0')
        marker_offset = 0x1000
        if gpu:
            linked_fixture = runpy.run_path(str(ROOT / 'tests/test_gpu_kernel.py'))['LinkedGPU']()
            linked_fixture.setUp()
            kernel = bytearray(linked_fixture.kernel)
            marker_offset = (len(kernel) + 0x3fff) & ~0x3fff
            kernel.extend(bytes(marker_offset + 0x4000 - len(kernel)))
            struct.pack_into('<Q', kernel, 32 + 32, len(kernel))
            struct.pack_into('<Q', kernel, 32 + 48, len(kernel))
        kernel[marker_offset:marker_offset+len(markers)] = markers
        self.kernel.write_bytes(kernel)
        self.original_digest = hashlib.sha256(kernel).hexdigest()
        self.kernel.with_suffix('.json').write_text(json.dumps({
            'output_sha256': self.original_digest, 'boot_args_bytes': 736, 'preserved': 'handoff-fixture'}))

    def build(self, gpu_pack=True, mode=None, wifi_pack=True, hostonly=False):
        env = {k: v for k, v in os.environ.items() if not k.startswith('GIULIA_')}
        if gpu_pack:
            env['GIULIA_GPU_PACK'] = str(self.packs['gpu'])
        if mode is not None:
            env['GIULIA_GPU_MODE'] = str(mode)
        if hostonly:
            env['GIULIA_GPU_HOSTONLY'] = '1'
        command = ['bash', str(self.repo / 'scripts/build-ios16-wifi-bringup.sh'),
                   str(self.uefi), str(self.interpreter), str(self.kernel), *self.inputs, str(self.output)]
        if wifi_pack:
            command.append(str(self.packs['wifi']))
        return subprocess.run(command, capture_output=True, text=True, env=env)

    def rejected(self, message, **options):
        result = self.build(**options)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.output.exists(), result.stdout)
        self.assertNotIn('UEFI_BUILD_REACHED', result.stdout)

    def test_compiled_gpu_requires_explicit_pack(self):
        self.rejected('Compiled GPU driver requires GIULIA_GPU_PACK', gpu_pack=False)

    def test_mode_requires_pack_and_supported_value(self):
        self.rejected('requires explicit GIULIA_GPU_PACK', gpu_pack=False, mode=1)
        self.rejected('must be 1, 2 or 3', mode=4)

    def test_gpu_requires_compiled_service(self):
        self.write_kernel(gpu=False)
        self.rejected('kernel compiled with --gpu')

    def test_missing_boot_resource_consumer_fails_before_build(self):
        binary=self.kernel.read_bytes().replace(b'GIULIA GPU: boot Gunyah resources',b'X'*32)
        self.kernel.write_bytes(binary)
        metadata=json.loads(self.kernel.with_suffix('.json').read_text())
        metadata['output_sha256']=hashlib.sha256(binary).hexdigest()
        self.kernel.with_suffix('.json').write_text(json.dumps(metadata))
        self.rejected('requires the Gunyah boot-resource consumer')

    def test_corrupt_pack_and_metadata_fail_before_build(self):
        data = bytearray(self.packs['gpu'].read_bytes());data[-1] ^= 1
        self.packs['gpu'].write_bytes(data)
        self.rejected('Firmware digest mismatch')
        self.kernel.with_suffix('.json').write_text('{"output_sha256":"wrong","boot_args_bytes":736}')
        self.rejected('Kernel handoff metadata mismatch')

    def assert_bundle(self, mode):
        result = self.build(mode=mode)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        metadata = json.loads((self.output / 'kernel.json').read_text())
        binary = (self.output / 'kernel.macho').read_bytes()
        wifi_binary = (self.output / 'kernel-wifi.macho').read_bytes()
        self.assertEqual(metadata['preserved'], 'handoff-fixture')
        self.assertEqual(metadata['pre_firmware_sha256'], self.original_digest)
        self.assertEqual(metadata['pre_gpu_firmware_sha256'], hashlib.sha256(wifi_binary).hexdigest())
        self.assertEqual(metadata['output_sha256'], hashlib.sha256(binary).hexdigest())
        for name in ('wifi', 'gpu'):
            firmware = metadata[name + '_firmware']
            self.assertEqual(binary[firmware['file_offset']:firmware['file_offset']+firmware['bytes']],
                             self.packs[name].read_bytes())
            self.assertEqual(firmware['sha256'], self.digests[name].hex())
        report = json.loads((self.output / 'source-manifest.json').read_text())
        self.assertEqual(report['gpu_mode'], mode or 1)
        from ios16_gpu_gunyah import check_request as check_gunyah
        handoff=check_gunyah((self.output/'giulia-ios16.adt').read_bytes(),mode or 1,binary)
        self.assertEqual(report['gpu_gunyah_handoff'],handoff)
        self.assertFalse(handoff['irq_lease'])
        self.assertNotIn(b'giulia-gpu-gunyah',Path(self.inputs[0]).read_bytes())
        self.assertEqual(report['firmware_digests'], {k: v.hex() for k, v in self.digests.items()})
        self.assertFalse(report['acceleration'])
        for name in ('ios16/gpu/giulia_gpu_control.c', 'ios16/gpu/giulia_gpu_control.h',
                     'ios16/gpu/giulia_gpu_power_owner.cpp',
                     'ios16/vendor/ArmSmmu4K/ArmSmmu4K.c', 'ios16/vendor/ArmSmmu4K/SOURCE.json'):
            source = self.repo / name
            snapshot = self.output / 'runtime-source' / name
            self.assertEqual(source.read_bytes(), snapshot.read_bytes())
            self.assertTrue(any(item['path'] == str(snapshot.resolve()) and
                                item['sha256'] == hashlib.sha256(source.read_bytes()).hexdigest()
                                for item in report['source_files']))
        self.assertTrue((self.output / 'gpu-offline-checks.json').is_file())

    def test_gpu_only_host_preparation_has_no_wifi_handoff(self):
        result = self.build(wifi_pack=False, hostonly=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((self.output / 'source-manifest.json').read_text())
        self.assertTrue(report['gpu_hostonly'])
        self.assertIsNone(report['runtime'])
        self.assertIsNone(report['firmware_digests']['wifi'])
        self.assertEqual(report['firmware_digests']['gpu'], self.digests['gpu'].hex())
        header = self.uefi / 'Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/PayloadIdentity.h'
        self.assertIn('#define GIULIA_WIFI_PCIE 0', header.read_text())
        self.assertNotIn('#define GIULIA_GPU_POWER 1', header.read_text())

    def test_hostonly_refuses_other_modes_before_build(self):
        self.rejected('requires GPU mode 1', mode=2, hostonly=True)
        self.rejected('requires GPU mode 1', mode=3, hostonly=True)

    def test_payload_entrypoint_refuses_invalid_hostonly_mode(self):
        for mode in (None, '2', '3'):
            command = [sys.executable, str(ROOT / 'scripts/make-ios16-payload.py'),
                       '--kernel', str(self.kernel), '--adt', self.inputs[0],
                       '--out', str(self.output), '--gpu-hostonly']
            if mode is not None:
                command += ['--gpu-mode', mode]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('--gpu-hostonly requires --gpu-mode 1', result.stderr)
            self.assertFalse(self.output.exists())

    @unittest.skipUnless(shutil.which('mformat') and shutil.which('mcopy'), 'FAT host tools')
    def test_real_rootless_hostonly_payload_and_verifier(self):
        actual_repo = self.root / 'actual-payload-repo'
        shutil.copytree(ROOT / 'scripts', actual_repo / 'scripts')
        pins = actual_repo / 'ios16/gpu'
        pins.mkdir(parents=True)
        shutil.copyfile(self.repo / 'ios16/gpu/giulia_gpu_firmware_pin.h',
                        pins / 'giulia_gpu_firmware_pin.h')
        kernel = bytearray(self.kernel.read_bytes())
        wifi_marker = b'GIULIA WIFI: PCIe/MHI/QMI/WMI ready;'
        kernel = kernel.replace(wifi_marker, bytes(len(wifi_marker)))
        extra = (b"{ 'IOClass' = SweetPlatformExpert; 'IOProviderClass' = IOPlatformExpertDevice; "
                 b"'IONameMatch' = 'oneplus,giulia'; }\0"
                 b"{ 'IOClass' = SweetCPU; 'IOProviderClass' = IOPlatformDevice; "
                 b"'IONameMatch' = 'arm,armv8'; }\0"
                 b'IOMobileFramebufferUserClient\0IOCoreSurfaceRoot\0SWEET HID: touch registered\0'
                 b'GIULIA SHELL: /dev/giulia-debug ready;\0giulia_gpu_hostonly\0'
                 b'GIULIA GPU GUNYAH: self_attempted=\0giulia_gpu_rm_diagnostic\0')
        at = len(kernel) - 0x1000
        self.assertFalse(any(kernel[at:at + len(extra)]))
        kernel[at:at + len(extra)] = extra
        append = runpy.run_path(str(ROOT / 'scripts/embed-gpu-firmware.py'))['append_pack']
        embedded, firmware = append(bytes(kernel), self.packs['gpu'].read_bytes(), self.digests['gpu'])
        binary = self.root / 'rootless.macho'
        binary.write_bytes(embedded)
        binary.with_suffix('.json').write_text(json.dumps({
            'output_sha256': hashlib.sha256(embedded).hexdigest(), 'boot_args_bytes': 736,
            'gpu_runtime_included': True, 'gpu_firmware': firmware}))

        def node(name, properties=(), children=()):
            properties = [('name', name.encode() + b'\0'), *properties]
            return (struct.pack('<II', len(properties), len(children)) + b''.join(
                struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
                for key, value in properties) + b''.join(children))

        adt = node('device-tree', [
            ('compatible', b'D22AP\0iPhone10,3\0oneplus,giulia\0qcom,sm8650\0'),
            ('model', b'iPhone10,3\0'), ('physical-model', b'OnePlus Ace 5\0')], [
            node('cpus', children=[node('cpu0', [('compatible', b'arm,armv8\0')])]),
            node('arm-io', children=[node('disp0')]), node('product'),
            node('chosen', [('dram-base', bytes(8)), ('dram-size', bytes(8)),
                            ('darwinboot-memory-size', struct.pack('<Q', 0x10000000))],
                 [node('memory-map')])])
        from ios16_gpu_gunyah import add_request
        tree = self.root / 'rootless.adt'
        from ios16_gpu_boot_state import add_request as add_boot_state
        from ios16_gpu_power import add_request as add_power
        for rm_diagnostic in (False, True):
            with self.subTest(rm_diagnostic=rm_diagnostic):
                requested=add_request(adt)
                if rm_diagnostic:requested=add_power(add_boot_state(requested))
                tree.write_bytes(requested)
                output=self.root/('real-rootless-rm' if rm_diagnostic else 'real-rootless-hostonly')
                options=['--gpu-mode','3','--gpu-rm-diagnostic'] if rm_diagnostic else ['--gpu-mode','1','--gpu-hostonly']
                result=subprocess.run([sys.executable,str(actual_repo/'scripts/make-ios16-payload.py'),
                    '--kernel',str(binary),'--adt',str(tree),'--out',str(output),
                    *options,'--debug-terminal'],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                report=json.loads((output/'payload.json').read_text())
                flag='gpu_rm_diagnostic' if rm_diagnostic else 'gpu_hostonly'
                self.assertTrue(report[flag])
                self.assertNotIn('root_finder_id',report)
                self.assertIn('giulia_gpu=3' if rm_diagnostic else 'giulia_gpu=1 giulia_gpu_hostonly=1',report['kernel_args'])
                if rm_diagnostic:self.assertIn('giulia_gpu_rm_diagnostic=1',report['kernel_args'])
                self.assertNotIn('vermeer_ufs_rw=1',report['kernel_args'])
                verify=[sys.executable,str(actual_repo/'scripts/verify-payload.py'),str(output)]
                result=subprocess.run(verify,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                if rm_diagnostic and shutil.which('dtc') and shutil.which('fdtput'):
                    self.check_rm_firmware_binding(actual_repo,output)
                report[flag]=False
                (output/'payload.json').write_text(json.dumps(report))
                result=subprocess.run(verify,capture_output=True,text=True)
                self.assertNotEqual(result.returncode,0)

    def check_rm_firmware_binding(self,actual_repo,output):
        dtb=self.root/'rm-fixture.dtb'
        subprocess.run(['dtc','-I','dts','-O','dtb','-o',str(dtb)],
            input='/dts-v1/; / { chosen {}; };',text=True,check=True,capture_output=True)
        kernel=gzip.compress(b'RAM-only UEFI fixture',mtime=0)+dtb.read_bytes()
        header=bytearray(4096)
        header[:8]=b'ANDROID!'
        struct.pack_into('<I',header,8,len(kernel))
        struct.pack_into('<I',header,40,4)
        firmware=bytes(header)+kernel+bytes(-len(kernel)%4096)
        firmware_path=self.root/'rm-fixture.img'
        firmware_path.write_bytes(firmware)
        report_path=self.root/'rm-firmware.json'
        firmware_report={'sha256':hashlib.sha256(firmware).hexdigest(),
            'status':'offline-image-checks-passed','gpu_gunyah_handoff_available':True,
            'gpu_power_handoff_available':False}
        command=[sys.executable,str(actual_repo/'scripts/pack-payload.py'),
            '--firmware',str(firmware_path),'--firmware-report',str(report_path),
            '--payload',str(output/'payload.img'),'--payload-report',str(output/'payload.json')]
        report_path.write_text(json.dumps(firmware_report))
        result=subprocess.run([*command,'--output',str(self.root/'rm-packed.img')],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        firmware_report['gpu_power_handoff_available']=True
        report_path.write_text(json.dumps(firmware_report))
        result=subprocess.run([*command,'--output',str(self.root/'rm-refused.img')],capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('unavailable firmware GPU preparation',result.stdout+result.stderr)
        self.assertFalse((self.root/'rm-refused.img').exists())

    def test_rm_diagnostic_refuses_incompatible_entrypoints(self):
        for options in ([],['--gpu-mode','2','--debug-terminal'],['--gpu-mode','3'],
                        ['--gpu-mode','3','--debug-terminal','--debug-mmio'],
                        ['--gpu-mode','3','--debug-terminal','--root-report',self.inputs[1]]):
            command=[sys.executable,str(ROOT/'scripts/make-ios16-payload.py'),
                '--kernel',str(self.kernel),'--adt',self.inputs[0],'--out',str(self.output),
                '--gpu-rm-diagnostic',*options]
            result=subprocess.run(command,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0)
            self.assertIn('--gpu-rm-diagnostic requires rootless mode 3',result.stderr)
            self.assertFalse(self.output.exists())

    def test_stale_kernel_without_cx_definition_fails_before_build(self):
        binary = self.kernel.read_bytes().replace(b'_ggcx_enable\0', b'_ggcx_unlink\0')
        self.kernel.write_bytes(binary)
        metadata = json.loads(self.kernel.with_suffix('.json').read_text())
        metadata['output_sha256'] = hashlib.sha256(binary).hexdigest()
        self.kernel.with_suffix('.json').write_text(json.dumps(metadata))
        self.rejected('GPU kernel lacks linked implementation: _ggcx_enable')

    def test_default_mode_preserves_both_firmware_metadata_and_source(self):
        self.assert_bundle(mode=None)

    def test_mode_two_reaches_payload(self):
        self.assert_bundle(mode=2)

    def test_mode_three_has_an_exact_adt_request_and_enabled_protocol(self):
        self.assert_bundle(mode=3)
        from ios16_gpu_power import check_request
        request=check_request((self.output/'giulia-ios16.adt').read_bytes(),3)
        self.assertFalse(request['provider_waits_preemptible'])
        header=self.uefi/'Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/PayloadIdentity.h'
        self.assertIn('#define GIULIA_GPU_POWER 1',header.read_text())
        self.assertNotIn(b'giulia-gpu-power',Path(self.inputs[0]).read_bytes())

    def test_wifi_without_gpu_remains_opted_out(self):
        self.write_kernel(gpu=False)
        result = self.build(gpu_pack=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((self.output / 'source-manifest.json').read_text())
        self.assertIsNone(report['gpu_mode'])
        self.assertIsNone(report['gpu_runtime'])
        self.assertIsNone(report['gpu_gunyah_handoff'])
        self.assertFalse((self.output / 'gpu-offline-checks.json').exists())


if __name__ == '__main__':
    unittest.main()
