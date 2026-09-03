/*
 * The apply tests. First the whole pipeline over the probe fixture:
 * its three trees built as directories, walked, given content
 * handles, decided in strict mode, emitted, parsed back and applied
 * to the onto directory, which is then walked again and held against
 * the decision -- every surviving name there, every removed name
 * gone, one inode per result pool, and each pool's bytes the bytes
 * the manifest named. A fourth copy of onto is built and left alone,
 * so that a name the rebase did not touch has something to be
 * compared with.
 *
 * Then the shapes a fixture cannot describe, each a small pair of
 * trees made by hand and a manifest written as text: the cp of a
 * file, a symlink, a directory and a fifo, an ln over a name that
 * belongs to another file, a write seen through a second hard link,
 * a three-level rm whose rmdir waits for the close, a cp landing on
 * the name a directory rm just freed, the mtime, a file flag, and
 * the four refusals -- an anchor that is not there, a directory in
 * the way of a link, a directory rm with a child left, a path that
 * tries to climb out of the root, and a write onto a symlink that
 * points out of the tree.
 *
 * The family is ZA of tests/MATRIX.md. Covered here: ZA1, ZA2, ZA3,
 * ZA6, ZA8, ZA9, ZA10, ZA11, ZA12, ZA13, ZA14, ZA15, ZA16, ZA17,
 * ZA19, ZA20, ZA22, ZA24, ZA25, ZA26, ZA27, ZA28, ZA30. ZA4 and ZA5
 * need mknod, which needs root; ZA7 is a socket, which has no
 * portable create; ZA18 cannot see the chown at all, since an apply
 * run by the tree's own owner skips it; ZA21 wants an immutable
 * file and ZA23 a forced re-stat mismatch, both of which need root;
 * ZA29 is the ACL cell already deferred in the matrix.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/xattr.h>
#elif defined(__FreeBSD__)
#include <sys/extattr.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

#include "apply.h"
#include "decide.h"
#include "fixture.h"
#include "manifest.h"
#include "name.h"
#include "walk.h"
#include "yellow.h"

#define	PATHMAX		1024
#define	TEXTMAX		4096
#define	SCAN_MIN	16

#define	XNAME		"user.zra"
#define	XVAL		"v\000w"
#define	XLEN		3

#if defined(__FreeBSD__)
#define	TESTFLAG	UF_ARCHIVE
#elif defined(__APPLE__)
#define	TESTFLAG	UF_NODUMP
#endif

/*
 * The two times, spelled as the platform spells them: POSIX 2008 on
 * FreeBSD and Linux, macOS's own names on macOS, which aliases the
 * POSIX ones only at a feature level -std=c99 does not ask for.
 * src/apply.c chooses the same pair in its platform section.
 */
#ifdef __APPLE__
#define	TS_ATIME(s)	((s)->st_atimespec)
#define	TS_MTIME(s)	((s)->st_mtimespec)
#else
#define	TS_ATIME(s)	((s)->st_atim)
#define	TS_MTIME(s)	((s)->st_mtim)
#endif

static int checks;

#define	CHECK(x)							\
	do {								\
		checks++;						\
		if (!(x)) {						\
			printf("%s:%d: check failed: %s\n", __FILE__,	\
			    __LINE__, #x);				\
			exit(1);					\
		}							\
	} while (0)

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

/* Every name a scan found under one root, parents before children. */
struct scan {
	char	**s_path;
	int	*s_isdir;
	int	s_n;
	int	s_cap;
};

static void
scan_push(struct scan *s, const char *rel, int isdir)
{
	char **np;
	int *ni, cap;

	if (s->s_n == s->s_cap) {
		cap = s->s_cap == 0 ? SCAN_MIN : s->s_cap * 2;
		np = realloc(s->s_path, (size_t)cap * sizeof (char *));
		CHECK(np != NULL);
		s->s_path = np;
		ni = realloc(s->s_isdir, (size_t)cap * sizeof (int));
		CHECK(ni != NULL);
		s->s_isdir = ni;
		s->s_cap = cap;
	}
	s->s_path[s->s_n] = malloc(strlen(rel) + 1);
	CHECK(s->s_path[s->s_n] != NULL);
	(void) strcpy(s->s_path[s->s_n], rel);
	s->s_isdir[s->s_n] = isdir;
	s->s_n++;
}

static void
scan_dir(struct scan *s, const char *root, const char *rel)
{
	char full[PATHMAX], child[PATHMAX];
	struct dirent *de;
	struct stat st;
	DIR *d;

	join(full, sizeof (full), root, rel);
	d = opendir(full);
	CHECK(d != NULL);
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		join(child, sizeof (child), rel, "/");
		join(child, sizeof (child), child, de->d_name);
		join(full, sizeof (full), root, child);
		CHECK(lstat(full, &st) == 0);
		scan_push(s, child, S_ISDIR(st.st_mode) ? 1 : 0);
	}
	CHECK(closedir(d) == 0);
}

