/*
 * zfsops: the ZFS operations one run needs -- hold, snapshot, clone,
 * mount and unmount, rollback, property get and set, existence,
 * release and destroy -- behind one handle that owns the libzfs
 * handle. All of it is library calls into libzfs_core and libzfs;
 * the tool never execs zfs(8).
 *
 * A snapshot the user gives is the user's, and the tool holds it for
 * the life of the rebase rather than the life of the process. A
 * dataset the user gives is snapshotted by the tool, and that
 * snapshot lives exactly as long as the rebase: the record says
 * which of them the tool made. The one dataset the tool creates is
 * the result clone of the clone form, which the user names; in the
 * dataset form it creates none and the record lives on the onto
 * dataset itself.
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
#define	ZR_PROP_READONLY	"zfs_rebase:readonly"
#define	ZR_PROP_STATE		"zfs_rebase:state"

/*
 * The record as the run hands it to the create. Every field is
 * written as a string, guids as unsigned decimal, because a user
 * property has no other type: zfs_set_prop_nvlist (module/zfs/
 * zfs_ioctl.c) returns EINVAL for a user property that is not a
 * string. made names the inputs the tool snapshotted itself and is
 * "" when both were given as snapshots; mode is "strict" or
 * "permissive"; form is "clone" or "dataset"; verify is "yes" or
 * "no".
 *
 * readonly is the dataset form's own: the value the onto dataset's
 * readonly property had before the run took the dataset over, so
 * that handing it back restores it. The clone form leaves it NULL
 * and the property is not written at all -- a clone is created
 * read-only and stays that way, and there is nothing to put back.
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
	const char	*readonly;	/* "on", "off", or NULL */
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
 * Write the whole record on a dataset that already exists, which is
 * what the dataset form does: there is no create to carry the
 * properties, so each of them is set on onto itself. Every one is
 * set locally, which is what a set does, and the state is not among
 * them -- the state is written at the gates.
 */
int zr_zfs_write_record(struct zr_zfs *z, const char *dataset,
    const struct zr_rebase_record *rec, char *err, size_t errlen);

/*
 * Take one snapshot. Returns 0 when it was taken, 1 when a snapshot
 * of that name is already there, and -1 with err set otherwise, so
 * that a caller naming a snapshot itself can try the next name and a
 * caller carrying the user's name can refuse.
 */
int zr_zfs_snapshot(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen);

/*
 * Destroy one snapshot, with defer off: a snapshot that is held or
 * cloned is a failure here rather than a promise to destroy it
 * later, because the tool releases its own holds first and has
 * nothing else to wait for. A snapshot that is not there is not a
 * failure -- lzc_destroy_snaps ignores it -- which is what lets
 * --abort be run twice.
 */
int zr_zfs_destroy_snap(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen);

/*
 * Roll dataset back to snapshot, which must be its most recent one.
 * The dataset may be mounted: zfs_ioc_rollback (module/zfs/
 * zfs_ioctl.c) looks the filesystem up with getzfsvfs and suspends
 * and resumes it around the rollback itself, and zfs(8) unmounts
 * nothing for a rollback either (zfs_do_rollback, cmd/zfs/
 * zfs_main.c, calls zfs_rollback, which calls lzc_rollback_to and
 * no mount call at all). The mount stays where it is, which is what
 * the dataset form needs: its dataset is mounted privately at the
 * time.
 *
 * Nothing else is destroyed. zfs rollback -r destroys the snapshots
 * that came after the target first; this does not, so a rollback
 * that would need that fails with EEXIST and says so rather than
 * taking a snapshot of the user's away.
 */
int zr_zfs_rollback(struct zr_zfs *z, const char *dataset,
    const char *snapshot, char *err, size_t errlen);

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

/*
 * Mount dataset at path, whatever its mountpoint property says, and
 * without changing that property: zfs_mount_at (lib/libzfs/
 * libzfs_mount.c) takes the path as an argument, and only zfs_mount
 * reads the property to find one. The directory must exist or be
 * creatable and must be empty, as it is for any mount. This is how
 * the dataset form takes a dataset over: the mountpoint it will go
 * back to is untouched all along.
 */
int zr_zfs_mount_at(struct zr_zfs *z, const char *dataset, const char *path,
    char *err, size_t errlen);

