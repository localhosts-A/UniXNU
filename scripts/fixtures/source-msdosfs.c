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
    const char expected[] = "Dali USB BOT FAT32 RAM read-only\n";
    char device_path[64] = {0};
    unsigned int mode = 0;
    vfs_context_t context;
    struct msdosfs_args arguments;
    vnode_t file = NULLVP;
    vnode_t write_file = NULLVP;
    bool file_opened = false;
    off_t file_size = 0;
    uio_t auio;
    UIO_STACKBUF(uio_buf, 1);
    char contents[sizeof(expected)] = {0};
    uint64_t checks = 0;
    int error = 0;

    if (!PE_parse_boot_argn("source-msdosfs-test", &mode, sizeof(mode))) {
        return 0;
    }
    if (mode != 1) {
        return EINVAL;
    }
    context = vfs_context_current();
    if (!context) {
        error = EINVAL;
        goto done;
    }

    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        if (source_storage_fixture_copy_bsd_path(device_path, sizeof(device_path))) {
            checks |= 1;
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
    if (error || file_size != sizeof(expected) - 1) {
        if (!error) error = EINVAL;
        goto done;
    }
    auio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ,
        &uio_buf[0], sizeof(uio_buf));
    if (!auio) {
        error = ENOMEM;
        goto done;
    }
    uio_addiov(auio, CAST_USER_ADDR_T(contents), sizeof(expected) - 1);
    error = VNOP_READ(file, auio, 0, context);
    if (error || uio_resid(auio) != 0 || memcmp(contents, expected, sizeof(expected) - 1) != 0) {
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
        error = 0;
        checks |= 32;
    } else {
        if (write_file) (void)vnode_close(write_file, 0, context);
        write_file = NULLVP;
        if (!error) error = EIO;
        goto done;
    }

done:
    if (file) {
        if (file_opened) (void)vnode_close(file, 0, context);
        else vnode_put(file);
    }
    if (write_file) (void)vnode_close(write_file, 0, context);
    printf("source msdosfs: qemu mount/read checks=%llx error=%d device=%s\n",
        checks, error, device_path[0] ? device_path : "unpublished");
    (void)source_msdosfs_test_checkpoint((test_magic << 32) | checks);
    return error;
}
