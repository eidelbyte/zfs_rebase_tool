/*
 * walk: one tree read from the filesystem into the shared name
 * table. Every name under the root becomes a name id and a member of
 * its pool, and every pool carries, from the first name that reached
 * it, the attributes the content oracle will later compare.
 */

#ifndef	ZR_WALK_H
#define	ZR_WALK_H

#include <sys/types.h>
#if defined(__FreeBSD__)
#include <sys/acl.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "name.h"

/*
 * The file flags as the tool sees them. FreeBSD keeps the DOS
 * archive bit, UF_ARCHIVE ("file needs to be archived"), for the
 * filesystem's own bookkeeping: ZFS sets it on every new object
 * (zfs_mknode) and again on every mtime or ctime update
 * (zfs_tstamp_update_setup), and a backup tool clears it. That is
 * ctime-like state, not the file's content, and carrying it would
 * make the tool's own apply read as drift, so every flag word the
 * tool reads is masked of it and it is never written. A deliberate
 * chflags nouarch is the one thing this loses, and the next write
 * would undo that anyway.
 */
#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/stat.h>
#if defined(__FreeBSD__)
#define	ZR_FLAGS_NOISE	((uint32_t)UF_ARCHIVE)
#else
#define	ZR_FLAGS_NOISE	((uint32_t)0)
#endif
#define	ZR_ST_FLAGS(stp)	((uint32_t)(stp)->st_flags & ~ZR_FLAGS_NOISE)
#endif

/*
 * One ACL, in the form the platform gives it. FreeBSD is the target,
 * and there an ACL is already a list of integers -- libc's acl_t --
 * so the walk keeps that and nothing is spelled out and read back.
 * Everywhere else it is the text acl_to_text made, which is the only
 * form the stand-in's API offers. A NULL is an absent ACL on both,
 * and zr_acl_equal and zr_acl_free below are all the rest of the
 * tool asks of one.
 */
#if defined(__FreeBSD__)
typedef acl_t zr_acl_t;
#else
typedef char *zr_acl_t;
#endif

/* One extended attribute. The value is bytes, not a string. */
struct zr_xattr {
	char		*zx_name;	/* NUL-terminated */
	unsigned char	*zx_value;
	size_t		zx_len;
};

/*
 * One pool's attributes, filled at first sight and never revised: a
 * pool is one file, so the second and later names of it see the same
 * inode and the same attributes.
 */
struct zr_attr {
	mode_t		za_mode;	/* type bits included */
	uid_t		za_uid;
	gid_t		za_gid;
	uint32_t	za_flags;	/* st_flags where there is one */
	uint64_t	za_size;
	uint64_t	za_rdev;
	/*
	 * The two the pruning compares, straight from the lstat the
	 * walk already made: the generation number, which ZFS fills
	 * from the ZPL's z_gen (module/os/freebsd/zfs/zfs_vnops_os.c,
	 * zfs_getattr), and the change time, which is z_ctime. A
	 * platform with no st_gen leaves za_gen 0 and the pruning
	 * there compares 0 with 0, which is why it is the real mode
	 * on FreeBSD that prunes and nothing else.
	 */
	uint64_t	za_gen;
	struct timespec	za_ctime;
	char		*za_target;	/* symlink target, else NULL */
	struct zr_xattr	*za_xattrs;	/* sorted by name, bytewise */
	uint32_t	za_nxattrs;
	/*
	 * The ACL, and NULL where there is none to keep: no ACL at
	 * all, or -- on FreeBSD -- an NFSv4 ACL that
	 * acl_is_trivial_np says the mode already expresses in
	 * full, so that an ordinary file costs nothing. za_dacl is
	 * the POSIX.1e default ACL, the one a directory hands its
	 * new children; an NFSv4 ACL has no second list, its
	 * inheritance being written into its own entries.
	 */
	zr_acl_t	za_acl;
	zr_acl_t	za_dacl;
};

/*
 * One walked tree: its pools, the attributes of each pool indexed by
 * pool index, and the root kept open so that a later phase can reach
 * any name without retracing the path from the filesystem root.
 */
struct zr_walk {
	struct zr_tree	zw_tree;
	struct zr_attr	*zw_attrs;	/* indexed by pool index */
	uint32_t	zw_nattrs;
	int		zw_rootfd;	/* the root directory, kept open */
	uint64_t	zw_dev;		/* st_dev of the root */
};

/*
 * Walk root, interning every name into names and adding it to
 * out->zw_tree, which is sealed and verified before this returns.
 * The walk never follows a symbolic link and never crosses a mount
 * point, and it skips a ".zfs" at the root, which is ZFS's control
 * directory and not part of the tree. It descends on a stack of open
 * directories, so it holds one descriptor per level of depth, and
 * each of those directories is read whole and sorted by leaf name
 * before it is descended: the ids therefore follow the manifest's
 * own order, per directory and sorted, parents before children, on
 * every filesystem, whatever order readdir gave.
 * Returns 0, or -1 with a message naming the path and the errno text
 * in err when errlen is not 0. Either way out is left consistent and
 * must be handed to zr_walk_fini.
 */
int zr_walk(const char *root, struct zr_names *names, struct zr_walk *out,
    char *err, size_t errlen);

/* Free the attributes, close the root, and finalise the tree. */
void zr_walk_fini(struct zr_walk *w);

/*
 * Two ACLs of the platform's own kind: 1 if they are equal, 0 if
 * not. Both absent is equal, one absent is not. On FreeBSD this is
 * the binary comparison -- the brand, the length, and every entry's
 * fields in order, an NFSv4 ACL being an ordered list where the
 * order is part of the meaning. The arguments are not const because
 * libc's entry walk carries its cursor inside the ACL.
 */
int zr_acl_equal(zr_acl_t a, zr_acl_t b);

/* Release one ACL. An absent one -- a NULL -- is nothing to free. */
void zr_acl_free(zr_acl_t a);

/*
 * Open one name of the walked tree relative to the kept root, with
 * O_NOFOLLOW and O_CLOEXEC added to oflags. The root's own name is
 * answered with a duplicate of the kept descriptor, which carries
 * the flags it was opened with rather than oflags. Returns the
 * descriptor, which the caller closes, or -1 with errno set.
 */
int zr_walk_openat(const struct zr_walk *w, zr_name_t nm, int oflags);

#endif	/* ZR_WALK_H */
