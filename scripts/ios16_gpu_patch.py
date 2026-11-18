"""Emit an independent Darwin 22 GPU bring-up service patch."""
from ios16_sources import source_path, kernel_source
from ios16_platform_patch import platform_patch
import difflib


def gpu_patch(out, root):
    platform_patch(out, root)
    # Anchor on AMFI, present in every GUI profile, regardless of Wi-Fi selection.
    units = [p.name for p in sorted((root/'ios16/gpu').glob('giulia_gpu_*.*'))
             if p.suffix in ('.c', '.cpp')]
    added = ['iokit/Kernel/arm/GiuliaGPU.cpp                                   optional iokitcpp\n']
    added += ['iokit/Kernel/arm/'+name+' optional iokitcpp\n' for name in units]
    patch = ('--- a/iokit/conf/files.arm64\n+++ b/iokit/conf/files.arm64\n'
             f'@@ -14,2 +14,{2+len(added)} @@\n'
             ' iokit/Kernel/arm/SweetKeyStore.cpp                               optional iokitcpp\n'
             + ''.join('+'+line for line in added) +
             ' iokit/Kernel/arm/GiuliaAMFI.cpp                                  optional iokitcpp\n'
             '--- a/iokit/KernelConfigTables.cpp\n+++ b/iokit/KernelConfigTables.cpp\n'
             '@@ -55,3 +55,9 @@\n'
             '     "     \'IOMatchCategory\' = AppleMobileFileIntegrity;"\n'
             '     "   },"\n'
             '+    "   {"\n'
             '+    "     \'IOClass\'         = GiuliaGPU;"\n'
             '+    "     \'IOProviderClass\' = IOResources;"\n'
             '+    "     \'IOResourceMatch\' = IOBSD;"\n'
             '+    "     \'IOMatchCategory\' = GiuliaGPU;"\n'
             '+    "   },"\n'
             '     "   {"\n')
    names = ['GiuliaGPU.cpp', *units, *[p.name for p in sorted((root/'ios16/gpu').glob('giulia_gpu_*.h'))]]
    sources = {name: source_path(root, name).read_text() for name in names}
    sources["giulia_gpu_gunyah_report.h"] = (root/"uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe/GpuGunyahReport.h").read_text()
    for path in sorted((root/'ios16/vendor/ArmSmmu4K').glob('*')):
        if path.name.startswith('test_') or (path.suffix not in ('.c','.h') and path.name not in ('LICENSE','SOURCE.json')): continue
        sources['vendor/ArmSmmu4K/'+path.name] = path.read_text()
    for name, data in sources.items():
        data = kernel_source(data)
        patch += ''.join(difflib.unified_diff([], data.splitlines(True),
                         fromfile='/dev/null', tofile='b/iokit/Kernel/arm/'+name))
    (out/'0077-giulia-gpu-runtime.patch').write_text(patch, encoding='utf-8', newline='\n')
