"""Actual UEFI menu C, real pinned EDK2 types, modeled GOP/Boot Services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class MenuPointerTests(unittest.TestCase):
    def test_production_menu_pointer(self):
        root = Path(__file__).resolve().parents[2]
        edk = Path(os.environ.get('DALI_USB_EDK2', root.parent / 'UEFI-Dali/Mu_Basecore'))
        if not (edk / 'MdePkg/Include/Uefi.h').is_file():
            self.fail('pinned EDK2 headers required: set DALI_USB_EDK2')
        package = root / 'package/UniXNUPkg'
        library = package / 'Library/UniXNULib'
        with tempfile.TemporaryDirectory() as directory:
            for enabled in (0, 1):
                target = Path(directory) / f'menu-{enabled}'
                command = [os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                           '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wno-unused-parameter',
                           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                           f'-D_PCD_VALUE_PcdUniXNUPointerInputEnabled={enabled}',
                           '-I' + str(edk / 'MdePkg/Include'), '-I' + str(edk / 'MdePkg/Include/X64'),
                           '-I' + str(package / 'Include'), '-I' + str(library),
                           str(library / 'Menu.c'), str(library / 'MenuPointer.c'),
                           str(Path(__file__).parent / 'fixtures/menu_pointer_test.c'), '-o', str(target)]
                subprocess.run(command, check=True, timeout=60)
                subprocess.run([str(target)], check=True, timeout=20)


if __name__ == '__main__':
    unittest.main()
