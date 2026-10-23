from pathlib import Path
import shutil
import sys
from types import SimpleNamespace
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu import feeds, tree


def context(root, device, output, local_module=None):
    args = SimpleNamespace(
        local_module=local_module or [],
        local=None,
        local_drivers=None,
        local_kits=None,
        local_patches=None,
        local_scripts=None,
        local_package=None,
    )
    return SimpleNamespace(
        root=root,
        device=device,
        args=args,
        tree=output / 'tree',
        board={'board': {'kit': 'Dali'}},
        name='dali',
    )


class PatchOverlays(unittest.TestCase):
    def test_device_patch_set_includes_base_patches_and_wins_conflicts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'base'
            device = Path(temporary) / 'device'
            output = Path(temporary) / 'out'
            base_patches = root / 'patches/xnu'
            device_patches = device / 'patches/xnu'
            base_patches.mkdir(parents=True)
            device_patches.mkdir(parents=True)
            (root / 'package').mkdir()
            (base_patches / 'armvirt.patch').write_text('base platform patch')
            (base_patches / 'shared.patch').write_text('base version')
            (device_patches / 'dali.patch').write_text('device platform patch')
            (device_patches / 'shared.patch').write_text('device version')

            ctx = context(root, device, output)
            modules = feeds.resolve(ctx)
            patch_set = modules[('patch', 'xnu')]
            self.assertEqual(patch_set.path, device_patches)
            self.assertEqual(patch_set.overlays, [base_patches])

            tree.assemble(ctx, modules)
            assembled = ctx.tree / 'patches/xnu'
            self.assertEqual((assembled / 'armvirt.patch').read_text(), 'base platform patch')
            self.assertEqual((assembled / 'dali.patch').read_text(), 'device platform patch')
            self.assertEqual((assembled / 'shared.patch').read_text(), 'device version')

            shutil.rmtree(device_patches)
            modules = feeds.resolve(ctx)
            tree.assemble(ctx, modules)
            self.assertTrue(assembled.is_symlink())
            self.assertEqual((assembled / 'armvirt.patch').read_text(), 'base platform patch')
            self.assertFalse((assembled / 'dali.patch').exists())

    def test_explicit_local_module_still_replaces_default_patch_sets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / 'base'
            device = Path(temporary) / 'device'
            local = Path(temporary) / 'local-xnu-patches'
            output = Path(temporary) / 'out'
            (root / 'patches/xnu').mkdir(parents=True)
            (device / 'patches/xnu').mkdir(parents=True)
            local.mkdir()
            (root / 'package').mkdir()
            (local / 'only-local.patch').write_text('local override')

            ctx = context(root, device, output, [f'patch:xnu={local}'])
            modules = feeds.resolve(ctx)
            patch_set = modules[('patch', 'xnu')]
            self.assertEqual(patch_set.path, local)
            self.assertEqual(patch_set.overlays, [])


if __name__ == '__main__':
    unittest.main()