/* Breadth first and never recursing, as check_walk.c does it. */
static void
scan_tree(struct scan *s, const char *root)
{
	int i;

	memset(s, 0, sizeof (*s));
	scan_dir(s, root, "");
	for (i = 0; i < s->s_n; i++) {
		if (s->s_isdir[i])
			scan_dir(s, root, s->s_path[i]);
	}
}

/* Children before parents, which is the scan order reversed. */
static void
rmtree(const char *root)
{
	char full[PATHMAX];
	struct scan s;
	int i;

	scan_tree(&s, root);
	for (i = s.s_n - 1; i >= 0; i--) {
		join(full, sizeof (full), root, s.s_path[i]);
		if (s.s_isdir[i])
			CHECK(rmdir(full) == 0);
		else
			CHECK(unlink(full) == 0);
		free(s.s_path[i]);
	}
	free(s.s_path);
	free(s.s_isdir);
	CHECK(rmdir(root) == 0);
}

static int
setx(const char *path, const char *name, const void *val, size_t len)
{
#if defined(__APPLE__)
	return (setxattr(path, name, val, len, 0, XATTR_NOFOLLOW));
#elif defined(__FreeBSD__)
	ssize_t n;

	n = extattr_set_link(path, EXTATTR_NAMESPACE_USER, name + 5, val, len);
	return (n == (ssize_t)len ? 0 : -1);
#elif defined(__linux__)
	return (lsetxattr(path, name, val, len, 0));
#else
	(void) path;
	(void) name;
	(void) val;
	(void) len;
	return (-1);
#endif
}

static ssize_t
getx(const char *path, const char *name, void *val, size_t len)
{
#if defined(__APPLE__)
	return (getxattr(path, name, val, len, 0, XATTR_NOFOLLOW));
#elif defined(__FreeBSD__)
	return (extattr_get_link(path, EXTATTR_NAMESPACE_USER, name + 5, val,
	    len));
#elif defined(__linux__)
	return (lgetxattr(path, name, val, len));
#else
	(void) path;
	(void) name;
	(void) val;
	(void) len;
	return (-1);
#endif
}

static size_t
slurp(const char *path, char *buf, size_t cap)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
		printf("  open: %s: %s\n", path, strerror(errno));
	CHECK(fd >= 0);
	n = read(fd, buf, cap);
	CHECK(close(fd) == 0);
	CHECK(n >= 0);
	return ((size_t)n);
}

static void
same_bytes(const char *a, const char *b)
{
	char ba[TEXTMAX], bb[TEXTMAX];
	size_t na, nb;

	na = slurp(a, ba, sizeof (ba));
	nb = slurp(b, bb, sizeof (bb));
	if (na != nb || memcmp(ba, bb, na) != 0)
		printf("  bytes differ: %s and %s\n", a, b);
	CHECK(na == nb);
	CHECK(memcmp(ba, bb, na) == 0);
}

static void
mkfile(const char *root, const char *rel, const char *text, mode_t mode)
{
	char full[PATHMAX];
	size_t len;
	int fd;

	join(full, sizeof (full), root, rel);
	fd = open(full, O_CREAT | O_EXCL | O_WRONLY, mode);
	CHECK(fd >= 0);
	len = strlen(text);
	CHECK(write(fd, text, len) == (ssize_t)len);
	CHECK(close(fd) == 0);
	CHECK(chmod(full, mode) == 0);
}

static void
mkdirp(const char *root, const char *rel, mode_t mode)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	CHECK(mkdir(full, mode) == 0);
	CHECK(chmod(full, mode) == 0);
}

static void
lstat_at(const char *root, const char *rel, struct stat *st)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	if (lstat(full, st) != 0)
		printf("  lstat: %s: %s\n", full, strerror(errno));
	CHECK(lstat(full, st) == 0);
}

static int
absent(const char *root, const char *rel)
{
	char full[PATHMAX];
	struct stat st;

	join(full, sizeof (full), root, rel);
	return (lstat(full, &st) != 0 && errno == ENOENT);
}

/*
 * The manifest document around one tree section body. The header is
 * the least a parse will take and the count must be right, since the
 * parse checks it against the lines.
 */
