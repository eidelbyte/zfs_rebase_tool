/*
 * yellow: the content oracle. Three walked trees go in and one
 * opaque content handle per pool comes out, written into every
 * pool's zp_content, where equal handles mean equal content and
 * nothing else does. What "equal content" is here is what
 * v4-yellow-content.md leaves to this layer in its sections 2 and
 * 7: the attributes always, a directory by those alone since its
 * entries are pools of their own, a file by its bytes, and the
 * times never.
 */

#ifndef	ZR_YELLOW_H
#define	ZR_YELLOW_H

#include "walk.h"

struct zr_oracle;

/*
 * An oracle over three walked trees, which must be sealed, share one
 * name table and outlive it. Returns 0 with *out set to an oracle
 * the caller frees with zr_oracle_fini, or -1.
 *
 * The three trees are positional: 0, 1 and 2, in the order given
 * here. They are called base, from and onto because that is what a
 * decision has, but nothing below requires that meaning -- verify
 * builds an oracle over the onto snapshot, the from snapshot and the
 * result tree and asks it the same questions. A message names the
 * tree by the word for its position, so a caller that means
 * something else by a position reads the word as that position.
 */
int zr_oracle_init(struct zr_oracle **out, struct zr_walk *base,
    struct zr_walk *from, struct zr_walk *onto);

/*
 * The caller knows -- without reading either -- that pool of tree, 1
 * for from and 2 for onto, holds exactly what base_pool of base
 * holds. The two are put in one class on that word, and neither is
 * ever read. Returns 0, or -1 on an argument the oracle cannot
 * place. zr_oracle_prune below is the one caller that knows it from
 * the walk itself; a caller with another source of the same word is
 * welcome to this.
 */
int zr_oracle_unchanged(struct zr_oracle *o, int tree, zr_pool_t pool,
    zr_pool_t base_pool);

