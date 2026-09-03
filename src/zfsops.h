/*
 * zfsops: the ZFS operations one run needs -- hold, clone and mount,
 * property get and set, existence, release, destroy, and the diff --
 * behind one handle that owns the libzfs handle, the cleanup
 * descriptor every hold is registered against, and the hold tag. All
 * of it is library calls into libzfs_core and libzfs; the tool never
 * execs zfs(8).
 *
 * The tool takes no snapshots: the three it works from are the
 * user's, and it only holds them for the length of the run. The one
 * dataset it creates is the result clone, which the user names.
 *
 * The bodies compile only with ZR_FREEBSD. Without it every call
 * here fails with "not built with ZR_FREEBSD", which is what the
 * portable build links against so that main.c is one program in
 * both.
 *
 * Every call returns 0 or -1 and writes a message into err when
 * errlen is not 0. Where a libzfs handle carries the failure the
 * message is that handle's own description and action, which is what
 * zfs(8) would have printed; where the failure is libzfs_core's or
 * libc's it is strerror.
 */

#ifndef	ZR_ZFSOPS_H
#define	ZR_ZFSOPS_H

#include <stddef.h>
#include <stdint.h>

#include "diff.h"

/*
 * The two user properties every result clone carries from birth.
 * They are what tells a zfs_rebase result from any other dataset, so
 * that --abort can refuse to touch anything else, and where the run
 * records the manifest it wrote.
 */
#define	ZR_PROP_STATE		"zfs_rebase:state"
#define	ZR_PROP_MANIFEST	"zfs_rebase:manifest"

struct zr_zfs;

/*
 * Open libzfs and the cleanup descriptor on /dev/zfs. holdtag is the
 * name every hold this handle takes is filed under. Returns 0 with
 * *out set to a handle the caller closes, or -1.
 */
int zr_zfs_open(struct zr_zfs **out, const char *holdtag, char *err,
    size_t errlen);

/*
 * Close the cleanup descriptor, which releases every hold registered
 * against it, and finish with libzfs. Safe on NULL.
 */
void zr_zfs_close(struct zr_zfs *z);

/*
 * Hold snapshot under this handle's tag, against the cleanup
 * descriptor, so that the hold dies with the process however it
 * dies.
 */
int zr_zfs_hold(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen);

/* Release this handle's tag from snapshot. */
int zr_zfs_release(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen);

/*
 * Clone snapshot as clone with readonly=on, the given mountpoint and
 * the two zfs_rebase user properties -- state "created" and the path
 * of the manifest this run writes -- then mount it there. The marker
 * is set by the create itself, so it exists from the clone's first
 * instant and no kill can leave a result dataset without one.
 */
int zr_zfs_clone(struct zr_zfs *z, const char *snapshot, const char *clone,
    const char *mountpoint, const char *manifest, char *err, size_t errlen);

/* Set readonly on or off. */
int zr_zfs_set_readonly(struct zr_zfs *z, const char *dataset, int on,
    char *err, size_t errlen);

/* Unmount dataset if it is mounted, then destroy it. */
int zr_zfs_destroy(struct zr_zfs *z, const char *dataset, char *err,
    size_t errlen);

/*
 * Whether a filesystem, snapshot or volume of that name exists.
 * Returns 1 or 0, or -1 with err set on a bad argument.
 */
int zr_zfs_exists(struct zr_zfs *z, const char *dataset, char *err,
    size_t errlen);

/*
 * One property of dataset as the string zfs(8) would print, which is
 * how mountpoint, mounted, casesensitivity, normalization, acltype
 * and origin are read. A property that has no value and no default
 * reads as "-", as zfs(8) prints it; origin on a dataset that is not
 * a clone is the one this tool asks for.
 */
int zr_zfs_get(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen);

/*
 * One numeric property as the number it is rather than as zfs(8)
 * would print it, which is how createtxg -- the kernel's own
 * ordering of the snapshots in a pool -- is read.
 */
int zr_zfs_get_int(struct zr_zfs *z, const char *dataset, const char *prop,
    uint64_t *out, char *err, size_t errlen);

/* Set one "module:name" user property. */
int zr_zfs_set_user(struct zr_zfs *z, const char *dataset, const char *prop,
    const char *value, char *err, size_t errlen);

/*
 * Read one "module:name" user property: 1 with buf set when it is
 * there, 0 when it is not, -1 on a failure with err set.
 */
int zr_zfs_get_user(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen);

/*
 * Run the diff fromsnap -> to and parse it. mountpoint is the
 * dataset's mountpoint, which is what zfs diff prints in front of
 * every path. out must be handed to zr_diff_fini either way.
 *
 * The caller should be ignoring SIGPIPE, as zfs(8) does around its
 * own call: the diff runs a thread over a pipe, and a failure on one
 * end can otherwise kill the process before libzfs can say what went
 * wrong. Setting a signal disposition is the driver's business, not
 * this layer's.
 */
int zr_zfs_diff(struct zr_zfs *z, const char *fromsnap, const char *to,
    const char *mountpoint, struct zr_diff *out, char *err, size_t errlen);

#endif	/* ZR_ZFSOPS_H */