static void
parse_body(struct zr_parsed *p, const char *body, int nactions)
{
	char text[TEXTMAX], err[256];
	FILE *f;
	int n;

	n = snprintf(text, sizeof (text),
	    "#rebase-manifest 4\n"
	    "#base b\n"
	    "#from f\n"
	    "#onto o\n"
	    "#mode strict\n"
	    "#actions %d\n"
	    "#conflicts 0\n"
	    "/\n"
	    "%s"
	    "..\n", nactions, body);
	CHECK(n > 0 && (size_t)n < sizeof (text));
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fputs(text, f) != EOF);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, p, err, sizeof (err)) != 0)
		printf("  parse: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
}

/* One targeted shape: a from tree, an onto tree and the walk between. */
struct shape {
	char		sh_root[PATHMAX];
	char		sh_from[PATHMAX];
	char		sh_onto[PATHMAX];
	struct zr_names	*sh_ns;
	struct zr_walk	sh_wf;
	int		sh_walked;
};

static void
shape_init(struct shape *sh)
{
	char tmpl[] = "/tmp/zrapplys.XXXXXX";

	memset(sh, 0, sizeof (*sh));
	CHECK(mkdtemp(tmpl) != NULL);
	join(sh->sh_root, sizeof (sh->sh_root), tmpl, "");
	join(sh->sh_from, sizeof (sh->sh_from), tmpl, "/from");
	join(sh->sh_onto, sizeof (sh->sh_onto), tmpl, "/onto");
	CHECK(mkdir(sh->sh_from, 0755) == 0);
	CHECK(mkdir(sh->sh_onto, 0755) == 0);
	sh->sh_ns = zr_names_create();
	CHECK(sh->sh_ns != NULL);
}

/* Walk the from tree, which every cp and write reads through. */
static void
shape_walk(struct shape *sh)
{
	char err[256];

	err[0] = '\0';
	if (zr_walk(sh->sh_from, sh->sh_ns, &sh->sh_wf, err,
	    sizeof (err)) != 0)
		printf("  walk from: %s\n", err);
	CHECK(err[0] == '\0');
	sh->sh_walked = 1;
}

static int
shape_apply(struct shape *sh, const char *body, int nactions,
    struct zr_apply_stats *st, char *err, size_t errlen)
{
	struct zr_parsed p;
	int rc;

	if (sh->sh_walked == 0)
		shape_walk(sh);
	parse_body(&p, body, nactions);
	rc = zr_apply(&p, sh->sh_onto, &sh->sh_wf, st, err, errlen);
	zr_parsed_fini(&p);
	return (rc);
}

/* The apply must succeed, and its message must have stayed empty. */
static void
shape_ok(struct shape *sh, const char *body, int nactions,
    struct zr_apply_stats *st)
{
	char err[512];

	err[0] = '\0';
	if (shape_apply(sh, body, nactions, st, err, sizeof (err)) != 0)
		printf("  apply: %s\n", err);
	CHECK(err[0] == '\0');
}

/* The apply must refuse, and say so with the word in it. */
static void
shape_refused(struct shape *sh, const char *body, int nactions,
    const char *word)
{
	struct zr_apply_stats st;
	char err[512];

	err[0] = '\0';
	CHECK(shape_apply(sh, body, nactions, &st, err, sizeof (err)) == -1);
	if (strstr(err, word) == NULL)
		printf("  message lacks \"%s\": %s\n", word, err);
	CHECK(strstr(err, word) != NULL);
}

static void
shape_fini(struct shape *sh)
{
	if (sh->sh_walked != 0)
		zr_walk_fini(&sh->sh_wf);
	zr_names_destroy(sh->sh_ns);
	rmtree(sh->sh_root);
}

/* Give one path the two times a copy is later checked against. */
static void
settimes(const char *path)
{
	struct timespec ts[2];

	ts[0].tv_sec = 1000000000;
	ts[0].tv_nsec = 0;
	ts[1].tv_sec = 1234567890;
	ts[1].tv_nsec = 0;
	CHECK(utimensat(AT_FDCWD, path, ts, AT_SYMLINK_NOFOLLOW) == 0);
}

static void
checktimes(const struct stat *s)
{
	CHECK(TS_MTIME(s).tv_sec == 1234567890);
	CHECK(TS_ATIME(s).tv_sec == 1000000000);
}

/*
 * ZA1, ZA19, ZA28: a file copied whole -- its bytes, its mode, its
 * one extended attribute, and the times, which are written after the
 * attribute and so are the ones that survive.
 */