/*
 * Unmount dataset from wherever it is mounted. Nothing is forced:
 * the flags are 0, so MNT_FORCE is never passed (do_unmount, lib/
 * libzfs/os/freebsd/libzfs_zmount.c, hands its flags straight to
 * unmount(2)), and a dataset somebody is using comes back EBUSY,
 * which is the refusal the dataset form is built on. A dataset that
 * is not mounted is nothing to do and not a failure.
 */
int zr_zfs_unmount(struct zr_zfs *z, const char *dataset, char *err,
    size_t errlen);

/*
 * Where dataset is mounted just now, which is not always where its
 * mountpoint property says: 1 with the path in buf, 0 when it is not
 * mounted, -1 with err set. zfs_is_mounted (lib/libzfs/
 * libzfs_mount.c) answers from the mount table and not from the
 * property.
 */
int zr_zfs_mounted_at(struct zr_zfs *z, const char *dataset, char *buf,
    size_t buflen, char *err, size_t errlen);

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
 * One property of dataset as the string zfs(8) would print. What is
 * really a string is what this is for, and after sprint 5 that is
 * two properties: origin and mountpoint, which are names. A property
 * that has no value and no default reads as "-", as zfs(8) prints
 * it; origin on a dataset that is not a clone is the one this tool
 * asks for. Everything else the tool reads is a number and goes
 * through zr_zfs_get_int, so that no verdict rests on a word.
 */
int zr_zfs_get(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen);

/*
 * One numeric property as the number it is rather than as zfs(8)
 * would print it: createtxg, the kernel's own ordering of the
 * snapshots in a pool; guid, which names a snapshot across a rename
 * or a promote; and the index and boolean properties below, which
 * zfs_prop_get_int answers from the dataset's own stats
 * (lib/libzfs/libzfs_dataset.c).
 */
int zr_zfs_get_int(struct zr_zfs *z, const char *dataset, const char *prop,
    uint64_t *out, char *err, size_t errlen);

/*
 * The values the driver compares those numbers with. They are
 * spelled out here because run.c is portable code with no ZFS header
 * in its include path -- only zfsops.c is compiled with those -- and
 * because neither name the audit expected is usable: zfs_case_t is
 * in sys/zfs_ioctl.h, which pulls in sys/dmu.h and the kernel
 * context and will not compile in userspace, and there is no
 * ZFS_NORMALIZE_NONE anywhere in the tree. What fixes each value:
 *
 *	casesensitivity	 the first member of zfs_case_t
 *			 (sys/zfs_ioctl.h), which zfs_prop_init
 *			 registers as the property's default
 *			 (module/zcommon/zfs_prop.c)
 *	normalization	 the "none" entry of normalize_table, the
 *			 only one that is 0: the rest are the
 *			 U8_TEXTPREP_ forms (module/zcommon/zfs_prop.c)
 *	mounted		 not an index but a boolean the handle
 *			 answers out of its own mount options --
 *			 get_numeric_property returns
 *			 (zhp->zfs_mntopts != NULL) for it
 *			 (lib/libzfs/libzfs_dataset.c) -- so anything
 *			 that is not 0 is mounted
 *
 * zr_zfs_open holds the first two against ZFS's own property tables
 * before a run does anything, so a value that drifted is a refusal
 * to run and never a wrong verdict about a dataset.
 */
#define	ZR_CASE_SENSITIVE	0
#define	ZR_NORMALIZE_NONE	0
#define	ZR_NOT_MOUNTED		0

/* Set one "module:name" user property. */
int zr_zfs_set_user(struct zr_zfs *z, const char *dataset, const char *prop,
    const char *value, char *err, size_t errlen);

/*
 * Take one "module:name" user property off a dataset. Inheriting a
 * user property is how it is removed: zfs_prop_inherit (lib/libzfs/
 * libzfs_dataset.c) takes the ZPROP_USERPROP arm straight to
 * ZFS_IOC_INHERIT_PROP, and dsl_prop_set_sync_impl (module/zfs/
 * dsl_prop.c) removes the local entry for ZPROP_SRC_INHERITED. It is
 * what zfs inherit does for a user property, and a property that is
 * not there is not a failure.
 */
int zr_zfs_clear_user(struct zr_zfs *z, const char *dataset, const char *prop,
    char *err, size_t errlen);

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
