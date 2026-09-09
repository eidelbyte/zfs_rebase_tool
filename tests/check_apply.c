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
 * And last the other document: the resolution's choices over three
 * trees, where keep, onto and from each say what the result's name
 * comes to, one group's names are pooled as the side pools them,
 * and the directory removal a conflict blocked goes through or does
 * not by what the choices left inside it.
 *
 * The family is ZA of tests/MATRIX.md. Covered here: ZA1, ZA2, ZA3,
 * ZA6, ZA7, ZA8, ZA9, ZA10, ZA11, ZA12, ZA13, ZA14, ZA15, ZA16,
 * ZA17, ZA19, ZA20, ZA22, ZA24, ZA25, ZA26, ZA27, ZA28, ZA30, ZA40
 * to ZA55, and ZA58 to ZA61 and ZA63. ZA4 and ZA5 need mknod, which
 * needs root; ZA18 cannot see the chown at all, since an apply run
 * by the tree's own owner skips it; ZA21 wants an immutable file and
 * ZA23 a forced re-stat mismatch, both of which need root; ZA29 and
 * ZA57 are the ACL cells already deferred in the matrix; ZA56 is
 * applying2 itself, which needs the box; and ZA62 is the securelevel
 * refusal, which needs a box booted above securelevel 0.
 *
 * ZA58 to ZA61 are the flags in the way of an action, which the Mac
 * says with uchg and uappnd where the target would say it with schg
 * and sappnd: ZFS has no user immutable flag, so on the box those
 * cases are root's and the probe says so with a skip line where
 * neither flag can be set.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

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

/*
 * A template under TMPDIR, or /tmp without it: the box's /tmp may be
 * a tmpfs, which has no extended attributes, and the tests that
 * build attributes must be pointed at a filesystem that has them.
 */
static void
tmp_template(char *buf, size_t len, const char *leaf)
{
	const char *d = getenv("TMPDIR");

	(void) snprintf(buf, len, "%s/%s", d != NULL && d[0] != '\0' ?
	    d : "/tmp", leaf);
}

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
#ifdef TESTFLAG
		/*
		 * A test that set an immutable or append-only flag and
		 * failed before its apply cleared it would hold its own
		 * tree down; the shell harnesses clear the flags the
		 * same way before they remove a built tree.
		 */
		(void) lchflags(full, 0);
#endif
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

/*
 * A unix-domain socket at one name, made the way src/apply.c makes
 * one: bind(2), and then the mode, since bind takes none.
 */
static void
mksock(const char *root, const char *rel, mode_t mode)
{
	struct sockaddr_un sun;
	char full[PATHMAX];
	size_t len;
	int fd;

	join(full, sizeof (full), root, rel);
	len = strlen(full);
	CHECK(len < sizeof (sun.sun_path));
	memset(&sun, 0, sizeof (sun));
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, full, len);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	CHECK(bind(fd, (const struct sockaddr *)&sun,
	    (socklen_t)sizeof (sun)) == 0);
	CHECK(close(fd) == 0);
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
	    "#rebase-manifest 5\n"
	    "#result -\n"
	    "#form posix\n"
	    "#base b 0\n"
	    "#from f 0\n"
	    "#onto o 0\n"
	    "#made -\n"
	    "#tag -\n"
	    "#take -\n"
	    "#written -\n"
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

/*
 * And the same text refused, which is where a path that climbs out
 * of the root is now stopped: the parse reads the components the
 * apply would have refused, so no tree is touched at all (ZM86,
 * ZM87).
 */