static void
check_cp_file(void)
{
	struct zr_apply_stats st;
	char val[16], full[PATHMAX], src[PATHMAX];
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	mkfile(sh.sh_from, "/f", "hello apply", 0640);
	join(full, sizeof (full), sh.sh_from, "/f");
	CHECK(setx(full, XNAME, XVAL, XLEN) == 0);
	settimes(full);
	shape_ok(&sh, "    f cp /f\n", 1, &st);
	CHECK(st.zs_cp == 1);
	CHECK(st.zs_bytes == strlen("hello apply"));
	lstat_at(sh.sh_onto, "/f", &s);
	CHECK(S_ISREG(s.st_mode));
	CHECK((s.st_mode & 07777) == 0640);
	checktimes(&s);
	join(full, sizeof (full), sh.sh_onto, "/f");
	join(src, sizeof (src), sh.sh_from, "/f");
	same_bytes(full, src);
	CHECK(getx(full, XNAME, val, sizeof (val)) == XLEN);
	CHECK(memcmp(val, XVAL, XLEN) == 0);
	shape_fini(&sh);
}

/* ZA3: a symlink copied as a symlink, its target and not its file. */
static void
check_cp_symlink(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], buf[PATHMAX];
	struct shape sh;
	struct stat s;
	ssize_t n;

	shape_init(&sh);
	join(full, sizeof (full), sh.sh_from, "/s");
	CHECK(symlink("../elsewhere/target", full) == 0);
	shape_ok(&sh, "    s cp /s\n", 1, &st);
	CHECK(st.zs_cp == 1);
	lstat_at(sh.sh_onto, "/s", &s);
	CHECK(S_ISLNK(s.st_mode));
	join(full, sizeof (full), sh.sh_onto, "/s");
	n = readlink(full, buf, sizeof (buf));
	CHECK(n == (ssize_t)strlen("../elsewhere/target"));
	CHECK(memcmp(buf, "../elsewhere/target", (size_t)n) == 0);
	shape_fini(&sh);
}

/* ZA2: a directory copied, created empty and with its own mode. */
static void
check_cp_dir(void)
{
	struct zr_apply_stats st;
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	mkdirp(sh.sh_from, "/d", 0750);
	shape_ok(&sh, "    d/ cp /d\n    ..\n", 1, &st);
	CHECK(st.zs_cp == 1);
	lstat_at(sh.sh_onto, "/d", &s);
	CHECK(S_ISDIR(s.st_mode));
	CHECK((s.st_mode & 07777) == 0750);
	shape_fini(&sh);
}

/* ZA6: a fifo copied as a fifo. */
static void
check_cp_fifo(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX];
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	join(full, sizeof (full), sh.sh_from, "/p");
	CHECK(mkfifo(full, 0640) == 0);
	CHECK(chmod(full, 0640) == 0);
	shape_ok(&sh, "    p cp /p\n", 1, &st);
	CHECK(st.zs_cp == 1);
	lstat_at(sh.sh_onto, "/p", &s);
	CHECK(S_ISFIFO(s.st_mode));
	CHECK((s.st_mode & 07777) == 0640);
	shape_fini(&sh);
}

/*
 * ZA11, ZA12: an ln over a name that at this moment belongs to
 * another file. The old file loses its only name; the new one is the
 * anchor's own object.
 */
static void
check_ln_replace(void)
{
	struct zr_apply_stats st;
	struct shape sh;
	struct stat sa, sx;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/a", "anchor bytes", 0644);
	mkfile(sh.sh_onto, "/x", "doomed bytes", 0644);
	lstat_at(sh.sh_onto, "/x", &sx);
	shape_ok(&sh, "    x ln /a\n", 1, &st);
	CHECK(st.zs_ln == 1);
	lstat_at(sh.sh_onto, "/a", &sa);
	CHECK(sa.st_nlink == 2);
	lstat_at(sh.sh_onto, "/x", &sx);
	CHECK(sx.st_ino == sa.st_ino);
	CHECK(sx.st_nlink == 2);
	shape_fini(&sh);
}

/*
 * ZA8, ZA9, ZA10: a write is done in place, so the second name of
 * the pool sees the new bytes and the object keeps its identity.
 */
static void
check_write_links(void)
{
	struct zr_apply_stats st;
	char pf[PATHMAX], qf[PATHMAX], ff[PATHMAX];
	struct shape sh;
	struct stat before, after;

	shape_init(&sh);
	mkfile(sh.sh_from, "/p", "the new bytes", 0644);
	mkfile(sh.sh_onto, "/p", "old", 0644);
	join(pf, sizeof (pf), sh.sh_onto, "/p");
	join(qf, sizeof (qf), sh.sh_onto, "/q");
	CHECK(link(pf, qf) == 0);
	lstat_at(sh.sh_onto, "/p", &before);
	shape_ok(&sh, "    p write /p\n", 1, &st);
	CHECK(st.zs_write == 1);
	CHECK(st.zs_bytes == strlen("the new bytes"));
	lstat_at(sh.sh_onto, "/p", &after);
	CHECK(after.st_ino == before.st_ino);
	CHECK(after.st_nlink == 2);
	join(ff, sizeof (ff), sh.sh_from, "/p");
	same_bytes(pf, ff);
	same_bytes(qf, ff);
	shape_fini(&sh);
}

