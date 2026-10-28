/* Built-in registration of the fixed, source-built Apple msdosfs driver. */
#include <mach/kmod.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/systm.h>
#include <sys/uio_internal.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vnode_if.h>
#include <sys/vfs_context.h>
#include "bpb.h"
#include <msdosfsmount.h>
#include <pexpert/pexpert.h>

extern int msdosfs_module_start(kmod_info_t *, void *);
extern int source_storage_fixture_copy_bsd_path(char *, size_t);
extern int source_storage_fixture_copy_usb_bsd_path(char *, size_t);
void source_msdosfs_register(void);

void
source_msdosfs_register(void)
{
    /* bsd_init calls this once after vfsinit has initialized the VFS tables.
     * Use the real module entry, which registers both upstream vnode vectors.
     * This does not mount or access a disk, and the built-in driver is not
     * unloaded while any filesystem may still refer to its operations. */
    int result = msdosfs_module_start(NULL, NULL);
    if (result != KERN_SUCCESS) {
        panic("source msdosfs: VFS registration failed (%d)", result);
    }
    printf("source msdosfs: VFS registered; no media mounted during registration\n");
}

static int
source_msdosfs_unmount_test_volume(const char *mountpoint, vfs_context_t context)
{
    vnode_t mountpoint_vnode = NULLVP;
    int error = vnode_lookup(mountpoint, 0, &mountpoint_vnode, context);
    if (error) {
        return error;
    }
    mount_t mounted = vnode_mount(mountpoint_vnode);
    struct vfsstatfs *statistics = mounted ? vfs_statfs(mounted) : NULL;
    if (!statistics) {
        vnode_put(mountpoint_vnode);
        return EINVAL;
    }
    fsid_t fsid = statistics->f_fsid;
    vnode_put(mountpoint_vnode);
    return vfs_unmountbyfsid(&fsid, 0, context);
}

__attribute__((noinline, used))
uint64_t
source_msdosfs_test_checkpoint(uint64_t result)
{
    __asm__ volatile("" : "+r"(result) :: "memory");
    return result;
}

