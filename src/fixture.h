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