/*
 * ZA14, ZA15, ZA27: three nested directories removed. Each rmdir
 * waits for its scope to close, so the deepest goes first; doing any
 * of them at its own line would find the directory not empty.
 */
static void
check_rm_deep(void)
{
	struct zr_apply_stats st;
	struct shape sh;

	shape_init(&sh);
	mkdirp(sh.sh_onto, "/d", 0755);
	mkdirp(sh.sh_onto, "/d/e", 0755);
	mkdirp(sh.sh_onto, "/d/e/g", 0755);
	mkfile(sh.sh_onto, "/d/e/g/f", "leaf", 0644);
	mkfile(sh.sh_onto, "/keep", "kept", 0644);
	shape_ok(&sh,
	    "    d/ rm\n"
	    "        e/ rm\n"
	    "            g/ rm\n"
	    "                f rm\n"
	    "                ..\n"
	    "            ..\n"
	    "        ..\n", 4, &st);
	CHECK(st.zs_rm == 4);
	CHECK(absent(sh.sh_onto, "/d"));
	CHECK(!absent(sh.sh_onto, "/keep"));
	shape_fini(&sh);
}

/*
 * ZA15, ZA27: the type change. A directory is emptied and removed,
 * and the very next line gives its name to a regular file, so the
 * rmdir has to have happened when the scope closed rather than at
 * the end of the manifest.
 */
static void
check_type_change(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], ff[PATHMAX];
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	mkfile(sh.sh_from, "/d", "now a file", 0644);
	mkdirp(sh.sh_onto, "/d", 0755);
	mkfile(sh.sh_onto, "/d/c", "child", 0644);
	shape_ok(&sh,
	    "    d/ rm\n"
	    "        c rm\n"
	    "        ..\n"
	    "    d cp /d\n", 3, &st);
	CHECK(st.zs_rm == 2);
	CHECK(st.zs_cp == 1);
	lstat_at(sh.sh_onto, "/d", &s);
	CHECK(S_ISREG(s.st_mode));
	join(full, sizeof (full), sh.sh_onto, "/d");
	join(ff, sizeof (ff), sh.sh_from, "/d");
	same_bytes(full, ff);
	shape_fini(&sh);
}

/* The mtime and the atime of the from object reach the result. */
static void
check_mtime(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX];
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	mkfile(sh.sh_from, "/m", "timed", 0644);
	join(full, sizeof (full), sh.sh_from, "/m");
	settimes(full);
	shape_ok(&sh, "    m cp /m\n", 1, &st);
	lstat_at(sh.sh_onto, "/m", &s);
	checktimes(&s);
	shape_fini(&sh);
}

#ifdef TESTFLAG

/*
 * ZA20: a file flag set on from is on the result, and the times the
 * step before it are still the from object's, so the flag went on
 * last and nothing was written after it.
 */
static void
check_flag(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX];
	struct shape sh;
	struct stat s;

	shape_init(&sh);
	mkfile(sh.sh_from, "/g", "flagged", 0644);
	join(full, sizeof (full), sh.sh_from, "/g");
	settimes(full);
	CHECK(lchflags(full, TESTFLAG) == 0);
	shape_ok(&sh, "    g cp /g\n", 1, &st);
	lstat_at(sh.sh_onto, "/g", &s);
	CHECK((s.st_flags & TESTFLAG) == TESTFLAG);
	checktimes(&s);
	join(full, sizeof (full), sh.sh_onto, "/g");
	CHECK(lchflags(full, 0) == 0);
	join(full, sizeof (full), sh.sh_from, "/g");
	CHECK(lchflags(full, 0) == 0);
	shape_fini(&sh);
}

#endif	/* TESTFLAG */

/*
 * The anchor of an ln that is not there. The message names it, and
 * nothing was unlinked or created on the way to finding out.
 */
static void
check_ln_missing(void)
{
	struct shape sh;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/keep", "kept", 0644);
	shape_refused(&sh, "    x ln /nope\n", 1, "/nope");
	CHECK(absent(sh.sh_onto, "/x"));
	CHECK(!absent(sh.sh_onto, "/keep"));
	shape_fini(&sh);
}

