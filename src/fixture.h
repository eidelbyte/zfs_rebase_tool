/*
 * fixture: one text file describing three trees -- base, from and
 * onto -- and the manifest a rebase of them must emit. Loading parses
 * it; two builders make it real, one writing plain directories, files,
 * hard links and symlinks under a directory, the other filling a
 * sealed struct zr_tree from the spec alone. The format and its rules
 * are tests/fixtures/FORMAT.md.
 */

#ifndef	ZR_FIXTURE_H
#define	ZR_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#include "name.h"

struct zr_fixture;

enum zr_fixture_tree {
	ZR_FX_BASE = 0,
	ZR_FX_FROM = 1,
	ZR_FX_ONTO = 2
};

/*
 * Parse the fixture at path. On success returns 0 with *out set to a
 * fixture the caller frees with zr_fixture_free. On any violation of
 * the format returns -1, leaves *out alone, and writes a message
 * naming the line number into err when errlen is not 0.
 */
int zr_fixture_load(const char *path, struct zr_fixture **out, char *err,
    size_t errlen);

void zr_fixture_free(struct zr_fixture *fx);

/*
 * The platform a fixture's "platform" line demands, or NULL when it
 * has none and builds anywhere. Owned by the fixture.
 */
const char *zr_fixture_platform(const struct zr_fixture *fx);

/*
 * Create one tree under rootdir, which must already exist and be
 * empty: mkdir(2) for a directory, the token and a newline for a
 * file, link(2) for a link, symlink(2) for a symlink, then the
 * entry's attributes, and the file flags of every object last of
 * all. Returns 0, or -1 at the first failure with errno set by the
 * call that failed. A fixture whose platform line names another
 * platform than this one is refused with ENOTSUP: the _err form says
 * so in words, naming the line, and is what a caller with a message
 * to print should use. The two are otherwise the same call.
 */
int zr_fixture_build(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *rootdir);
int zr_fixture_build_err(const struct zr_fixture *fx,
    enum zr_fixture_tree which, const char *rootdir, char *err, size_t errlen);

/*
 * What one edit did: one decision per name, over the union of the
 * names the directory held and the names the tree lists, the root
 * apart. The six are exclusive and add up to that union, so the sum
 * is the number of names a caller can count for itself.
 *
 *	ze_removed	the directory had the name, the tree has not:
 *			unlinked, or the directory removed once the
 *			names under it had gone
 *	ze_created	the tree has the name, the directory had not,
 *			and the pool it belongs to is new here: the
 *			first name of it makes the object, the rest
 *			link to that name
 *	ze_relinked	the names on one object had to change: a name
 *			linked onto an object that stays, a name
 *			taken off one, or a name that left the pool it
 *			was in and was made afresh
 *	ze_rewritten	the name and its pooling were right and the
 *			object was not: a file's bytes written through
 *			it, which keeps the inode; a symlink or an
 *			object of the wrong type removed and made
 *			again, which does not
 *	ze_attrs	only the pool's attributes differed: mode,
 *			owner, group, extended attributes, ACL or
 *			file flags
 *	ze_untouched	nothing differed, and nothing was done to the
 *			name or to the object under it
 *
 * Untouched is a property of a whole pool: if one name of a pool is
 * untouched then that pool's name set and its object both matched,
 * so every name of it is untouched too.
 */
struct zr_fixture_edit_stats {
	uint64_t	ze_removed;
	uint64_t	ze_created;
	uint64_t	ze_rewritten;
	uint64_t	ze_relinked;
	uint64_t	ze_attrs;
	uint64_t	ze_untouched;
};

/*
 * Make the tree under dir, which already holds one built tree --
 * typically this fixture's base -- equal to the fixture's which
 * tree, by the smallest set of edits: every object that is already
 * what the tree asks for is not touched at all, so its inode, its
 * generation number and its ctime survive, which is the signal a
 * replay of two sides prunes by. Directories under it are the one
 * exception the caller must know: an untouched directory whose
 * children changed still has its own ctime moved by the kernel, as
 * it would by any real edit.
 *
 * The order of operations is the builder's: the file flags come off
 * whatever must be edited beneath them before anything else, the
 * removals run children before parents, the creations parents
 * before children, and the flags go back on last of all. Nothing
 * removes the tree and writes it again.
 *
 * Returns 0 with *st filled when st is not NULL, or -1 with the
 * path, the step and the reason in err when errlen is not 0; the
 * statistics mean nothing then. A fixture whose platform line names
 * another platform than this one is refused with ENOTSUP, as it is
 * for a build.
 */
int zr_fixture_edit(const struct zr_fixture *fx, enum zr_fixture_tree which,
    const char *dir, struct zr_fixture_edit_stats *st, char *err,
    size_t errlen);

/*
 * Fill out with one tree straight from the spec, touching no
 * filesystem: every entry is a pool member, a link joins its target's
 * pool, inode numbers are synthetic and distinct per pool, nlink is
 * the pool's name count and zp_content is the handle described in
 * tests/fixtures/FORMAT.md -- one number standing for everything the
 * content oracle compares, so that two pools share it exactly when a
 * walk of the built trees would call them equal. The tree is sealed
 * on return. out is initialised here and belongs to the caller, who
 * frees it with zr_tree_fini. Names are interned into ns. Returns 0,
 * or -1 with out left finalised.
 */
int zr_fixture_to_tree(const struct zr_fixture *fx, enum zr_fixture_tree which,
    struct zr_names *ns, struct zr_tree *out);

/*
 * The expect block: the manifest text, verbatim, or NULL if the
 * fixture has no expect block. Owned by the fixture.
 */
const char *zr_fixture_expect(const struct zr_fixture *fx);

#endif	/* ZR_FIXTURE_H */
