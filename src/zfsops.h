/*
 * zfsops: the ZFS operations one run needs -- hold, clone and mount,
 * property get and set, existence, release and destroy -- behind one
 * handle that owns the libzfs handle. All of it is library calls
 * into libzfs_core and libzfs; the tool never execs zfs(8).
 *
 * The tool takes no snapshots: the three it works from are the
 * user's, and it holds them for the life of the rebase, not the life
 * of the process. The one dataset it creates is the result clone,
 * which the user names, and which carries the record.
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

/*
 * The record: the user properties a result carries, every one of
 * them set by the create itself so that the record exists from the
 * result's first instant and no kill can leave a result without one.
 * The tag and the manifest together are what says "a zfs_rebase
 * result": --abort refuses to touch a dataset that is missing either
 * of them, and a later --continue reads the rest of the record to
 * pick the rebase up in another process.
 *
 * There is no state property at birth. The state is written at the
 * gates the run passes -- applying1, conflicts, done -- so that what
 * a kill leaves is the last gate reached and nothing else.
 */
#define	ZR_PROP_BASE		"zfs_rebase:base"
#define	ZR_PROP_BASE_GUID	"zfs_rebase:base_guid"
#define	ZR_PROP_FROM		"zfs_rebase:from"
#define	ZR_PROP_FROM_GUID	"zfs_rebase:from_guid"
#define	ZR_PROP_ONTO		"zfs_rebase:onto"
#define	ZR_PROP_ONTO_GUID	"zfs_rebase:onto_guid"
#define	ZR_PROP_MADE		"zfs_rebase:made"
#define	ZR_PROP_MODE		"zfs_rebase:mode"
#define	ZR_PROP_FORM		"zfs_rebase:form"
#define	ZR_PROP_TAG		"zfs_rebase:tag"
#define	ZR_PROP_VERIFY		"zfs_rebase:verify"
#define	ZR_PROP_MANIFEST	"zfs_rebase:manifest"
#define	ZR_PROP_STATE		"zfs_rebase:state"

/*
 * The record as the run hands it to the create. Every field is
 * written as a string, guids as unsigned decimal, because a user
 * property has no other type: zfs_set_prop_nvlist (module/zfs/
 * zfs_ioctl.c) returns EINVAL for a user property that is not a
 * string. made names the inputs the tool snapshotted itself and is
 * "" while the tool takes none; mode is "strict" or "permissive";
 * form is "clone"; verify is "yes" or "no".
 */
struct zr_rebase_record {
	const char	*base;		/* pool/fs@snap */
	const char	*from;
	const char	*onto;
	uint64_t	base_guid;
	uint64_t	from_guid;
	uint64_t	onto_guid;
	const char	*made;
	const char	*mode;
	const char	*form;
	const char	*tag;		/* the hold tag, "zr-<12 hex>" */
	const char	*verify;
	const char	*manifest;	/* absolute path */
};

struct zr_zfs;

/* Open libzfs. Returns 0 with *out set to a handle the caller closes. */
int zr_zfs_open(struct zr_zfs **out, char *err, size_t errlen);

/* Finish with libzfs. Safe on NULL. Holds are untouched: they persist. */
void zr_zfs_close(struct zr_zfs *z);

/*
 * Hold snapshot under tag, with no cleanup descriptor, so that the
 * hold outlives this process and the rebase can be picked up by
 * another one. Every hold a rebase takes is filed under the tag in
 * its own record, and nothing but that rebase's done, --abort or
 * --continue releases it.
 */
int zr_zfs_hold(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen);

/*
 * Release tag from snapshot. A tag that is not there is not a
 * failure -- lzc_release puts it on the errlist and returns 0 --
 * which is what lets --abort be run twice.
 */
int zr_zfs_release(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen);

/*
 * Clone snapshot as clone with readonly=on, the given mountpoint and
 * the whole record, then mount it there. The record is set by the
 * create itself, so it exists from the clone's first instant and no
 * kill can leave a result dataset the tool cannot recognize.
 */
int zr_zfs_clone(struct zr_zfs *z, const char *snapshot, const char *clone,
    const char *mountpoint, const struct zr_rebase_record *rec, char *err,
    size_t errlen);

/*
 * Hold snapshot under tag against a cleanup descriptor of this
 * process's own, so that the kernel gives the hold back when the
 * process ends, however it ends. That is what --verify wants and a
 * rebase does not: a report holds its inputs still for as long as it
 * reads them and leaves nothing behind, while a rebase's holds are
 * the rebase and outlive every process it takes. The descriptor is
 * opened on the first such hold and belongs to the handle.
 */
int zr_zfs_hold_tmp(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen);

/*
 * The snapshot of this pool whose guid is guid, if there is one: 1
 * with its full name in buf, 0 when the pool holds no such snapshot,
 * -1 with err set. Every filesystem of the pool is walked, depth
 * first from the pool's own root dataset, and the first snapshot
 * that matches wins.
 *
 * A guid is what a snapshot is, where its name is only what it is
 * called: a rename or a promote moves the name and keeps the guid,
 * and a snapshot destroyed and taken again under the old name keeps
 * the name and gets a new guid. This is how a post-done verify finds
 * an input the record named, and how it tells that apart from a
 * different snapshot wearing the same name.
 */
int zr_zfs_find_guid(struct zr_zfs *z, const char *pool, uint64_t guid,
    char *buf, size_t buflen, char *err, size_t errlen);

/*
 * Mount dataset where its own mountpoint property says. For the
 * result clone after a reboot, or after anything else that unmounted
 * it; the caller looks at the mounted property first and calls this
 * only when the answer is no.
 */
int zr_zfs_mount(struct zr_zfs *z, const char *dataset, char *err,
    size_t errlen);

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
 * would print it: createtxg, the kernel's own ordering of the
 * snapshots in a pool, and guid, which names a snapshot across a
 * rename or a promote.
 */
int zr_zfs_get_int(struct zr_zfs *z, const char *dataset, const char *prop,
    uint64_t *out, char *err, size_t errlen);

/* Set one "module:name" user property. */
int zr_zfs_set_user(struct zr_zfs *z, const char *dataset, const char *prop,
    const char *value, char *err, size_t errlen);

/*
 * Read one local "module:name" user property: 1 with buf set when
 * the dataset itself carries it, 0 when it does not, -1 on a failure
 * with err set.
 *
 * Local, and nothing else, because a user property inherits down the
 * naming tree: zfs set zfs_rebase:tag=x on a pool gives every
 * dataset under it that tag, and a record read from an inherited
 * value would let --abort destroy a dataset no run ever made. Each
 * property's nvlist carries ZPROP_SOURCE beside ZPROP_VALUE, and the
 * source of a local value is the dataset's own name; get_source
 * (lib/libzfs/libzfs_dataset.c) and the user-property arm of
 * get_callback (cmd/zfs/zfs_main.c) both read it that way. A
 * received value, whose source is the string "$recvd", is treated
 * here as absent: it came in on a send stream rather than from a run
 * of this tool, and its snapshots and manifest path are another
 * machine's.
 */
int zr_zfs_get_user(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen);

#endif	/* ZR_ZFSOPS_H */