int
source_msdosfs_qemu_test(void)
{
    const uint64_t test_magic = 0x4d534446ULL;
    const char mountpoint[] = "/mnt";
    const char file_path[] = "/mnt/README.TXT";
    const char write_path[] = "/mnt/QEMUPASS.TXT";
    const char ram_expected[] = "Dali USB BOT FAT32 RAM read-only\n";
    const char usb_expected[] = "Dali USB BOT FAT32 QEMU read-only\n";
    char persisted_expected[] = "Dali USB BOT FAT32 persisted\n";
    char device_path[64] = {0};
    unsigned int mode = 0;
    vfs_context_t context;
    struct msdosfs_args arguments;
    vnode_t file = NULLVP;
    vnode_t write_file = NULLVP;
    bool file_opened = false;
    bool write_file_opened = false;
    bool mount_live = false;
    off_t file_size = 0;
    uio_t auio;
    UIO_STACKBUF(uio_buf, 1);
    char contents[sizeof(usb_expected)] = {0};
    uint64_t checks = 0;
    int error = 0;

    if (!PE_parse_boot_argn("source-msdosfs-test", &mode, sizeof(mode))) {
        return 0;
    }
    if (mode < 1 || mode > 3) {
        return EINVAL;
    }
    context = vfs_context_current();
    if (!context) {
        error = EINVAL;
        goto done;
    }

    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        int path_found = mode >= 2 ?
            source_storage_fixture_copy_usb_bsd_path(device_path, sizeof(device_path)) :
            source_storage_fixture_copy_bsd_path(device_path, sizeof(device_path));
        if (path_found) {
            checks |= 1;
            if (mode >= 2) checks |= 64; // Path came from the Dali USB BOT IOMedia.
            break;
        }
        delay(10000);
    }
    if (!(checks & 1)) {
        error = ETIMEDOUT;
        goto done;
    }

    if (vnode_lookup(mountpoint, 0, &file, context) != 0) {
        file = NULLVP;
        error = ENOENT;
        goto done;
    }
    if (vnode_vtype(file) != VDIR) {
        error = ENOTDIR;
        vnode_put(file);
        file = NULLVP;
        goto done;
    }
    vnode_put(file);
    file = NULLVP;
    checks |= 2;

    bzero(&arguments, sizeof(arguments));
    arguments.magic = MSDOSFS_ARGSMAGIC;
    error = kernel_mount_localargs("msdos", mountpoint, device_path, &arguments,
        MNT_RDONLY, context);
    if (error) {
        goto done;
    }
    mount_live = true;
    checks |= 4;

    error = vnode_open(file_path, FREAD, 0, 0, &file, context);
    if (error) {
        file = NULLVP;
        goto done;
    }
    file_opened = true;
    mount_t mounted = vnode_mount(file);
    if (!mounted || !vfs_statfs(mounted) ||
        strcmp(vfs_statfs(mounted)->f_fstypename, "msdos") != 0 ||
        (vfs_flags(mounted) & MNT_RDONLY) == 0) {
        error = EINVAL;
        goto done;
    }
    checks |= 8;

    error = vnode_size(file, &file_size, context);
    const char *expected = mode >= 2 ? usb_expected : ram_expected;
    size_t expected_size = mode >= 2 ? sizeof(usb_expected) - 1 : sizeof(ram_expected) - 1;
    if (error || file_size != expected_size) {
        if (!error) error = EINVAL;
        goto done;
    }
    auio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ,
        &uio_buf[0], sizeof(uio_buf));
    if (!auio) {
        error = ENOMEM;
        goto done;
    }
    uio_addiov(auio, CAST_USER_ADDR_T(contents), expected_size);
    error = VNOP_READ(file, auio, 0, context);
    if (error || uio_resid(auio) != 0 || memcmp(contents, expected, expected_size) != 0) {
        if (!error) error = EIO;
        goto done;
    }
    checks |= 16;
    error = vnode_close(file, 0, context);
    file = NULLVP;
    file_opened = false;
    if (error) {
        goto done;
    }

    error = vnode_open(file_path, FWRITE, 0, 0, &write_file, context);
    if (error == EROFS) {
        if (write_file) {
            (void)vnode_close(write_file, FWRITE, context);
            write_file = NULLVP;
        }
        error = 0;
        checks |= 32;
    } else {
        if (write_file) (void)vnode_close(write_file, FWRITE, context);
        write_file = NULLVP;
        if (!error) error = EIO;
        goto done;
    }

    if (mode == 3) {
        error = source_msdosfs_unmount_test_volume(mountpoint, context);
        if (error) goto done;
        mount_live = false;
        checks |= 128;

        bzero(&arguments, sizeof(arguments));
        arguments.magic = MSDOSFS_ARGSMAGIC;
        error = kernel_mount_localargs("msdos", mountpoint, device_path, &arguments,
            0, context);
        if (error) goto done;
        mount_live = true;

        error = vnode_open(write_path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW,
            0644, 0, &write_file, context);
        if (error) {
            if (write_file) (void)vnode_close(write_file, FWRITE, context);
            write_file = NULLVP;
            goto done;
        }
        write_file_opened = true;
        mount_t writable_mount = vnode_mount(write_file);
        if (!writable_mount || !vfs_statfs(writable_mount) ||
            strcmp(vfs_statfs(writable_mount)->f_fstypename, "msdos") != 0 ||
            (vfs_flags(writable_mount) & MNT_RDONLY) != 0) {
            error = EINVAL;
            goto done;
        }
        checks |= 256;

        int resid = 0;
        error = vn_rdwr(UIO_WRITE, write_file, persisted_expected,
            sizeof(persisted_expected) - 1, 0, UIO_SYSSPACE, IO_NOAUTH | IO_UNIT,
            vfs_context_ucred(context), &resid, vfs_context_proc(context));
        if (error || resid != 0) {
            if (!error) error = EIO;
            goto done;
        }
        error = vnode_size(write_file, &file_size, context);
        if (error || file_size != sizeof(persisted_expected) - 1) {
            if (!error) error = EIO;
            goto done;
        }
        checks |= 512;
        checks |= 1024;

        error = VNOP_FSYNC(write_file, MNT_WAIT, context);
        if (error) goto done;
        checks |= 2048;
        error = vnode_close(write_file, FWRITE, context);
        write_file = NULLVP;
        write_file_opened = false;
        if (error) goto done;

        error = source_msdosfs_unmount_test_volume(mountpoint, context);
        if (error) goto done;
        mount_live = false;
        checks |= 4096;

        bzero(&arguments, sizeof(arguments));
        arguments.magic = MSDOSFS_ARGSMAGIC;
        error = kernel_mount_localargs("msdos", mountpoint, device_path, &arguments,
            MNT_RDONLY, context);
        if (error) goto done;
        mount_live = true;
        checks |= 8192;

        error = vnode_open(write_path, FREAD, 0, 0, &file, context);
        if (error) {
            file = NULLVP;
            goto done;
        }
        file_opened = true;
        mount_t remounted = vnode_mount(file);
        if (!remounted || !vfs_statfs(remounted) ||
            strcmp(vfs_statfs(remounted)->f_fstypename, "msdos") != 0 ||
            (vfs_flags(remounted) & MNT_RDONLY) == 0) {
            error = EINVAL;
            goto done;
        }
        error = vnode_size(file, &file_size, context);
        if (error || file_size != sizeof(persisted_expected) - 1) {
            if (!error) error = EINVAL;
            goto done;
        }
        auio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ,
            &uio_buf[0], sizeof(uio_buf));
        if (!auio) {
            error = ENOMEM;
            goto done;
        }
        bzero(contents, sizeof(contents));
        uio_addiov(auio, CAST_USER_ADDR_T(contents), sizeof(persisted_expected) - 1);
        error = VNOP_READ(file, auio, 0, context);
        if (error || uio_resid(auio) != 0 ||
            memcmp(contents, persisted_expected, sizeof(persisted_expected) - 1) != 0) {
            if (!error) error = EIO;
            goto done;
        }
        checks |= 16384;
        error = vnode_close(file, 0, context);
        file = NULLVP;
        file_opened = false;
        if (error) goto done;

        error = source_msdosfs_unmount_test_volume(mountpoint, context);
        if (error) goto done;
        mount_live = false;
        checks |= 32768;
    }

done:
    if (file) {
        if (file_opened) (void)vnode_close(file, 0, context);
        else vnode_put(file);
    }
    if (write_file) {
        if (write_file_opened) (void)vnode_close(write_file, FWRITE, context);
        else vnode_put(write_file);
    }
    if (mount_live) {
        int unmount_error = source_msdosfs_unmount_test_volume(mountpoint, context);
        if (!error) error = unmount_error;
    }
    printf("source msdosfs: qemu mode=%u mount/read checks=%llx error=%d device=%s\n",
        mode, checks, error, device_path[0] ? device_path : "unpublished");
    (void)source_msdosfs_test_checkpoint((test_magic << 32) | checks);
    return error;
}
