/*
 * walk: one tree read from the filesystem into the shared name
 * table. Every name under the root becomes a name id and a member of
 * its pool, and every pool carries, from the first name that reached
 * it, the attributes the content oracle will later compare.
 */

#ifndef	ZR_WALK_H
#define	ZR_WALK_H

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#include "name.h"

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
	char		*za_target;	/* symlink target, else NULL */
	struct zr_xattr	*za_xattrs;	/* sorted by name, bytewise */
	uint32_t	za_nxattrs;
	char		*za_acl;	/* platform ACL as text, else NULL */
	char		*za_dacl;	/* POSIX.1e default ACL, else NULL */
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
 * directories, so it holds one descriptor per level of depth.
 * Returns 0, or -1 with a message naming the path and the errno text
 * in err when errlen is not 0. Either way out is left consistent and
 * must be handed to zr_walk_fini.
 */
int zr_walk(const char *root, struct zr_names *names, struct zr_walk *out,
    char *err, size_t errlen);

/* Free the attributes, close the root, and finalise the tree. */
void zr_walk_fini(struct zr_walk *w);

/*
 * Open one name of the walked tree relative to the kept root, with
 * O_NOFOLLOW and O_CLOEXEC added to oflags. The root's own name is
 * answered with a duplicate of the kept descriptor, which carries
 * the flags it was opened with rather than oflags. Returns the
 * descriptor, which the caller closes, or -1 with errno set.
 */
int zr_walk_openat(const struct zr_walk *w, zr_name_t nm, int oflags);

#endif	/* ZR_WALK_H */
