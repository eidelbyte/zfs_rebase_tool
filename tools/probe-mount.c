/*
 * probe-mount: the one libzfs call the dataset form's exclusivity
 * makes, on its own. Unmounts DATASET from wherever it is, mounts it
 * at DIR the way zr_zfs_mount_at does (zfs_mount_at with no options
 * and no flags), reports what libzfs said, and puts it back at its
 * own mountpoint. A box probe for a mount that answered EINVAL; not
 * part of the tool and not built by default.
 *
 *   make probe-mount
 *   sudo ./build/probe-mount POOL/scratch /var/db/zfs_rebase/probe
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libzfs.h>

int
main(int argc, char **argv)
{
	libzfs_handle_t *hdl;
	zfs_handle_t *zhp;
	int rc;

	if (argc != 3) {
		(void) fprintf(stderr, "usage: probe-mount DATASET DIR\n");
		return (2);
	}
	hdl = libzfs_init();
	if (hdl == NULL) {
		(void) fprintf(stderr, "libzfs_init: %s\n", strerror(errno));
		return (2);
	}
	libzfs_print_on_error(hdl, B_FALSE);
	zhp = zfs_open(hdl, argv[1], ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		(void) fprintf(stderr, "%s: %s\n", argv[1],
		    libzfs_error_description(hdl));
		return (2);
	}
	if (zfs_is_mounted(zhp, NULL)) {
		rc = zfs_unmount(zhp, NULL, 0);
		(void) printf("unmount: %d%s%s\n", rc, rc ? ": " : "",
		    rc ? libzfs_error_description(hdl) : "");
	} else {
		(void) printf("not mounted to begin with\n");
	}
	errno = 0;
	rc = zfs_mount_at(zhp, NULL, 0, argv[2]);
	(void) printf("zfs_mount_at(%s): %d errno %d (%s)\n", argv[2], rc,
	    errno, strerror(errno));
	if (rc != 0)
		(void) printf("  libzfs: %s: %s\n", libzfs_error_action(hdl),
		    libzfs_error_description(hdl));
	else
		(void) printf("  mounted; unmounting again: %d\n",
		    zfs_unmount(zhp, NULL, 0));
	rc = zfs_mount(zhp, NULL, 0);
	(void) printf("back at its own mountpoint: %d%s%s\n", rc,
	    rc ? ": " : "", rc ? libzfs_error_description(hdl) : "");
	zfs_close(zhp);
	libzfs_fini(hdl);
	return (0);
}