/* ZA13: an ln whose name is held by a directory is loud, not a delete. */
static void
check_ln_over_dir(void)
{
	struct shape sh;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/a", "anchor", 0644);
	mkdirp(sh.sh_onto, "/x", 0755);
	shape_refused(&sh, "    x ln /a\n", 1, "directory");
	CHECK(!absent(sh.sh_onto, "/x"));
	shape_fini(&sh);
}

/* ZA16: a directory rm with a child the manifest did not remove. */
static void
check_rm_nonempty(void)
{
	struct shape sh;

	shape_init(&sh);
	mkdirp(sh.sh_onto, "/d", 0755);
	mkfile(sh.sh_onto, "/d/c", "still here", 0644);
	shape_refused(&sh, "    d/ rm\n    ..\n", 1, "rmdir");
	CHECK(!absent(sh.sh_onto, "/d/c"));
	shape_fini(&sh);
}

/* ZA25, ZA26: a path that tries to climb out of the root. */
static void
check_escape(void)
{
	struct shape sh;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/a", "anchor", 0644);
	shape_refused(&sh, "    x ln /../a\n", 1, "\"..\"");
	CHECK(absent(sh.sh_onto, "/x"));
	shape_fini(&sh);
}

/*
 * ZA25: a write onto a name that is a symbolic link out of the tree.
 * The apply refuses it, the link is left as it was, and the file it
 * points at is untouched.
 */
static void
check_write_symlink_out(void)
{
	char full[PATHMAX], out[PATHMAX], buf[TEXTMAX];
	struct shape sh;

	shape_init(&sh);
	mkfile(sh.sh_from, "/l", "the from bytes", 0644);
	join(out, sizeof (out), sh.sh_root, "/outside");
	mkfile(sh.sh_root, "/outside", "untouched", 0644);
	join(full, sizeof (full), sh.sh_onto, "/l");
	CHECK(symlink(out, full) == 0);
	shape_refused(&sh, "    l write /l\n", 1, "/l");
	CHECK(slurp(out, buf, sizeof (buf)) == strlen("untouched"));
	CHECK(memcmp(buf, "untouched", strlen("untouched")) == 0);
	/* the link itself is still a link, and still that one */
	CHECK(readlink(full, buf, sizeof (buf)) == (ssize_t)strlen(out));
	shape_fini(&sh);
}

/* One walked probe tree, plus the pieces the pipeline needs. */
struct world {
	char			w_base[PATHMAX];
	char			w_from[PATHMAX];
	char			w_onto[PATHMAX];
	char			w_orig[PATHMAX];
	struct zr_names		*w_ns;
	struct zr_walk		w_wb, w_wf, w_wo, w_wr;
	struct zr_oracle	*w_oracle;
	struct zr_decision	w_d;
	struct zr_parsed	w_p;
};

static void
world_build(struct world *w, const char *root)
{
	struct zr_fixture *fx = NULL;
	char err[512];

	err[0] = '\0';
	if (zr_fixture_load("tests/fixtures/probe.zrt", &fx, err,
	    sizeof (err)) != 0)
		printf("  fixture: %s\n", err);
	CHECK(fx != NULL);
	join(w->w_base, sizeof (w->w_base), root, "/base");
	join(w->w_from, sizeof (w->w_from), root, "/from");
	join(w->w_onto, sizeof (w->w_onto), root, "/onto");
	join(w->w_orig, sizeof (w->w_orig), root, "/orig");
	CHECK(mkdir(w->w_base, 0755) == 0);
	CHECK(mkdir(w->w_from, 0755) == 0);
	CHECK(mkdir(w->w_onto, 0755) == 0);
	CHECK(mkdir(w->w_orig, 0755) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_BASE, w->w_base) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_FROM, w->w_from) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_ONTO, w->w_onto) == 0);
	CHECK(zr_fixture_build(fx, ZR_FX_ONTO, w->w_orig) == 0);
	zr_fixture_free(fx);
}

