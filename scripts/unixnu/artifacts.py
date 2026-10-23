"""Resolve build artifacts shared by the build driver and its host probes."""
import os
from pathlib import Path

from .util import Error, expand


def package_efi(root, explicit=None):
    """Find the built UniXNU EFI application, requiring an explicit choice if ambiguous."""
    selected = explicit or os.environ.get('UNIXNU_UEFI_EFI')
    if selected:
        path = expand(selected)
        if path.is_dir():
            path = path / 'AARCH64-DEBUG/UniXNU.efi'
        if not path.is_file():
            raise Error(f'UniXNU EFI application not found: {path}')
        return path

    root = Path(root).resolve()
    out = expand(os.environ.get('UNIXNU_OUT') or root / 'out')
    candidates = [root / 'package/Build/Out/AARCH64-DEBUG/UniXNU.efi']
    candidates.extend(out.glob('*/package/Build/Out/AARCH64-DEBUG/UniXNU.efi'))
    found = sorted({path.resolve() for path in candidates if path.is_file()})
    if len(found) == 1:
        return found[0]
    if len(found) > 1:
        raise Error('multiple UniXNU EFI applications found; pass --uefi-efi FILE or set UNIXNU_UEFI_EFI')
    raise Error(f'UniXNU EFI application not found under {out}; run "unixnu build package" first')
