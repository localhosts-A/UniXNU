/* Built-in registration of the fixed, source-built Apple msdosfs driver. */
#include <mach/kmod.h>
#include <sys/systm.h>

extern int msdosfs_module_start(kmod_info_t *, void *);
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
    printf("source msdosfs: VFS registered; FAT32 runtime unqualified\n");
}
