"""Install the fixed GPU resource snapshot before and after successful EBS."""
from pathlib import Path
import argparse
import shutil

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / 'uefi-overlay/Platforms/OnePlus/giuliaPkg/Drivers/GiuliaInitrdDxe'
INCLUDE = '#include "GpuBootStateHandoff.h"\n'
PREPARE = '  (VOID)GiuliaGpuBootStatePrepare ((VOID *)(UINTN)TreeAddress, TreeSize);\n'
FINISH = '      GiuliaGpuBootStateFinish ();\n'
ABORT = '  GiuliaGpuBootStateAbort ();\n'


def install(driver):
    path = driver / 'DarwinBoot.c'
    text = path.read_text()
    mapping = '  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);'
    enter = '      DarwinEnter ('
    close = '  Root->Close (Root); Root = NULL;'
    exit_call = 'gBS->ExitBootServices ('
    if any(text.count(anchor) != 1 for anchor in (mapping, enter, close, exit_call, '\nFail:\n')):
        raise ValueError('GPU boot state loader anchors changed')
    if INCLUDE not in text:
        lines = text.splitlines(True)
        at = max(i for i, line in enumerate(lines) if line.startswith('#include ')) + 1
        lines.insert(at, INCLUDE)
        text = ''.join(lines)
    if PREPARE not in text:
        text = text.replace(mapping, PREPARE + mapping)
    if FINISH not in text:
        text = text.replace(enter, FINISH + enter)
    if ABORT not in text:
        text = text.replace('\nFail:\n', '\nFail:\n' + ABORT)
    if any(text.count(item) != 1 for item in (INCLUDE, PREPARE, FINISH, ABORT)):
        raise ValueError('Duplicate GPU boot state integration')
    if not text.index(close) < text.index(PREPARE) < text.index(mapping) < text.index(exit_call) < text.index(FINISH) < text.index(enter):
        raise ValueError('GPU boot state must span the successful EBS path')
    prefix = text[text.index(exit_call):text.index(FINISH)]
    if 'if (!EFI_ERROR (Status)) {' not in prefix:
        raise ValueError('GPU boot state finalizer requires successful EBS')
    names = ('GpuBootState.h', 'GpuBootStateHandoff.h', 'GpuPowerAdt.h', 'WifiPowerCache.h')
    for name in names:
        if not (HEADERS / name).is_file():
            raise ValueError('Missing GPU boot state input ' + name)
    for name in names:
        shutil.copyfile(HEADERS / name, driver / name)
    path.write_text(text, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('driver', type=Path)
    install(parser.parse_args().driver)
    print('GPU resource snapshot installed before EBS and immediately after successful EBS')
