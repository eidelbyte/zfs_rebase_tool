/* Syntax-check stub. Not part of the build. See tests/stub/README. */

#ifndef	ZR_STUB_LIBZFS_H
#define	ZR_STUB_LIBZFS_H

#include <stddef.h>

#include <sys/stdtypes.h>
#include <sys/nvpair.h>
#include <sys/fs/zfs.h>

#define	_LIBZFS_H

/* Verbatim from include/libzfs.h. */
typedef struct zfs_handle zfs_handle_t;
typedef struct zpool_handle zpool_handle_t;
typedef struct libzfs_handle libzfs_handle_t;

_LIBZFS_H libzfs_handle_t *libzfs_init(void);
_LIBZFS_H void libzfs_fini(libzfs_handle_t *);
_LIBZFS_H const char *libzfs_error_action(libzfs_handle_t *);
_LIBZFS_H const char *libzfs_error_description(libzfs_handle_t *);

_LIBZFS_H zfs_handle_t *zfs_open(libzfs_handle_t *, const char *, int);
_LIBZFS_H void zfs_close(zfs_handle_t *);
_LIBZFS_H const char *zfs_prop_to_name(zfs_prop_t);
_LIBZFS_H int zfs_prop_set(zfs_handle_t *, const char *, const char *);
_LIBZFS_H int zfs_prop_get(zfs_handle_t *, zfs_prop_t, char *, size_t,
    zprop_source_t *, char *, size_t, boolean_t);

_LIBZFS_H int zfs_mount(zfs_handle_t *, const char *, int);
_LIBZFS_H int zfs_unmount(zfs_handle_t *, const char *, int);

typedef enum diff_flags {
	ZFS_DIFF_PARSEABLE = 1 << 0,
	ZFS_DIFF_TIMESTAMP = 1 << 1,
	ZFS_DIFF_CLASSIFY = 1 << 2,
	ZFS_DIFF_NO_MANGLE = 1 << 3
} diff_flags_t;

_LIBZFS_H int zfs_show_diffs(zfs_handle_t *, int, const char *, const char *,
    int);

#endif	/* ZR_STUB_LIBZFS_H */