/* Walk, assign content, decide, emit, parse: the --posix pipeline. */
static void
world_decide(struct world *w)
{
	struct zr_manifest_hdr hdr;
	char err[512];
	FILE *f;

	w->w_ns = zr_names_create();
	CHECK(w->w_ns != NULL);
	err[0] = '\0';
	CHECK(zr_walk(w->w_base, w->w_ns, &w->w_wb, err, sizeof (err)) == 0);
	CHECK(zr_walk(w->w_from, w->w_ns, &w->w_wf, err, sizeof (err)) == 0);
	CHECK(zr_walk(w->w_onto, w->w_ns, &w->w_wo, err, sizeof (err)) == 0);
	CHECK(zr_oracle_init(&w->w_oracle, &w->w_wb, &w->w_wf,
	    &w->w_wo) == 0);
	if (zr_oracle_assign(w->w_oracle, err, sizeof (err)) != 0)
		printf("  oracle: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(zr_decide(&w->w_wb.zw_tree, &w->w_wf.zw_tree, &w->w_wo.zw_tree,
	    ZR_MODE_STRICT, &w->w_d) == 0);
	CHECK(w->w_d.zd_nconflicts == 1);
	hdr.base = w->w_base;
	hdr.from = w->w_from;
	hdr.onto = w->w_onto;
	hdr.mode = ZR_MODE_STRICT;
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(zr_manifest_emit(f, &hdr, &w->w_wb.zw_tree, &w->w_wf.zw_tree,
	    &w->w_wo.zw_tree, &w->w_d) == 0);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, &w->w_p, err, sizeof (err)) != 0)
		printf("  parse: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
	CHECK(w->w_p.zp_actions_declared == 8);
	CHECK(w->w_p.zp_nrecords == 1);
}

static void
world_fini(struct world *w)
{
	zr_walk_fini(&w->w_wr);
	zr_parsed_fini(&w->w_p);
	zr_decision_fini(&w->w_d);
	zr_oracle_fini(w->w_oracle);
	zr_walk_fini(&w->w_wo);
	zr_walk_fini(&w->w_wf);
	zr_walk_fini(&w->w_wb);
	zr_names_destroy(w->w_ns);
}

/* The counts the stats claim are the actions the manifest holds. */
static void
check_counts(const struct world *w, const struct zr_apply_stats *st)
{
	uint64_t rm = 0, ln = 0, cp = 0, wr = 0;
	uint32_t i;

	for (i = 0; i < w->w_p.zp_nactions; i++) {
		switch (w->w_p.zp_actions[i].za_kind) {
		case ZR_ACT_RM:
			rm++;
			break;
		case ZR_ACT_LN:
			ln++;
			break;
		case ZR_ACT_CP:
			cp++;
			break;
		case ZR_ACT_WRITE:
			wr++;
			break;
		default:
			break;
		}
	}
	CHECK(st->zs_rm == rm && rm == 3);
	CHECK(st->zs_ln == ln && ln == 1);
	CHECK(st->zs_cp == cp && cp == 3);
	CHECK(st->zs_write == wr && wr == 1);
	CHECK(st->zs_rm + st->zs_ln + st->zs_cp + st->zs_write ==
	    w->w_p.zp_actions_declared);
	CHECK(st->zs_bytes > 0);
}

/*
 * Every name the decision said survives is in the walked result and
 * every name onto held and the decision dropped is gone.
 */
static void
check_names(struct world *w)
{
	const struct zr_decision *d = &w->w_d;
	zr_name_t n;

	for (n = 0; n < d->zd_nnames; n++) {
		if (d->zd_state[n] & ZR_NS_SURVIVES) {
			CHECK(zr_tree_pool(&w->w_wr.zw_tree, n) !=
			    ZR_POOL_NONE);
			continue;
		}
		if ((d->zd_state[n] & ZR_NS_ONTO) == 0)
			continue;
		if (zr_tree_pool(&w->w_wr.zw_tree, n) != ZR_POOL_NONE) {
			printf("  still there: %s\n",
			    zr_names_str(w->w_ns, n, NULL));
		}
		CHECK(zr_tree_pool(&w->w_wr.zw_tree, n) == ZR_POOL_NONE);
	}
}

/*
 * Every result pool is one object on the disk: its names all reach
 * one pool of the re-walk, that pool has exactly those names, and
 * they share one inode. ZA9 lives here -- the three names of the
 * hardlink pool are one file, and the write reached all of them.
 */
static void
check_pools(struct world *w)
{
	const struct zr_result_pool *rp;
	const struct zr_pool *p;
	zr_pool_t q;
	uint32_t k, i;

	for (k = 0; k < w->w_d.zd_npools; k++) {
		rp = &w->w_d.zd_pools[k];
		q = zr_tree_pool(&w->w_wr.zw_tree, rp->zr_names[0]);
		CHECK(q != ZR_POOL_NONE);
		p = &w->w_wr.zw_tree.zt_pools[q];
		CHECK(p->zp_nnames == rp->zr_nnames);
		for (i = 0; i < rp->zr_nnames; i++)
			CHECK(zr_tree_pool(&w->w_wr.zw_tree,
			    rp->zr_names[i]) == q);
	}
}

