# Kernel build for the cpu-desktop hardware images (used by scripts/build-session-image.sh); fails when the kernel build fails.
from pathlib import Path
import os,subprocess,json,shutil
root=Path.cwd()
names='SOURCE_CORECRYPTO14 SOURCE_PTHREAD ARM_DT_PLATFORM ARMCPU NATIVE_UFS NATIVE_AES NATIVE_VIRTIO_BLOCK NATIVE_RAM_BLOCK TRUST_CACHE_RUNTIME SOURCE_MOCKFS NATIVE_CS_HASH_CACHE VERIFIED_ROOT_PROBE VERIFIED_ROOT_PROVIDER OWNER_ROOT_AUDIT OWNER_ROOT_TRUST SHENG_PMGR OWNER_CACHE_MOUNT OWNER_CACHE_STARTUP OWNER_BOOT_MANIFEST OWNER_DATA_MOUNT OWNER_UNION_SYSTEMOS DT_PANIC_REGION ZONE_PGZ_ALIGNMENT_PROBE NATIVE_ARCH_TIMER NATIVE_PAN BOOT_PROGRESS NATIVE_LEGACY_DISPLAY NATIVE_ADRENO'.split()
updates={'XNU_'+n:'1' for n in names}
updates.update(XNU_CRYPTO_BOOTSTRAP='prelinked',XNU_OWNER_DATA_VOLUME_UUID='18AB5557-DC2D-469B-8758-855E60F5B0DF',
 XNU_NATIVE_VIRTIO_BLOCK_ARCHIVE=str(root/'build/native-virtio-block-verified-bundle/libNativeVirtioBlock.a'),
 XNU_VERIFIED_ROOT_ARCHIVE=str(root/'build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a'),
 XNU_EXTRA_LD_ARCHIVES='',XNU_NATIVE_MOBILE_DISPLAY='0',XNU_ARMVIRT_PLATFORM='0',JOBS='4')
(root/'build/cpu-desktop/kernel-environment.json').write_text(json.dumps(updates,indent=2)+'\n')
env=dict(os.environ);env.update(updates)
with (root/'build/cpu-desktop/kernel-build.log').open('w') as f:
 result=subprocess.run(['bash','scripts/build-xnu-4k-baseline.sh'],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=900)
print('kernel build exit',result.returncode)
if result.returncode:raise SystemExit('KERNEL BUILD FAILED: see build/cpu-desktop/kernel-build.log')