static void
parse_body_refused(const char *body, int nactions, const char *word)
{
	char text[TEXTMAX], err[256];
	struct zr_parsed p;
	FILE *f;
	int n;

	n = snprintf(text, sizeof (text),
	    "#rebase-manifest 5\n"
	    "#result -\n"
	    "#form posix\n"
	    "#base b 0\n"
	    "#from f 0\n"
	    "#onto o 0\n"
	    "#made -\n"
	    "#tag -\n"
	    "#take -\n"
	    "#written -\n"
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
	CHECK(zr_manifest_parse(f, &p, err, sizeof (err)) == -1);
	if (strstr(err, word) == NULL)
		printf("  message lacks \"%s\": %s\n", word, err);
	CHECK(strstr(err, word) != NULL);
	CHECK(fclose(f) == 0);
	zr_parsed_fini(&p);
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
	char tmpl[256];

	memset(sh, 0, sizeof (*sh));
	tmp_template(tmpl, sizeof (tmpl), "zrapplys.XXXXXX");
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

	struct zr_walk wo;
	char werr[256];

	if (sh->sh_walked == 0)
		shape_walk(sh);
	parse_body(&p, body, nactions);
	/* the onto tree as it stands before apply, the dup source */
	CHECK(zr_walk(sh->sh_onto, sh->sh_ns, &wo, werr, sizeof (werr)) == 0);
	rc = zr_apply_with(&p, sh->sh_onto, &sh->sh_wf, &wo, NULL, st, err,
	    errlen);
	zr_walk_fini(&wo);
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
/*
 * ZA: dup. onto holds A and B as one file; the manifest severs B as
 * a copy of onto's own bytes (from has nothing to offer). Afterwards
 * B is its own object with A's bytes and mode, A is untouched, and
 * the count lands in zs_dup, not zs_cp.
 */
static void
check_dup_severs(void)
{
	struct zr_apply_stats st;
	char a[PATHMAX], b[PATHMAX];
	struct shape sh;
	struct stat sa, sb;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/A", "onto bytes", 0640);
	join(a, sizeof (a), sh.sh_onto, "/A");
	join(b, sizeof (b), sh.sh_onto, "/B");
	CHECK(link(a, b) == 0);
	shape_ok(&sh, "    B dup /A\n", 1, &st);
	CHECK(st.zs_dup == 1);
	CHECK(st.zs_cp == 0);
	lstat_at(sh.sh_onto, "/A", &sa);
	lstat_at(sh.sh_onto, "/B", &sb);
	CHECK(S_ISREG(sb.st_mode));
	CHECK(sa.st_ino != sb.st_ino);
	CHECK(sa.st_nlink == 1);
	CHECK((sb.st_mode & 07777) == 0640);
	same_bytes(b, a);
	shape_fini(&sh);
}

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
 * ZA7: a socket copied as a socket. There is no mknod for one and
 * no portable bindat, so the apply binds an AF_UNIX socket at the
 * path built from the root; what the result holds afterwards is a
 * socket with from's mode, and the walk reads it back as one.
 */
static void
check_cp_sock(void)
{
	struct zr_apply_stats st;
	struct zr_walk w;
	struct shape sh;
	struct stat s;
	zr_name_t nm;
	zr_pool_t po;
	char err[256];

	shape_init(&sh);
	mksock(sh.sh_from, "/k", 0660);
	shape_ok(&sh, "    k cp /k\n", 1, &st);
	CHECK(st.zs_cp == 1);
	lstat_at(sh.sh_onto, "/k", &s);
	CHECK(S_ISSOCK(s.st_mode));
	CHECK((s.st_mode & 07777) == 0660);
	/* and the tool's own reader calls it one */
	err[0] = '\0';
	CHECK(zr_walk(sh.sh_onto, sh.sh_ns, &w, err, sizeof (err)) == 0);
	nm = zr_names_lookup(w.zw_tree.zt_names, "/k", 2);
	CHECK(nm != ZR_NAME_NONE);
	po = zr_tree_pool(&w.zw_tree, nm);
	CHECK(po != ZR_POOL_NONE);
	CHECK(w.zw_tree.zt_pools[po].zp_type == ZR_T_SOCK);
	zr_walk_fini(&w);
	shape_fini(&sh);
}

/*
 * ZA63: the repair puts a socket back. The result lost a name the
 * manifest never spoke for, and the repair copies it out of onto
 * again -- which for a socket is the same bind the cp does, through
 * the same code.
 */
static void
check_repair_sock(void)
{
	struct zr_apply_stats st;
	struct zr_walk wo, wf;
	struct zr_parsed p;
	struct shape sh;
	struct stat s;
	char res[PATHMAX], full[PATHMAX], err[512];

	shape_init(&sh);
	/* onto is the anchor: a file and a socket beside it */
	mkfile(sh.sh_onto, "/A", "kept", 0644);
	mksock(sh.sh_onto, "/sk", 0600);
	/* and the result, which somebody left without the socket */
	join(res, sizeof (res), sh.sh_root, "/res");
	CHECK(mkdir(res, 0755) == 0);
	mkfile(res, "/A", "kept", 0644);
	parse_body(&p, "", 0);
	err[0] = '\0';
	CHECK(zr_walk(sh.sh_onto, sh.sh_ns, &wo, err, sizeof (err)) == 0);
	CHECK(zr_walk(sh.sh_from, sh.sh_ns, &wf, err, sizeof (err)) == 0);
	err[0] = '\0';
	if (zr_apply_check(&p, res, sh.sh_ns, &wo, &wf, 0, 1, &st, NULL, err,
	    sizeof (err)) != 0)
		printf("  check: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(st.zs_restored == 1);
	join(full, sizeof (full), res, "/sk");
	CHECK(lstat(full, &s) == 0);
	CHECK(S_ISSOCK(s.st_mode));
	CHECK((s.st_mode & 07777) == 0600);
	zr_walk_fini(&wf);
	zr_walk_fini(&wo);
	zr_parsed_fini(&p);
	rmtree(res);
	shape_fini(&sh);
}

/*
 * ZY109: the walk the check ends with, handed back. The caller says
 * where it is to be left, and what comes back is a walk of the
 * result -- the same pools a walk made here finds -- and an oracle
 * over onto, from and it, ready to be asked. The walk the repair
 * threw away is not the one handed over: the check walks again after
 * it writes, so what the caller gets holds the socket the repair put
 * back.
 */
static void
check_kept_walk(void)
{
	struct zr_apply_kept kept;
	struct zr_apply_stats st;
	struct zr_walk wo, wf, mine, fresh;
	struct zr_parsed p;
	struct shape sh;
	char res[PATHMAX], err[512];
	zr_name_t nm;
	zr_pool_t pk, pm;

	shape_init(&sh);
	mkfile(sh.sh_onto, "/A", "kept", 0644);
	mksock(sh.sh_onto, "/sk", 0600);
	join(res, sizeof (res), sh.sh_root, "/res");
	CHECK(mkdir(res, 0755) == 0);
	mkfile(res, "/A", "kept", 0644);
	parse_body(&p, "", 0);
	err[0] = '\0';
	CHECK(zr_walk(sh.sh_onto, sh.sh_ns, &wo, err, sizeof (err)) == 0);
	CHECK(zr_walk(sh.sh_from, sh.sh_ns, &wf, err, sizeof (err)) == 0);
	memset(&kept, 0, sizeof (kept));
	kept.zk_walk = &mine;
	err[0] = '\0';
	if (zr_apply_check(&p, res, sh.sh_ns, &wo, &wf, 0, 1, &st, &kept,
	    err, sizeof (err)) != 0)
		printf("  check: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(st.zs_restored == 1);
	CHECK(kept.zk_live == 1);
	CHECK(kept.zk_oracle != NULL);
	CHECK(mine.zw_rootfd >= 0);
	/* the socket the repair put back is in the walk handed over */
	nm = zr_names_lookup(sh.sh_ns, "/sk", 3);
	CHECK(nm != ZR_NAME_NONE);
	pk = zr_tree_pool(&mine.zw_tree, nm);
	CHECK(pk != ZR_POOL_NONE);
	CHECK(mine.zw_tree.zt_pools[pk].zp_type == ZR_T_SOCK);
	/* and it is the tree a walk made here finds, pool for pool */
	err[0] = '\0';
	CHECK(zr_walk(res, sh.sh_ns, &fresh, err, sizeof (err)) == 0);
	pm = zr_tree_pool(&fresh.zw_tree, nm);
	CHECK(pm != ZR_POOL_NONE);
	CHECK(fresh.zw_tree.zt_npools == mine.zw_tree.zt_npools);
	CHECK(fresh.zw_tree.zt_pools[pm].zp_type ==
	    mine.zw_tree.zt_pools[pk].zp_type);
	zr_walk_fini(&fresh);
	zr_oracle_fini(kept.zk_oracle);
	zr_walk_fini(&mine);
	zr_walk_fini(&wf);
	zr_walk_fini(&wo);
	zr_parsed_fini(&p);
	rmtree(res);
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

/*
 * The flag of this pair that this filesystem and this user can put
 * on a file and take off again: the user one where there is a user
 * one (the Mac, UFS), the system one where this is root and
 * securelevel is 0 or less (ZFS, which has no user immutable flag at
 * all: zfs_freebsd_setattr answers uchg with EOPNOTSUPP), and zero
 * where there is neither. check_fixture.c probes the same way, for
 * the same reason; the caller says what it skipped. The probe file
 * is made and removed here, so it is never one of the shape's names.
 */
static uint32_t
settable_flag(const char *dir, uint32_t uflag, uint32_t sflag)
{
	char full[PATHMAX];
	uint32_t got = 0;
	FILE *fp;
#ifdef __FreeBSD__
	int level = 0;
	size_t len = sizeof (level);
#endif

	join(full, sizeof (full), dir, "/flag-probe");
	fp = fopen(full, "w");
	CHECK(fp != NULL);
	CHECK(fclose(fp) == 0);
	if (uflag != 0 && lchflags(full, (unsigned long)uflag) == 0)
		got = uflag;
#ifdef __FreeBSD__
	else if (sflag != 0 && geteuid() == 0 &&
	    sysctlbyname("kern.securelevel", &level, &len, NULL, 0) == 0 &&
	    level <= 0 && lchflags(full, (unsigned long)sflag) == 0)
		got = sflag;
#else
	(void) sflag;
#endif
	CHECK(lchflags(full, 0) == 0);
	CHECK(unlink(full) == 0);
	return (got);
}

static uint32_t
immutable_flag(const char *dir)
{
#ifdef UF_IMMUTABLE
	return (settable_flag(dir, (uint32_t)UF_IMMUTABLE,
	    (uint32_t)SF_IMMUTABLE));
#else
	return (settable_flag(dir, 0, (uint32_t)SF_IMMUTABLE));
#endif
}

static uint32_t
append_flag(const char *dir)
{
#ifdef UF_APPEND
	return (settable_flag(dir, (uint32_t)UF_APPEND, (uint32_t)SF_APPEND));
#else
	return (settable_flag(dir, 0, (uint32_t)SF_APPEND));
#endif
}

/* What one name of the result carries just now. */
static uint32_t
flags_at(const char *root, const char *rel)
{
	struct stat s;

	lstat_at(root, rel, &s);
	return ((uint32_t)s.st_flags);
}

/*
 * ZA58: an object the manifest removes is under an immutable flag,
 * which on ZFS refuses the unlink outright (zfs_zaccess_delete
 * returns EPERM for ZFS_IMMUTABLE and ZFS_NOUNLINK). The apply takes
 * the flag off first and the name goes.
 */
static void
check_rm_immutable(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX];
	struct shape sh;
	uint32_t fl;

	shape_init(&sh);
	fl = immutable_flag(sh.sh_onto);
	if (fl == 0) {
		printf("  skip ZA58: no immutable flag this filesystem and "
		    "user can set; ZFS has no uchg, and schg is root's\n");
		shape_fini(&sh);
		return;
	}
	mkfile(sh.sh_onto, "/g", "held down", 0644);
	join(full, sizeof (full), sh.sh_onto, "/g");
	CHECK(lchflags(full, (unsigned long)fl) == 0);
	shape_ok(&sh, "    g rm\n", 1, &st);
	CHECK(st.zs_rm == 1);
	CHECK(absent(sh.sh_onto, "/g"));
	shape_fini(&sh);
}

/*
 * ZA59: the same flag on an object a write rewrites. Without the
 * clearing step the open for truncation is EPERM; with it the bytes
 * are from's, and so are the flags, since the flags an action sets
 * are still written last and from's object carries none.
 */
static void
check_write_immutable(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], onto[PATHMAX];
	struct shape sh;
	uint32_t fl;

	shape_init(&sh);
	fl = immutable_flag(sh.sh_onto);
	if (fl == 0) {
		printf("  skip ZA59: no immutable flag this filesystem and "
		    "user can set\n");
		shape_fini(&sh);
		return;
	}
	mkfile(sh.sh_from, "/w", "the new bytes", 0644);
	mkfile(sh.sh_onto, "/w", "the old bytes", 0644);
	join(full, sizeof (full), sh.sh_onto, "/w");
	CHECK(lchflags(full, (unsigned long)fl) == 0);
	shape_ok(&sh, "    w write /w\n", 1, &st);
	CHECK(st.zs_write == 1);
	join(onto, sizeof (onto), sh.sh_from, "/w");
	same_bytes(full, onto);
	CHECK((flags_at(sh.sh_onto, "/w") & fl) == 0);
	shape_fini(&sh);
}

/*
 * ZA60: a write with no bytes in it -- a fifo, whose write is its
 * attributes and nothing else -- over an object under an immutable
 * flag. That is the attribute half of the same refusal: an immutable
 * object takes no chown, no chmod and no times either (zfs_setattr).
 */
static void
check_attrs_immutable(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX];
	struct shape sh;
	struct stat s;
	uint32_t fl;

	shape_init(&sh);
	fl = immutable_flag(sh.sh_onto);
	if (fl == 0) {
		printf("  skip ZA60: no immutable flag this filesystem and "
		    "user can set\n");
		shape_fini(&sh);
		return;
	}
	join(full, sizeof (full), sh.sh_from, "/p");
	CHECK(mkfifo(full, 0640) == 0);
	CHECK(chmod(full, 0640) == 0);
	join(full, sizeof (full), sh.sh_onto, "/p");
	CHECK(mkfifo(full, 0600) == 0);
	CHECK(chmod(full, 0600) == 0);
	CHECK(lchflags(full, (unsigned long)fl) == 0);
	shape_ok(&sh, "    p write /p\n", 1, &st);
	CHECK(st.zs_write == 1);
	lstat_at(sh.sh_onto, "/p", &s);
	CHECK(S_ISFIFO(s.st_mode));
	CHECK((s.st_mode & 07777) == 0640);
	CHECK(((uint32_t)s.st_flags & fl) == 0);
	shape_fini(&sh);
}

/*
 * ZA61: an append-only flag on an object a write rewrites. ZFS
 * refuses a non-appending write to one (zfs_write returns EPERM for
 * ZFS_APPENDONLY without O_APPEND), and a truncating open is exactly
 * that, so this is the second flag family and not a spelling of the
 * first.
 */
static void
check_write_append_only(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], src[PATHMAX];
	struct shape sh;
	uint32_t fl;

	shape_init(&sh);
	fl = append_flag(sh.sh_onto);
	if (fl == 0) {
		printf("  skip ZA61: no append-only flag this filesystem and "
		    "user can set; sappnd is root's\n");
		shape_fini(&sh);
		return;
	}
	mkfile(sh.sh_from, "/a", "the new bytes", 0644);
	mkfile(sh.sh_onto, "/a", "the old bytes", 0644);
	join(full, sizeof (full), sh.sh_onto, "/a");
	CHECK(lchflags(full, (unsigned long)fl) == 0);
	shape_ok(&sh, "    a write /a\n", 1, &st);
	CHECK(st.zs_write == 1);
	join(src, sizeof (src), sh.sh_from, "/a");
	same_bytes(full, src);
	CHECK((flags_at(sh.sh_onto, "/a") & fl) == 0);
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

/*
 * ZA25, ZA26: a path that tries to climb out of the root. The parse
 * is where it is refused now, so the apply is never reached and the
 * tree is never opened: za_path_ok stays as the second line, for the
 * paths the apply builds itself.
 */
static void
check_escape(void)
{
	struct zr_apply_stats st;
	struct shape sh;

	parse_body_refused("    x ln /../a\n", 1, "\"..\"");
	parse_body_refused("    x ln /\\056\\056/a\n", 1, "\"..\"");
	shape_init(&sh);
	mkfile(sh.sh_onto, "/a", "anchor", 0644);
	mkfile(sh.sh_onto, "/x", "onto's own", 0644);
	shape_ok(&sh, "    x ln /a\n", 1, &st);
	CHECK(!absent(sh.sh_onto, "/x"));
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

/*
 * ---------------------------------------------------------------
 * The other document: the resolution's choices. Three trees by hand
 * -- onto, from, and the result the applying1 stage left -- a
 * manifest written as text so that its conflict marks and its
 * blocked removals are real, and the skeleton the tool itself would
 * write beside it, answered here the way a picker answers it.
 * ---------------------------------------------------------------
 */

struct pick {
	char		pk_root[PATHMAX];
	char		pk_onto[PATHMAX];
	char		pk_from[PATHMAX];
	char		pk_res[PATHMAX];
	struct zr_names	*pk_ns;
};

static void
pick_init(struct pick *p)
{
	char tmpl[256];

	memset(p, 0, sizeof (*p));
	tmp_template(tmpl, sizeof (tmpl), "zrchoice.XXXXXX");
	CHECK(mkdtemp(tmpl) != NULL);
	join(p->pk_root, sizeof (p->pk_root), tmpl, "");
	join(p->pk_onto, sizeof (p->pk_onto), tmpl, "/onto");
	join(p->pk_from, sizeof (p->pk_from), tmpl, "/from");
	join(p->pk_res, sizeof (p->pk_res), tmpl, "/res");
	CHECK(mkdir(p->pk_onto, 0755) == 0);
	CHECK(mkdir(p->pk_from, 0755) == 0);
	CHECK(mkdir(p->pk_res, 0755) == 0);
	p->pk_ns = zr_names_create();
	CHECK(p->pk_ns != NULL);
}

static void
pick_fini(struct pick *p)
{
	zr_names_destroy(p->pk_ns);
	rmtree(p->pk_root);
}

/* The two documents of one rebase: the manifest and its skeleton. */
struct doc {
	struct zr_parsed	dc_m;
	struct zr_resolution	dc_r;
};

/*
 * The manifest around one tree section body and the conflict records
 * its marks point at, and then the skeleton of it, which is the
 * document write_skeleton() puts beside the manifest in a real run.
 */
static void
doc_build(struct doc *d, const char *body, int nactions, int nconf,
    const char *records)
{
	char text[TEXTMAX], err[256];
	FILE *f;
	int n;

	memset(d, 0, sizeof (*d));
	n = snprintf(text, sizeof (text),
	    "#rebase-manifest 5\n"
	    "#result -\n"
	    "#form posix\n"
	    "#base b 0\n"
	    "#from f 0\n"
	    "#onto o 0\n"
	    "#made -\n"
	    "#tag -\n"
	    "#take -\n"
	    "#written -\n"
	    "#mode strict\n"
	    "#actions %d\n"
	    "#conflicts %d\n"
	    "/\n"
	    "%s"
	    "..\n"
	    "%s", nactions, nconf, body, records);
	CHECK(n > 0 && (size_t)n < sizeof (text));
	f = tmpfile();
	CHECK(f != NULL);
	CHECK(fputs(text, f) != EOF);
	rewind(f);
	err[0] = '\0';
	if (zr_manifest_parse(f, &d->dc_m, err, sizeof (err)) != 0)
		printf("  parse: %s\n", err);
	CHECK(err[0] == '\0');
	CHECK(fclose(f) == 0);
	CHECK(zr_resolution_skeleton(&d->dc_m, ZR_CH_NONE, &d->dc_r) == 0);
}

static void
doc_fini(struct doc *d)
{
	zr_resolution_fini(&d->dc_r);
	zr_parsed_fini(&d->dc_m);
}

/* One conflicted name answered, as the picker would answer it. */
static void
doc_choose(struct doc *d, const char *path, enum zr_choice ch)
{
	uint32_t i;

	for (i = 0; i < d->dc_r.zs_nlines; i++) {
		if (strcmp((const char *)d->dc_r.zs_lines[i].zl_path,
		    path) == 0) {
			d->dc_r.zs_lines[i].zl_choice = ch;
			return;
		}
	}
	printf("  no resolution line for %s\n", path);
	CHECK(0);
}

/* One clean name a verify found changed, with the choice it took. */
static void
doc_drift(struct doc *d, const char *path, enum zr_choice ch)
{
	CHECK(zr_resolution_add_drift(&d->dc_r, (const unsigned char *)path,
	    strlen(path), 0, ch) == 0);
}

/* The same on a directory, which is what scopes the lines under it. */
static void
doc_drift_dir(struct doc *d, const char *path, enum zr_choice ch)
{
	CHECK(zr_resolution_add_drift(&d->dc_r, (const unsigned char *)path,
	    strlen(path), 1, ch) == 0);
}

/* One line the manifest never marked, added by hand as a person can. */
static void
doc_add_conflict(struct doc *d, const char *path, uint32_t group,
    enum zr_choice ch)
{
	CHECK(zr_resolution_add_conflict(&d->dc_r, (const unsigned char *)path,
	    strlen(path), 0, group, ch) == 0);
}

/*
 * The two sides walked afresh -- they never change, but the result
 * does, and the apply walks that itself -- and the choices carried
 * out on the result tree.
 */
static int
pick_apply(struct pick *p, struct doc *d, struct zr_apply_stats *st, char *err,
    size_t errlen)
{
	struct zr_walk wo, wf;
	char werr[256];
	int rc;

	werr[0] = '\0';
	CHECK(zr_walk(p->pk_onto, p->pk_ns, &wo, werr, sizeof (werr)) == 0);
	CHECK(zr_walk(p->pk_from, p->pk_ns, &wf, werr, sizeof (werr)) == 0);
	rc = zr_apply_choices(&d->dc_r, &d->dc_m, p->pk_res, p->pk_ns, &wo,
	    &wf, NULL, st, err, errlen);
	zr_walk_fini(&wf);
	zr_walk_fini(&wo);
	return (rc);
}

/* The apply must succeed, and its message must have stayed empty. */
static void
pick_ok(struct pick *p, struct doc *d, struct zr_apply_stats *st)
{
	char err[512];

	err[0] = '\0';
	if (pick_apply(p, d, st, err, sizeof (err)) != 0)
		printf("  choices: %s\n", err);
	CHECK(err[0] == '\0');
}

/*
 * ZA52: the same document again, which must find everything it asks
 * for already true. That is the check applying2 makes on itself, so
 * every shape below ends with it.
 */
static void
pick_stable(struct pick *p, struct doc *d)
{
	struct zr_apply_stats st;

	pick_ok(p, d, &st);
	CHECK(st.zs_made == 0);
	CHECK(st.zs_dropped == 0);
	CHECK(st.zs_linked == 0);
	CHECK(st.zs_latedirs == 0);
	CHECK(st.zs_line == ZR_LINE_NONE);
}

/* The apply must refuse, and say so with the word in it. */
static void
pick_refused(struct pick *p, struct doc *d, const char *word)
{
	struct zr_apply_stats st;
	char err[512];

	err[0] = '\0';
	CHECK(pick_apply(p, d, &st, err, sizeof (err)) == -1);
	if (strstr(err, word) == NULL)
		printf("  message lacks \"%s\": %s\n", word, err);
	CHECK(strstr(err, word) != NULL);
}

/* One name's bytes, held against the text the test named. */
static void
has_bytes(const char *root, const char *rel, const char *text)
{
	char full[PATHMAX], buf[TEXTMAX];
	size_t n;

	join(full, sizeof (full), root, rel);
	n = slurp(full, buf, sizeof (buf));
	if (n != strlen(text) || memcmp(buf, text, n) != 0)
		printf("  %s: not the bytes the test wanted\n", full);
	CHECK(n == strlen(text));
	CHECK(memcmp(buf, text, n) == 0);
}

/* Two names of one tree: are they one object? */
static int
one_object(const char *root, const char *a, const char *b)
{
	struct stat sa, sb;

	lstat_at(root, a, &sa);
	lstat_at(root, b, &sb);
	return (sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev);
}

/* One hard link inside a tree the test is building. */
static void
mklink(const char *root, const char *from, const char *to)
{
	char a[PATHMAX], b[PATHMAX];

	join(a, sizeof (a), root, from);
	join(b, sizeof (b), root, to);
	CHECK(link(a, b) == 0);
}

/* One symbolic link inside a tree the test is building. */
static void
mksym(const char *root, const char *rel, const char *target)
{
	char full[PATHMAX];

	join(full, sizeof (full), root, rel);
	CHECK(symlink(target, full) == 0);
}

/*
 * ZA40: the choice keep leaves the name exactly as the person left
 * it. The result holds a hand merge, which is neither side's object,
 * and both the object and its bytes are still there afterwards.
 */
static void
check_choice_keep(void)
{
	struct zr_apply_stats st;
	struct stat before, after;
	struct pick p;
	struct doc d;
	static const char body[] = "    k conflict 1\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /k changed on both sides\n"
	    "  base ()\n"
	    "  from ({/k}y)\n"
	    "  onto ({/k}z)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/k", "onto bytes\n", 0644);
	mkfile(p.pk_from, "/k", "from bytes\n", 0644);
	mkfile(p.pk_res, "/k", "the hand merge\n", 0644);
	doc_build(&d, body, 0, 1, records);
	doc_choose(&d, "/k", ZR_CH_KEEP);
	lstat_at(p.pk_res, "/k", &before);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_kept == 1);
	CHECK(st.zs_made == 0);
	CHECK(st.zs_dropped == 0);
	CHECK(st.zs_linked == 0);
	CHECK(st.zs_line == ZR_LINE_NONE);
	lstat_at(p.pk_res, "/k", &after);
	CHECK(before.st_ino == after.st_ino);
	has_bytes(p.pk_res, "/k", "the hand merge\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA41, ZA51: the choice onto puts onto's bytes, mode and extended
 * attribute back over an edit, and a name the result already holds
 * as onto has it is left alone and counted as skipped.
 */
static void
check_choice_onto(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], val[16];
	struct pick p;
	struct doc d;
	struct stat sr;
	static const char body[] =
	    "    e conflict 1\n"
	    "    s conflict 2\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /e changed on both sides\n"
	    "  base ()\n"
	    "  from ({/e}y)\n"
	    "  onto ({/e}z)\n"
	    "conflict 2 changed-both\n"
	    "  why  /s changed on both sides\n"
	    "  base ()\n"
	    "  from ({/s}y)\n"
	    "  onto ({/s}z)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/e", "onto bytes\n", 0640);
	join(full, sizeof (full), p.pk_onto, "/e");
	CHECK(setx(full, XNAME, XVAL, XLEN) == 0);
	mkfile(p.pk_from, "/e", "from bytes\n", 0644);
	mkfile(p.pk_res, "/e", "somebody edited this\n", 0600);
	mkfile(p.pk_onto, "/s", "same both ways\n", 0644);
	mkfile(p.pk_from, "/s", "other bytes\n", 0644);
	mkfile(p.pk_res, "/s", "same both ways\n", 0644);
	doc_build(&d, body, 0, 2, records);
	doc_choose(&d, "/e", ZR_CH_ONTO);
	doc_choose(&d, "/s", ZR_CH_ONTO);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 1);
	CHECK(st.zs_skipped == 1);
	CHECK(st.zs_dropped == 0);
	CHECK(st.zs_line == 0);
	has_bytes(p.pk_res, "/e", "onto bytes\n");
	lstat_at(p.pk_res, "/e", &sr);
	CHECK((sr.st_mode & 07777) == 0640);
	join(full, sizeof (full), p.pk_res, "/e");
	CHECK(getx(full, XNAME, val, sizeof (val)) == XLEN);
	CHECK(memcmp(val, XVAL, XLEN) == 0);
	has_bytes(p.pk_res, "/s", "same both ways\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA43, ZA49: the choice from makes the name from's object -- its
 * bytes and its mode for a file, and its target for a symbolic link,
 * which replaces the regular file the result held there.
 */
static void
check_choice_from(void)
{
	struct zr_apply_stats st;
	char full[PATHMAX], buf[TEXTMAX];
	struct pick p;
	struct doc d;
	struct stat sr;
	static const char body[] =
	    "    g conflict 1\n"
	    "    l conflict 2\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /g changed on both sides\n"
	    "  base ()\n"
	    "  from ({/g}y)\n"
	    "  onto ({/g}z)\n"
	    "conflict 2 contested-home\n"
	    "  why  /l was made on both sides\n"
	    "  base ()\n"
	    "  from ({/l}y)\n"
	    "  onto ({/l}z)\n";

	pick_init(&p);
	mkfile(p.pk_from, "/g", "from bytes\n", 0600);
	mksym(p.pk_from, "/l", "a/target");
	mkfile(p.pk_onto, "/g", "onto bytes\n", 0644);
	mkfile(p.pk_onto, "/l", "not a link\n", 0644);
	mkfile(p.pk_res, "/g", "onto bytes\n", 0644);
	mkfile(p.pk_res, "/l", "not a link\n", 0644);
	doc_build(&d, body, 0, 2, records);
	doc_choose(&d, "/g", ZR_CH_FROM);
	doc_choose(&d, "/l", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 2);
	CHECK(st.zs_skipped == 0);
	has_bytes(p.pk_res, "/g", "from bytes\n");
	lstat_at(p.pk_res, "/g", &sr);
	CHECK((sr.st_mode & 07777) == 0600);
	lstat_at(p.pk_res, "/l", &sr);
	CHECK(S_ISLNK(sr.st_mode));
	join(full, sizeof (full), p.pk_res, "/l");
	CHECK(readlink(full, buf, sizeof (buf)) == (ssize_t)strlen("a/target"));
	CHECK(memcmp(buf, "a/target", strlen("a/target")) == 0);
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA42, ZA44: a choice of a side that has no such name takes the
 * name away -- onto's absence for one name, from's for the other --
 * and a second pass finds both already gone.
 */
static void
check_choice_gone(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	static const char body[] =
	    "    x conflict 1\n"
	    "    y conflict 2\n";
	static const char records[] =
	    "\n"
	    "conflict 1 orphaned-add\n"
	    "  why  /x was added on one side only\n"
	    "  base ()\n"
	    "  from ({/x}y)\n"
	    "  onto ()\n"
	    "conflict 2 orphaned-add\n"
	    "  why  /y was added on one side only\n"
	    "  base ()\n"
	    "  from ()\n"
	    "  onto ({/y}z)\n";

	pick_init(&p);
	mkfile(p.pk_from, "/x", "from only\n", 0644);
	mkfile(p.pk_onto, "/y", "onto only\n", 0644);
	mkfile(p.pk_res, "/x", "from only\n", 0644);
	mkfile(p.pk_res, "/y", "onto only\n", 0644);
	doc_build(&d, body, 0, 2, records);
	doc_choose(&d, "/x", ZR_CH_ONTO);
	doc_choose(&d, "/y", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_dropped == 2);
	CHECK(st.zs_made == 0);
	CHECK(st.zs_line == 0);
	CHECK(absent(p.pk_res, "/x"));
	CHECK(absent(p.pk_res, "/y"));
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA45, ZA46: one group, one side, and the pooling that side has.
 * The two names from holds as one file end as one object here, the
 * first of them copied and the second linked onto it; the two names
 * from holds apart end as two.
 */
static void
check_choice_pool(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	struct stat s1;
	static const char body[] =
	    "    p1 conflict 1\n"
	    "    p2 conflict 1\n"
	    "    q1 conflict 2\n"
	    "    q2 conflict 2\n";
	static const char records[] =
	    "\n"
	    "conflict 1 disagree\n"
	    "  why  /p1 and /p2 are one file on one side only\n"
	    "  base ()\n"
	    "  from ({/p1 /p2}y)\n"
	    "  onto ({/p1}z {/p2}w)\n"
	    "conflict 2 disagree\n"
	    "  why  /q1 and /q2 disagree\n"
	    "  base ()\n"
	    "  from ({/q1}y {/q2}w)\n"
	    "  onto ({/q1}v {/q2}u)\n";

	pick_init(&p);
	mkfile(p.pk_from, "/p1", "shared\n", 0644);
	mklink(p.pk_from, "/p1", "/p2");
	mkfile(p.pk_from, "/q1", "one\n", 0644);
	mkfile(p.pk_from, "/q2", "two\n", 0644);
	mkfile(p.pk_onto, "/p1", "onto p1\n", 0644);
	mkfile(p.pk_onto, "/p2", "onto p2\n", 0644);
	mkfile(p.pk_onto, "/q1", "onto q1\n", 0644);
	mkfile(p.pk_onto, "/q2", "onto q2\n", 0644);
	mkfile(p.pk_res, "/p1", "onto p1\n", 0644);
	mkfile(p.pk_res, "/p2", "onto p2\n", 0644);
	mkfile(p.pk_res, "/q1", "onto q1\n", 0644);
	mkfile(p.pk_res, "/q2", "onto q2\n", 0644);
	doc_build(&d, body, 0, 2, records);
	doc_choose(&d, "/p1", ZR_CH_FROM);
	doc_choose(&d, "/p2", ZR_CH_FROM);
	doc_choose(&d, "/q1", ZR_CH_FROM);
	doc_choose(&d, "/q2", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 3);
	CHECK(st.zs_linked == 1);
	CHECK(one_object(p.pk_res, "/p1", "/p2"));
	lstat_at(p.pk_res, "/p1", &s1);
	CHECK(s1.st_nlink == 2);
	has_bytes(p.pk_res, "/p1", "shared\n");
	has_bytes(p.pk_res, "/p2", "shared\n");
	CHECK(!one_object(p.pk_res, "/q1", "/q2"));
	has_bytes(p.pk_res, "/q1", "one\n");
	has_bytes(p.pk_res, "/q2", "two\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA47: one group, two choices. Both sides hold the two names as one
 * file; the name that chose onto and the name that chose from end as
 * two objects, each holding its own side's bytes, neither pooled.
 */
static void
check_choice_mixed(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	struct stat s1, s2;
	static const char body[] =
	    "    m1 conflict 1\n"
	    "    m2 conflict 1\n";
	static const char records[] =
	    "\n"
	    "conflict 1 disagree\n"
	    "  why  /m1 and /m2 changed on both sides\n"
	    "  base ()\n"
	    "  from ({/m1 /m2}y)\n"
	    "  onto ({/m1 /m2}z)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/m1", "onto shared\n", 0644);
	mklink(p.pk_onto, "/m1", "/m2");
	mkfile(p.pk_from, "/m1", "from shared\n", 0644);
	mklink(p.pk_from, "/m1", "/m2");
	mkfile(p.pk_res, "/m1", "edited\n", 0644);
	mklink(p.pk_res, "/m1", "/m2");
	doc_build(&d, body, 0, 1, records);
	doc_choose(&d, "/m1", ZR_CH_ONTO);
	doc_choose(&d, "/m2", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 2);
	CHECK(st.zs_linked == 0);
	CHECK(!one_object(p.pk_res, "/m1", "/m2"));
	has_bytes(p.pk_res, "/m1", "onto shared\n");
	has_bytes(p.pk_res, "/m2", "from shared\n");
	lstat_at(p.pk_res, "/m1", &s1);
	lstat_at(p.pk_res, "/m2", &s2);
	CHECK(s1.st_nlink == 1);
	CHECK(s2.st_nlink == 1);
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA48: a directory line with a name under it. The directory comes
 * first in the document, so it is made before the child that goes
 * inside it, and both take the chosen side's mode and bytes.
 */
static void
check_choice_dir(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	struct stat sd, sc;
	static const char body[] =
	    "    d/ conflict 1\n"
	    "        c conflict 1\n"
	    "        ..\n";
	static const char records[] =
	    "\n"
	    "conflict 1 contested-home\n"
	    "  why  /d and /d/c were made on both sides\n"
	    "  base ()\n"
	    "  from ({/d}y {/d/c}w)\n"
	    "  onto ({/d}z {/d/c}v)\n";

	pick_init(&p);
	mkdirp(p.pk_onto, "/d", 0700);
	mkfile(p.pk_onto, "/d/c", "onto child\n", 0644);
	mkdirp(p.pk_from, "/d", 0755);
	mkfile(p.pk_from, "/d/c", "from child\n", 0640);
	mkdirp(p.pk_res, "/d", 0700);
	mkfile(p.pk_res, "/d/c", "onto child\n", 0644);
	doc_build(&d, body, 0, 1, records);
	doc_choose(&d, "/d", ZR_CH_FROM);
	doc_choose(&d, "/d/c", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 2);
	lstat_at(p.pk_res, "/d", &sd);
	CHECK(S_ISDIR(sd.st_mode));
	CHECK((sd.st_mode & 07777) == 0755);
	lstat_at(p.pk_res, "/d/c", &sc);
	CHECK((sc.st_mode & 07777) == 0640);
	has_bytes(p.pk_res, "/d/c", "from child\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA50: drift lines, which carry no group. Two of them naming names
 * from holds as one file are still two names of their own here --
 * pooling is a group's, and a drift line is in none -- and a third
 * with keep is not touched.
 */
static void
check_choice_drift(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	struct stat s1, s2;

	pick_init(&p);
	mkfile(p.pk_from, "/a", "from bytes\n", 0644);
	mklink(p.pk_from, "/a", "/b");
	mkfile(p.pk_onto, "/a", "onto a\n", 0644);
	mkfile(p.pk_onto, "/b", "onto b\n", 0644);
	mkfile(p.pk_onto, "/c", "onto c\n", 0644);
	mkfile(p.pk_res, "/a", "edited a\n", 0644);
	mkfile(p.pk_res, "/b", "edited b\n", 0644);
	mkfile(p.pk_res, "/c", "edited c\n", 0644);
	doc_build(&d, "", 0, 0, "");
	doc_drift(&d, "/a", ZR_CH_FROM);
	doc_drift(&d, "/b", ZR_CH_FROM);
	doc_drift(&d, "/c", ZR_CH_KEEP);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 2);
	CHECK(st.zs_linked == 0);
	CHECK(st.zs_kept == 1);
	has_bytes(p.pk_res, "/a", "from bytes\n");
	has_bytes(p.pk_res, "/b", "from bytes\n");
	has_bytes(p.pk_res, "/c", "edited c\n");
	CHECK(!one_object(p.pk_res, "/a", "/b"));
	lstat_at(p.pk_res, "/a", &s1);
	lstat_at(p.pk_res, "/b", &s2);
	CHECK(s1.st_nlink == 1);
	CHECK(s2.st_nlink == 1);
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA53: the removal a conflict blocked. The manifest removes /d, and
 * applying1 could not, because /d/c was conflicted; the choice takes
 * /d/c away, and the directory goes with it after the choices.
 */
static void
check_choice_frees_dir(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	static const char body[] =
	    "    d/ rm\n"
	    "        c conflict 1\n"
	    "        ..\n";
	static const char records[] =
	    "\n"
	    "conflict 1 orphaned-add\n"
	    "  why  /d/c is an add whose anchors the other side deleted\n"
	    "  base ()\n"
	    "  from ()\n"
	    "  onto ({/d/c}z)\n";

	pick_init(&p);
	mkdirp(p.pk_onto, "/d", 0755);
	mkfile(p.pk_onto, "/d/c", "onto child\n", 0644);
	mkdirp(p.pk_res, "/d", 0755);
	mkfile(p.pk_res, "/d/c", "onto child\n", 0644);
	doc_build(&d, body, 1, 1, records);
	doc_choose(&d, "/d/c", ZR_CH_FROM);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_dropped == 1);
	CHECK(st.zs_latedirs == 1);
	CHECK(absent(p.pk_res, "/d/c"));
	CHECK(absent(p.pk_res, "/d"));
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA54: the same removal, and a choice that cements a name under it.
 * The directory stays, which is the blocked-rm rule's other half,
 * and the removal is counted as one more thing left alone.
 */
static void
check_choice_holds_dir(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	static const char body[] =
	    "    d/ rm\n"
	    "        c conflict 1\n"
	    "        ..\n";
	static const char records[] =
	    "\n"
	    "conflict 1 orphaned-add\n"
	    "  why  /d/c is an add whose anchors the other side deleted\n"
	    "  base ()\n"
	    "  from ()\n"
	    "  onto ({/d/c}z)\n";

	pick_init(&p);
	mkdirp(p.pk_onto, "/d", 0755);
	mkfile(p.pk_onto, "/d/c", "onto child\n", 0644);
	mkdirp(p.pk_res, "/d", 0755);
	mkfile(p.pk_res, "/d/c", "the hand merge\n", 0644);
	doc_build(&d, body, 1, 1, records);
	doc_choose(&d, "/d/c", ZR_CH_KEEP);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_kept == 1);
	CHECK(st.zs_latedirs == 0);
	CHECK(st.zs_skipped == 1);
	has_bytes(p.pk_res, "/d/c", "the hand merge\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA64: a directory line whose chosen side has no such directory
 * while a line under it says keep. The removal cannot be made -- the
 * kept name holds the directory open -- and the pre-scan is what
 * knows it, so nothing is asked of the disk: the line is skipped and
 * counted, the run does not die on an ENOTEMPTY, and the check after
 * the choices is what judges it.
 */
static void
check_choice_blocked_dir(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;

	pick_init(&p);
	mkdirp(p.pk_res, "/e", 0755);
	mkfile(p.pk_res, "/e/f", "the person's\n", 0644);
	doc_build(&d, "", 0, 0, "");
	doc_drift_dir(&d, "/e", ZR_CH_ONTO);
	doc_drift(&d, "/e/f", ZR_CH_KEEP);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_kept == 1);
	CHECK(st.zs_dropped == 0);
	CHECK(st.zs_skipped == 1);
	CHECK(st.zs_line == ZR_LINE_NONE);
	CHECK(!absent(p.pk_res, "/e"));
	has_bytes(p.pk_res, "/e/f", "the person's\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);

	/* and with nothing kept under it the same directory goes */
	pick_init(&p);
	mkdirp(p.pk_res, "/e", 0755);
	mkfile(p.pk_res, "/e/f", "a stray\n", 0644);
	doc_build(&d, "", 0, 0, "");
	doc_drift_dir(&d, "/e", ZR_CH_ONTO);
	doc_drift(&d, "/e/f", ZR_CH_ONTO);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_dropped == 2);
	CHECK(absent(p.pk_res, "/e"));
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA65: one document with a choice of each kind on it. keep leaves
 * the name exactly as the person left it, onto makes it onto's
 * object and from makes it from's, and the three do not disturb one
 * another.
 */
static void
check_choice_each(void)
{
	struct zr_apply_stats st;
	struct stat before, after;
	struct pick p;
	struct doc d;
	static const char body[] =
	    "    k conflict 1\n"
	    "    o conflict 2\n"
	    "    r conflict 3\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /k changed on both sides\n"
	    "  base ()\n"
	    "  from ({/k}a)\n"
	    "  onto ({/k}b)\n"
	    "conflict 2 changed-both\n"
	    "  why  /o changed on both sides\n"
	    "  base ()\n"
	    "  from ({/o}c)\n"
	    "  onto ({/o}d)\n"
	    "conflict 3 changed-both\n"
	    "  why  /r changed on both sides\n"
	    "  base ()\n"
	    "  from ({/r}e)\n"
	    "  onto ({/r}f)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/k", "onto k\n", 0644);
	mkfile(p.pk_onto, "/o", "onto o\n", 0644);
	mkfile(p.pk_onto, "/r", "onto r\n", 0644);
	mkfile(p.pk_from, "/k", "from k\n", 0644);
	mkfile(p.pk_from, "/o", "from o\n", 0644);
	mkfile(p.pk_from, "/r", "from r\n", 0644);
	mkfile(p.pk_res, "/k", "the hand merge\n", 0644);
	mkfile(p.pk_res, "/o", "the hand merge\n", 0644);
	mkfile(p.pk_res, "/r", "the hand merge\n", 0644);
	doc_build(&d, body, 0, 3, records);
	doc_choose(&d, "/k", ZR_CH_KEEP);
	doc_choose(&d, "/o", ZR_CH_ONTO);
	doc_choose(&d, "/r", ZR_CH_FROM);
	lstat_at(p.pk_res, "/k", &before);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_kept == 1);
	CHECK(st.zs_made == 2);
	CHECK(st.zs_dropped == 0);
	lstat_at(p.pk_res, "/k", &after);
	CHECK(before.st_ino == after.st_ino);
	has_bytes(p.pk_res, "/k", "the hand merge\n");
	has_bytes(p.pk_res, "/o", "onto o\n");
	has_bytes(p.pk_res, "/r", "from r\n");
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA66: a conflict line for a name the manifest never marked. It is
 * the person's own instruction and is carried out like a drift line
 * with that choice; its group number is not read, so a name onto
 * pools with the marked name of "that group" is still its own object
 * here.
 */
static void
check_choice_unmarked(void)
{
	struct zr_apply_stats st;
	struct pick p;
	struct doc d;
	static const char body[] = "    x conflict 1\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /x changed on both sides\n"
	    "  base ()\n"
	    "  from ({/x}y)\n"
	    "  onto ({/x,/z}z)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/x", "one object\n", 0644);
	mklink(p.pk_onto, "/x", "/z");
	mkfile(p.pk_from, "/x", "from bytes\n", 0644);
	mkfile(p.pk_res, "/x", "the hand merge\n", 0644);
	mkfile(p.pk_res, "/z", "a stray\n", 0644);
	doc_build(&d, body, 0, 1, records);
	doc_choose(&d, "/x", ZR_CH_ONTO);
	doc_add_conflict(&d, "/z", 1, ZR_CH_ONTO);
	pick_ok(&p, &d, &st);
	CHECK(st.zs_made == 2);
	CHECK(st.zs_linked == 0);
	has_bytes(p.pk_res, "/x", "one object\n");
	has_bytes(p.pk_res, "/z", "one object\n");
	CHECK(!one_object(p.pk_res, "/x", "/z"));
	pick_stable(&p, &d);
	doc_fini(&d);
	pick_fini(&p);
}

/*
 * ZA55: a document with a choice still "-" is refused, and refused
 * before anything is written: the name that was answered is left as
 * the result had it.
 */
static void
check_choice_unanswered(void)
{
	struct pick p;
	struct doc d;
	static const char body[] =
	    "    a conflict 1\n"
	    "    u conflict 2\n";
	static const char records[] =
	    "\n"
	    "conflict 1 changed-both\n"
	    "  why  /a changed on both sides\n"
	    "  base ()\n"
	    "  from ({/a}y)\n"
	    "  onto ({/a}z)\n"
	    "conflict 2 changed-both\n"
	    "  why  /u changed on both sides\n"
	    "  base ()\n"
	    "  from ({/u}y)\n"
	    "  onto ({/u}z)\n";

	pick_init(&p);
	mkfile(p.pk_onto, "/a", "onto a\n", 0644);
	mkfile(p.pk_onto, "/u", "onto u\n", 0644);
	mkfile(p.pk_from, "/a", "from a\n", 0644);
	mkfile(p.pk_from, "/u", "from u\n", 0644);
	mkfile(p.pk_res, "/a", "edited\n", 0644);
	mkfile(p.pk_res, "/u", "edited\n", 0644);
	doc_build(&d, body, 0, 2, records);
	doc_choose(&d, "/a", ZR_CH_ONTO);
	pick_refused(&p, &d, "/u");
	has_bytes(p.pk_res, "/a", "edited\n");
	has_bytes(p.pk_res, "/u", "edited\n");
	doc_fini(&d);
	pick_fini(&p);
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
	/*
	 * The posix form's header: the three directories as given and
	 * "-" for the rest of the run (v4-manifest.md, section 6).
	 */
	memset(&hdr, 0, sizeof (hdr));
	hdr.result = "-";
	hdr.form = ZR_HFORM_POSIX;
	hdr.base = w->w_base;
	hdr.from = w->w_from;
	hdr.onto = w->w_onto;
	hdr.made = "-";
	hdr.tag = "-";
	hdr.take = "-";
	hdr.written = "-";
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
	if (zr_apply_with(&w.w_p, w.w_onto, &w.w_wf, &w.w_wo, NULL, &st, err,
	    sizeof (err)) != 0)
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
	char probe[256];

	tmp_template(probe, sizeof (probe), "zrapply.XXXXXX");
	CHECK(mkdtemp(probe) != NULL);
	check_probe(probe);
	rmtree(probe);

	check_cp_file();
	check_dup_severs();
	check_cp_symlink();
	check_cp_dir();
	check_cp_fifo();
	check_cp_sock();
	check_repair_sock();
	check_kept_walk();
	check_ln_replace();
	check_write_links();
	check_rm_deep();
	check_type_change();
	check_mtime();
#ifdef TESTFLAG
	check_flag();
	check_rm_immutable();
	check_write_immutable();
	check_attrs_immutable();
	check_write_append_only();
#endif
	check_ln_missing();
	check_ln_over_dir();
	check_rm_nonempty();
	check_escape();
	check_write_symlink_out();

	check_choice_keep();
	check_choice_onto();
	check_choice_from();
	check_choice_gone();
	check_choice_pool();
	check_choice_mixed();
	check_choice_dir();
	check_choice_drift();
	check_choice_frees_dir();
	check_choice_holds_dir();
	check_choice_unanswered();
	check_choice_blocked_dir();
	check_choice_each();
	check_choice_unmarked();

	printf("check_apply: %d checks passed\n", checks);
	return (0);
}