/*
 * The bytes of every result pool. A pool one of whose names carried
 * a cp or a write holds the bytes of the from path that action
 * named; any other pool holds what onto held before the apply, which
 * is what the untouched fourth copy is for. The conflicted name is
 * one of those, and that is ZA17.
 */
static void
check_bytes(struct world *w)
{
	char have[PATHMAX], want[PATHMAX];
	const struct zr_result_pool *rp;
	const struct zr_action *a;
	const char *arg, *path;
	uint32_t k, i, j;
	zr_name_t nm;
	size_t len;

	for (k = 0; k < w->w_d.zd_npools; k++) {
		rp = &w->w_d.zd_pools[k];
		if (w->w_wr.zw_tree.zt_pools[zr_tree_pool(&w->w_wr.zw_tree,
		    rp->zr_names[0])].zp_type != ZR_T_FILE)
			continue;
		arg = NULL;
		for (i = 0; i < rp->zr_nnames && arg == NULL; i++) {
			for (j = 0; j < w->w_p.zp_nactions; j++) {
				a = &w->w_p.zp_actions[j];
				if (a->za_kind != ZR_ACT_CP &&
				    a->za_kind != ZR_ACT_WRITE)
					continue;
				nm = zr_names_lookup(w->w_ns,
				    (const char *)a->za_path, a->za_pathlen);
				if (nm != rp->zr_names[i])
					continue;
				arg = (const char *)a->za_arg;
				break;
			}
		}
		path = zr_names_str(w->w_ns, rp->zr_names[0], &len);
		CHECK(path != NULL);
		if (arg != NULL)
			join(want, sizeof (want), w->w_from, arg);
		else
			join(want, sizeof (want), w->w_orig, path);
		for (i = 0; i < rp->zr_nnames; i++) {
			path = zr_names_str(w->w_ns, rp->zr_names[i], &len);
			CHECK(path != NULL);
			join(have, sizeof (have), w->w_onto, path);
			same_bytes(have, want);
		}
	}
}

/*
 * ZA30: the probe manifest, emitted from three real directories,
 * parsed back and applied to the onto directory. What the apply left
 * is walked again and held against the decision that wrote it.
 */
static void
check_probe(const char *root)
{
	struct zr_apply_stats st;
	char err[512], full[PATHMAX], orig[PATHMAX];
	struct world w;
	struct stat s1, s2, s3;

	memset(&w, 0, sizeof (w));
	w.w_wr.zw_rootfd = -1;
	world_build(&w, root);
	world_decide(&w);
	err[0] = '\0';
	if (zr_apply(&w.w_p, w.w_onto, &w.w_wf, &st, err, sizeof (err)) != 0)
		printf("  apply: %s\n", err);
	CHECK(err[0] == '\0');
	check_counts(&w, &st);
	err[0] = '\0';
	if (zr_walk(w.w_onto, w.w_ns, &w.w_wr, err, sizeof (err)) != 0)
		printf("  re-walk: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(zr_tree_verify(&w.w_wr.zw_tree, err, sizeof (err)) == 0);
	check_names(&w);
	check_pools(&w);
	check_bytes(&w);

	/* the three names of the edited pool are one file, with a link */
	lstat_at(w.w_onto, "/h1", &s1);
	lstat_at(w.w_onto, "/h2", &s2);
	lstat_at(w.w_onto, "/h3", &s3);
	CHECK(s1.st_ino == s2.st_ino && s2.st_ino == s3.st_ino);
	CHECK(s1.st_nlink == 3);

	/* ZA17: the conflicted name still holds what onto put there */
	join(full, sizeof (full), w.w_onto, "/a");
	join(orig, sizeof (orig), w.w_orig, "/a");
	same_bytes(full, orig);

	/* what the manifest removed is gone, and the rename landed */
	CHECK(absent(w.w_onto, "/b"));
	CHECK(absent(w.w_onto, "/d"));
	CHECK(!absent(w.w_onto, "/e/f"));
	CHECK(!absent(w.w_onto, "/n"));
	world_fini(&w);
}

int
main(void)
{
	char probe[] = "/tmp/zrapply.XXXXXX";

	CHECK(mkdtemp(probe) != NULL);
	check_probe(probe);
	rmtree(probe);

	check_cp_file();
	check_cp_symlink();
	check_cp_dir();
	check_cp_fifo();
	check_ln_replace();
	check_write_links();
	check_rm_deep();
	check_type_change();
	check_mtime();
#ifdef TESTFLAG
	check_flag();
#endif
	check_ln_missing();
	check_ln_over_dir();
	check_rm_nonempty();
	check_escape();
	check_write_symlink_out();

	printf("check_apply: %d checks passed\n", checks);
	return (0);
}