/*
 * The unchanged set, from the walks alone. side is 1 for from and 2
 * for onto. Every pool of that side whose first name base holds too
 * is compared with the base pool holding it, by what the two walks
 * already have in memory, and told to zr_oracle_unchanged when
 * every one of these agrees:
 *
 *	the object number, the type, the link count, the number of
 *	names, the generation number, the change time to the
 *	nanosecond, every one of those names in that same base pool,
 *	the extended attributes, and both ACLs
 *
 * Nothing is read from disk and nothing is asked of ZFS: the
 * attributes are the ones each walk captured for itself.
 *
 * The name condition is this layer's own, and catches a rename or a
 * link change made inside the tick a ctime is stored at (gethrestime
 * is getnanotime on FreeBSD: include/os/freebsd/spl/sys/time.h).
 * The extended attributes are compared rather than trusted to the
 * ctime because one way of setting one leaves the ctime standing,
 * which is the paragraph after next; the ACLs, whose every change
 * does move the ctime, are compared beside them because it costs
 * nothing and the word this returns is then never looser for either
 * of them than a read's would be. What is left to the object
 * number, the generation number and the ctime is the bytes of the
 * object alone, and these are the ZFS facts that trust stands on,
 * all of them in the OpenZFS tree FreeBSD carries:
 *
 *	a write moves the ctime -- module/zfs/zfs_vnops.c, zfs_write,
 *	and module/os/freebsd/zfs/zfs_vnops_os.c, zfs_putpages, both
 *	through zfs_tstamp_update_setup with CONTENT_MODIFIED;
 *
 *	a truncate, a chmod, a chown, a chflags or any other of the
 *	attributes the znode keeps moves it -- module/os/freebsd/zfs/
 *	zfs_vnops_os.c, zfs_setattr, which writes SA_ZPL_CTIME for
 *	every mask it accepts;
 *
 *	setting an ACL moves it -- module/os/freebsd/zfs/zfs_acl.c,
 *	zfs_setacl through zfs_aclset_common;
 *
 *	setting an extended attribute in the SA storage moves it --
 *	module/zfs/zfs_sa.c, zfs_sa_set_xattr, which puts
 *	SA_ZPL_CTIME in the same bulk update as SA_ZPL_DXATTR;
 *
 *	an object number is only ever reused by an object of a later
 *	generation -- the generation is the txg the object was made
 *	in (module/os/freebsd/zfs/zfs_znode_os.c, zfs_mknode) and the
 *	walk reads it as st_gen, which zfs_getattr fills from z_gen.
 *
 * And the fact that makes the attributes a comparison and not a
 * trust: an extended attribute in the directory storage does not
 * move the file's ctime at all. module/os/freebsd/zfs/zfs_vnops_os.c,
 * zfs_setextattr_dir looks the file's hidden directory up and does
 * VOP_SETATTR and VOP_WRITE on a child of it, so the file's own
 * znode is never written; and module/os/freebsd/zfs/zfs_dir.c,
 * zfs_make_xattrdir, which makes that directory the first time,
 * writes SA_ZPL_XATTR on the file and no timestamp with it. Three
 * things reach that storage, so it is not a corner: the property
 * xattr=dir, which is inherited (module/zcommon/zfs_prop.c
 * registers xattr PROP_INHERIT with ZFS_XATTR_SA as the default --
 * and note that xattr=on is the SA storage in this tree, its table
 * mapping "on" to ZFS_XATTR_SA); an object made before the dataset
 * kept its attributes in the SA, where z_is_sa is false, since
 * zfs_setextattr_impl asks for z_use_sa, z_is_sa and z_xattr_sa
 * together; and that same function's fallback, which tries the SA
 * and takes the directory on any failure of it, an attribute over
 * DXATTR_MAX_ENTRY_SIZE included. Until this comparison was here, a
 * from side whose only change was such an attribute was declared
 * unchanged and the change was dropped in silence.
 *
 * lib/libzfs/libzfs_diff.c is where the rule came from: it prints
 * nothing for an object whose gen and ctime both match, and the
 * ioctl it asks reads the same ZPL attributes the walk's lstat did.
 * Read honestly, that test is not this one. It runs only over the
 * objects the DMU has already reported as rewritten in the txg
 * range, so what it does is downgrade a change it knows about,
 * where here the same fields are the only evidence that there was
 * no change at all. Its own line above the test reads "No apparent
 * changes.  Could we assert !this?".
 *
 * The object numbers only mean the same thing on both sides when the
 * side really descends from base, so this belongs to the real mode
 * with a derived base and to nothing else. *marked comes back with
 * the number of pools put in a class. Returns 0, or -1 on an
 * argument the oracle cannot place.
 */
int zr_oracle_prune(struct zr_oracle *o, int side, uint32_t *marked);

/*
 * One pair, asked for directly: pool pa of tree ta against pool pb
 * of tree tb, each tree named by its position in zr_oracle_init.
 * Returns 1 equal, 0 different, -1 with a message in err when errlen
 * is not 0.
 *
 * This is the same comparison zr_oracle_assign makes and it goes
 * through the same memory: a pair already in one class answers 1
 * without reading, a pair already compared and found different
 * answers 0 without reading, and a pair found equal here is put in
 * one class, so equality still travels. A read that fails is a
 * failure, never a verdict of "different".
 */
int zr_oracle_equal(struct zr_oracle *o, int ta, zr_pool_t pa, int tb,
    zr_pool_t pb, char *err, size_t errlen);

/*
 * Compare every green-adjacent pair -- two pools of different trees
 * holding one name -- that is not already in one class, put the
 * equal ones in one class, and write one handle per class into the
 * zp_content of every pool of all three trees, dense from 0. A pair
 * is read at most once however many names it shares. Returns 0, or
 * -1 with a message naming the tree and the path in err when errlen
 * is not 0: a read that fails is a failure, never a verdict of
 * "different".
 */
int zr_oracle_assign(struct zr_oracle *o, char *err, size_t errlen);

/* Every byte read from either side, for the tests and the report. */
uint64_t zr_oracle_bytes_read(const struct zr_oracle *o);

void zr_oracle_fini(struct zr_oracle *o);

#endif	/* ZR_YELLOW_H */
