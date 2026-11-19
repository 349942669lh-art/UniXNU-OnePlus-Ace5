"""Source ownership and host include paths for the Darwin driver modules.

Generated XNU paths are deliberately separate from repository source paths.
No compatibility copies are kept in the old flat source directory.
"""
from pathlib import Path
import re

MODULES = ('common', 'platform', 'audio', 'display', 'gpu', 'keys', 'modem',
           'slider', 'storage', 'terminal', 'touch', 'trust', 'wifi')
SPECIAL = {
    'GiuliaAMFI.cpp': 'trust', 'giulia_trust_catalog.h': 'trust',
    'GiuliaPlatformResources.cpp': 'platform',
    'GiuliaPlatformQmp.cpp': 'platform',
    'giulia_sha256.h': 'common', 'giulia_debug_abi.h': 'common',
    'giulia_log_bytes.h': 'common', 'giulia_scm.h': 'platform',
    'giulia_input_provider.h': 'common',
    'giulia_smmu.h': 'storage',
    'giulia_input_hid.inc': 'display', 'giulia_input_keyboard.inc': 'display',
    'giulia_screen_framebuffer.inc': 'display',
    'giulia_terminal_ufs.inc': 'storage',
}


def module_for(name):
    if name in SPECIAL:
        return SPECIAL[name]
    for module in MODULES:
        if name.startswith('giulia_' + module + '_') or name.startswith('giulia_' + module + '.'):
            return module
        if name == 'Giulia' + {'gpu': 'GPU', 'wifi': 'WiFi'}.get(module, module.title()) + '.cpp':
            return module
    if name.startswith(('giulia_screen', 'giulia_input', 'giulia_shell', 'giulia_mmio')):
        return 'terminal'
    if name.startswith('giulia_reboot'):
        return 'platform'
    raise ValueError('Source has no module owner: ' + name)


def source_path(root, name):
    return Path(root) / 'ios16' / module_for(name) / name


def include_flags(root):
    """Host integration harnesses; production dependencies are checked separately."""
    base = Path(root) / 'ios16'
    return [flag for path in (base, *(base / name for name in MODULES))
            for flag in ('-I', str(path))]


def kernel_source(text):
    """Flatten only repository includes when staging into XNU's arm directory."""
    text = re.sub(r'#include "\.\./(?:' + '|'.join(MODULES) + r')/([^"/]+)"',
                  r'#include "\1"', text)
    text = text.replace('"../vendor/', '"vendor/')
    for report, target in (('GpuPowerReport.h', 'giulia_platform_power_report.h'),
                           ('GpuBootState.h', 'giulia_gpu_boot_state_report.h'),
                           ('GpuGunyahReport.h', 'giulia_gpu_gunyah_report.h')):
        text = re.sub(r'"(?:\.\./)+uefi-overlay/[^"\n]+/' + report + '"',
                      '"' + target + '"', text)
    return text.replace('#include <stdint.h>\n', '').replace('#include <stddef.h>\n', '')
