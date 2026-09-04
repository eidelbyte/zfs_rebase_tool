/*
 * The content oracle's tests: small trees built as directories,
 * walked into one shared name table and handed to the oracle, then
 * read back as handles and as a count of the bytes it took to reach
 * them. Cells ZC1, ZC3 to ZC8, ZC10, ZC12 to ZC20, ZC23, ZC24 and
 * ZC26 to ZC37. ZC20 is the oracle's own half of the fast path, the
 * word taken without a read; ZC26 onwards are where that word comes
 * from, the pruning rule over one tree walked twice.
 * ZC2 (uid, gid, flags) and ZC11 (device numbers)
 * want root, so they stay deferred to the box probe with ZC9, the
 * ACL, whose two models differ; ZC21 is the driver's and ZC22 the
 * decision's.
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
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/xattr.h>
#elif defined(__FreeBSD__)
#include <sys/extattr.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif

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
#define	WALK_MIN	16

#define	XA1		"user.zra"
#define	XA2		"user.zrb"

#define	MEG		((size_t)1024 * 1024)
#define	CHUNK		((size_t)128 * 1024)	/* the oracle's buffer */
#define	NOFLIP		((size_t)-1)
#define	HOLE		4096			/* the sparse file's size */
#define	MTIME		1000000000		/* an mtime long past */

/*
 * What the main trees are worth reading: /eq on both of its pairs,
 * /mt, /hole, /xord and the /pq pool pair, which is read once for
 * the two names it shares. Every other pair is settled before a
 * byte, or never compared at all.
 */
#define	MAIN_BYTES	((uint64_t)(2 * 2 * 6 + 2 * 6 + 2 * HOLE + 2 * 2 + \
			2 * 10))

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

/* Every name a scan found under one root, parents before children. */
struct scan {
	char	**s_path;
	int	*s_isdir;
	int	s_n;
	int	s_cap;
};

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

static void
scan_push(struct scan *s, const char *rel, int isdir)
{
	char **np;
	int *ni, cap;

	if (s->s_n == s->s_cap) {
		cap = s->s_cap == 0 ? WALK_MIN : s->s_cap * 2;
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

/* A directory of exactly mode, whatever the umask would have said. */
static void
mkd(int dfd, const char *name, mode_t mode)
{
	CHECK(mkdirat(dfd, name, mode) == 0);
	CHECK(fchmodat(dfd, name, mode, 0) == 0);
}

static void
wr(int dfd, const char *name, const void *data, size_t len, mode_t mode)
{
	int fd;

	fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL, mode);
	CHECK(fd >= 0);
	if (len > 0)
		CHECK(write(fd, data, len) == (ssize_t)len);
	CHECK(fchmod(fd, mode) == 0);
	CHECK(close(fd) == 0);
}

static void
wrs(int dfd, const char *name, const char *s, mode_t mode)
{
	wr(dfd, name, s, strlen(s), mode);
}

/* ZC19: the same length, one as a hole and one as written zeros. */
static void
holef(int dfd, const char *name, off_t len)
{
	int fd;

	fd = openat(dfd, name, O_WRONLY | O_CREAT | O_EXCL, 0644);
	CHECK(fd >= 0);
	CHECK(ftruncate(fd, len) == 0);
	CHECK(fchmod(fd, 0644) == 0);
	CHECK(close(fd) == 0);
}

static void
zerof(int dfd, const char *name, size_t len)
{
	char z[HOLE];

	CHECK(len <= sizeof (z));
	memset(z, 0, len);
	wr(dfd, name, z, len, 0644);
}

/*
 * One extended attribute in the platform's user namespace, named as
 * the walk reports it: check_walk.c sets them the same way.
 */
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

/* ZC15: a time the oracle must not look at. */
static void
setmtime(int dfd, const char *name, time_t sec)
{
	struct timespec ts[2];

	ts[0].tv_sec = sec;
	ts[0].tv_nsec = 0;
	ts[1].tv_sec = sec;
	ts[1].tv_nsec = 0;
	CHECK(utimensat(dfd, name, ts, 0) == 0);
}

/* Three trees under one temporary directory, and the oracle over them. */
struct trees {
	char			t_root[PATHMAX];
	char			t_dir[3][PATHMAX];
	int			t_fd[3];
	struct zr_names		*t_ns;
	struct zr_walk		t_w[3];
	struct zr_oracle	*t_o;
};

static void
trees_open(struct trees *t, char *tmpl)
{
	static const char *const leaf[3] = { "/b", "/f", "/o" };
	int i;

	memset(t, 0, sizeof (*t));
	CHECK(mkdtemp(tmpl) != NULL);
	join(t->t_root, sizeof (t->t_root), tmpl, "");
	for (i = 0; i < 3; i++) {
		join(t->t_dir[i], sizeof (t->t_dir[i]), t->t_root, leaf[i]);
		CHECK(mkdir(t->t_dir[i], 0755) == 0);
		CHECK(chmod(t->t_dir[i], 0755) == 0);
		t->t_fd[i] = open(t->t_dir[i], O_RDONLY | O_DIRECTORY);
		CHECK(t->t_fd[i] >= 0);
	}
}

/* The three built trees walked into one table, and an oracle over them. */
static void
trees_walk(struct trees *t)
{
	char err[256];
	int i;

	for (i = 0; i < 3; i++) {
		CHECK(close(t->t_fd[i]) == 0);
		t->t_fd[i] = -1;
	}
	t->t_ns = zr_names_create();
	CHECK(t->t_ns != NULL);
	for (i = 0; i < 3; i++) {
		err[0] = '\0';
		if (zr_walk(t->t_dir[i], t->t_ns, &t->t_w[i], err,
		    sizeof (err)) != 0)
			printf("  walk: %s\n", err);
		CHECK(err[0] == '\0');
	}
	CHECK(zr_oracle_init(&t->t_o, &t->t_w[0], &t->t_w[1],
	    &t->t_w[2]) == 0);
}

static void
trees_close(struct trees *t)
{
	int i;

	zr_oracle_fini(t->t_o);
	for (i = 0; i < 3; i++)
		zr_walk_fini(&t->t_w[i]);
	zr_names_destroy(t->t_ns);
	rmtree(t->t_root);
}

static zr_pool_t
poolof(const struct trees *t, int which, const char *path)
{
	zr_pool_t p;
	zr_name_t nm;

	nm = zr_names_lookup(t->t_ns, path, strlen(path));
	if (nm == ZR_NAME_NONE)
		printf("  no such name: %s\n", path);
	CHECK(nm != ZR_NAME_NONE);
	p = zr_tree_pool(&t->t_w[which].zw_tree, nm);
	if (p == ZR_POOL_NONE)
		printf("  no pool for: %s\n", path);
	CHECK(p != ZR_POOL_NONE);
	return (p);
}

static uint32_t
hand(const struct trees *t, int which, const char *path)
{
	return (t->t_w[which].zw_tree.zt_pools[poolof(t, which,
	    path)].zp_content);
}

/* How many pools of the three trees carry one handle. */
static uint32_t
handle_uses(const struct trees *t, uint32_t h)
{
	uint32_t i, n;
	int k;

	n = 0;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < t->t_w[k].zw_tree.zt_npools; i++) {
			if (t->t_w[k].zw_tree.zt_pools[i].zp_content == h)
				n++;
		}
	}
	return (n);
}

/*
 * The handles are dense from 0 and none of them is ZR_CONTENT_NONE:
 * every value up to the largest is carried by some pool.
 */
static void
check_dense(const struct trees *t)
{
	unsigned char *seen;
	uint32_t h, max, i, n;
	int k;

	max = 0;
	for (k = 0; k < 3; k++) {
		for (i = 0; i < t->t_w[k].zw_tree.zt_npools; i++) {
			h = t->t_w[k].zw_tree.zt_pools[i].zp_content;
			CHECK(h != ZR_CONTENT_NONE);
			if (h > max)
				max = h;
		}
	}
	seen = calloc((size_t)max + 1, 1);
	CHECK(seen != NULL);
	for (k = 0; k < 3; k++) {
		for (i = 0; i < t->t_w[k].zw_tree.zt_npools; i++)
			seen[t->t_w[k].zw_tree.zt_pools[i].zp_content] = 1;
	}
	n = 0;
	for (i = 0; i <= max; i++)
		n += seen[i];
	CHECK(n == max + 1);
	free(seen);
}

/* One name per cell, laid out so that no two of them share a pool. */
static void
build_main(struct trees *t)
{
	char full[PATHMAX];
	int b, f, o, d;

	b = t->t_fd[0];
	f = t->t_fd[1];
	o = t->t_fd[2];

	/* ZC5: the same bytes and attributes, in all three trees */
	wrs(b, "eq", "hello\n", 0644);
	wrs(f, "eq", "hello\n", 0644);
	wrs(o, "eq", "hello\n", 0644);

	/* ZC4: a size apart, which is settled before a byte is read */
	wrs(b, "size", "abc\n", 0644);
	wrs(f, "size", "abcd\n", 0644);

	/* ZC17: two files with no bytes at all */
	wrs(b, "empty", "", 0644);
	wrs(f, "empty", "", 0644);

	/* ZC1: the same bytes, and one mode bit apart */
	wrs(b, "mode", "same\n", 0644);
	wrs(f, "mode", "same\n", 0600);

	/* ZC15: the same bytes, written at different times */
	wrs(b, "mt", "mtime\n", 0644);
	wrs(f, "mt", "mtime\n", 0644);
	setmtime(f, "mt", MTIME);

	/* ZC19: a hole against the same length written out as zeros */
	holef(b, "hole", HOLE);
	zerof(f, "hole", HOLE);

	/* ZC7: an attribute one side has and the other does not */
	wrs(b, "xone", "x\n", 0644);
	wrs(f, "xone", "x\n", 0644);
	join(full, sizeof (full), t->t_dir[0], "/xone");
	CHECK(setx(full, XA1, "1", 1) == 0);

	/* ZC6: one attribute, two values */
	wrs(b, "xval", "x\n", 0644);
	wrs(f, "xval", "x\n", 0644);
	join(full, sizeof (full), t->t_dir[0], "/xval");
	CHECK(setx(full, XA1, "1", 1) == 0);
	join(full, sizeof (full), t->t_dir[1], "/xval");
	CHECK(setx(full, XA1, "2", 1) == 0);

	/* ZC8: the same two attributes, set in the other order */
	wrs(b, "xord", "x\n", 0644);
	wrs(f, "xord", "x\n", 0644);
	join(full, sizeof (full), t->t_dir[0], "/xord");
	CHECK(setx(full, XA1, "1", 1) == 0);
	CHECK(setx(full, XA2, "2", 1) == 0);
	join(full, sizeof (full), t->t_dir[1], "/xord");
	CHECK(setx(full, XA2, "2", 1) == 0);
	CHECK(setx(full, XA1, "1", 1) == 0);

	/* ZC10: one target, then two */
	CHECK(symlinkat("target/one", b, "sym-same") == 0);
	CHECK(symlinkat("target/one", f, "sym-same") == 0);
	CHECK(symlinkat("target/one", b, "sym-diff") == 0);
	CHECK(symlinkat("target/two", f, "sym-diff") == 0);

	/*
	 * ZC3: a directory against a file at one name. The type is
	 * the first thing compared and the last thing that could
	 * make these two equal: za_mode carries S_IFMT, so the
	 * attributes would have parted them anyway.
	 */
	mkd(b, "tf", 0755);
	wrs(f, "tf", "tf\n", 0644);

	/* ZC12: two directories with nothing but their attributes */
	mkd(b, "d-eq", 0755);
	mkd(f, "d-eq", 0755);

	/* ZC13: entries are pools of their own, so not content */
	mkd(b, "d-ent", 0755);
	mkd(f, "d-ent", 0755);
	d = openat(b, "d-ent", O_RDONLY | O_DIRECTORY);
	CHECK(d >= 0);
	wrs(d, "x", "e\n", 0644);
	CHECK(close(d) == 0);
	d = openat(f, "d-ent", O_RDONLY | O_DIRECTORY);
	CHECK(d >= 0);
	wrs(d, "y", "e\n", 0644);
	CHECK(close(d) == 0);

	/* ZC14: a directory's mode is its content */
	mkd(b, "d-mode", 0755);
	mkd(f, "d-mode", 0700);

	/* ZC20: bytes that differ, under a caller who says they do not */
	wrs(b, "unch", "one\n", 0644);
	wrs(f, "unch", "two\n", 0644);

	/*
	 * A pool pair that shares two names and differs: the second
	 * name must not read it a second time.
	 */
	wrs(b, "pq1", "differs-a\n", 0644);
	CHECK(linkat(b, "pq1", b, "pq2", 0) == 0);
	wrs(f, "pq1", "differs-b\n", 0644);
	CHECK(linkat(f, "pq1", f, "pq2", 0) == 0);

	/* a pool no other tree shares a name with, on either side */
	wrs(b, "lonely", "lonely\n", 0644);
	wrs(o, "only-onto", "onto\n", 0644);
}

static void
check_main(void)
{
	char tmpl[256];
	char err[256];
	struct trees t;
	uint32_t h;

	tmp_template(tmpl, sizeof (tmpl), "zryellow.XXXXXX");
	trees_open(&t, tmpl);
	build_main(&t);
	trees_walk(&t);

	/* ZC20: the from pool is the base pool, on the caller's word */
	CHECK(zr_oracle_unchanged(t.t_o, 1, poolof(&t, 1, "/unch"),
	    poolof(&t, 0, "/unch")) == 0);
	CHECK(zr_oracle_unchanged(t.t_o, 0, 0, 0) == -1);
	CHECK(zr_oracle_unchanged(t.t_o, 1, t.t_w[1].zw_tree.zt_npools,
	    0) == -1);

	err[0] = 'x';
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');

	CHECK(hand(&t, 0, "/eq") == hand(&t, 1, "/eq"));	/* ZC5 */
	CHECK(hand(&t, 0, "/eq") == hand(&t, 2, "/eq"));
	CHECK(hand(&t, 0, "/size") != hand(&t, 1, "/size"));	/* ZC4 */
	CHECK(hand(&t, 0, "/empty") == hand(&t, 1, "/empty"));	/* ZC17 */
	CHECK(hand(&t, 0, "/mode") != hand(&t, 1, "/mode"));	/* ZC1 */
	CHECK(hand(&t, 0, "/mt") == hand(&t, 1, "/mt"));	/* ZC15 */
	CHECK(hand(&t, 0, "/hole") == hand(&t, 1, "/hole"));	/* ZC19 */
	CHECK(hand(&t, 0, "/xone") != hand(&t, 1, "/xone"));	/* ZC7 */
	CHECK(hand(&t, 0, "/xval") != hand(&t, 1, "/xval"));	/* ZC6 */
	CHECK(hand(&t, 0, "/xord") == hand(&t, 1, "/xord"));	/* ZC8 */
	CHECK(hand(&t, 0, "/sym-same") == hand(&t, 1, "/sym-same"));
	CHECK(hand(&t, 0, "/sym-diff") != hand(&t, 1, "/sym-diff"));
	CHECK(hand(&t, 0, "/tf") != hand(&t, 1, "/tf"));	/* ZC3 */
	CHECK(hand(&t, 0, "/d-eq") == hand(&t, 1, "/d-eq"));	/* ZC12 */
	CHECK(hand(&t, 0, "/d-ent") == hand(&t, 1, "/d-ent"));	/* ZC13 */
	CHECK(hand(&t, 0, "/d-mode") != hand(&t, 1, "/d-mode"));
	CHECK(hand(&t, 0, "/unch") == hand(&t, 1, "/unch"));	/* ZC20 */

	/* one pool on each side, two shared names, read once */
	CHECK(poolof(&t, 0, "/pq1") == poolof(&t, 0, "/pq2"));
	CHECK(poolof(&t, 1, "/pq1") == poolof(&t, 1, "/pq2"));
	CHECK(hand(&t, 0, "/pq1") != hand(&t, 1, "/pq1"));
	CHECK(hand(&t, 0, "/pq1") == hand(&t, 0, "/pq2"));

	/* the two roots and onto's are one class, all three empty */
	CHECK(hand(&t, 0, "/") == hand(&t, 1, "/"));
	CHECK(hand(&t, 0, "/") == hand(&t, 2, "/"));

	/* a pool nothing shares a name with holds its handle alone */
	h = hand(&t, 0, "/lonely");
	CHECK(handle_uses(&t, h) == 1);
	CHECK(handle_uses(&t, hand(&t, 2, "/only-onto")) == 1);
	CHECK(handle_uses(&t, hand(&t, 0, "/eq")) == 3);

	CHECK(zr_oracle_bytes_read(t.t_o) == MAIN_BYTES);
	check_dense(&t);
	trees_close(&t);
}

/*
 * ZC16 and ZC18: one file of len bytes in base and in from, apart at
 * the byte flip or nowhere at all. The trees hold nothing else, so
 * the bytes read are exactly this one comparison's.
 */
static void
check_big(size_t len, size_t flip, int equal, uint64_t bytes)
{
	char tmpl[256];
	char err[256];
	struct trees t;
	unsigned char *buf;
	size_t i;

	buf = malloc(len);
	CHECK(buf != NULL);
	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)(i % 251);
	tmp_template(tmpl, sizeof (tmpl), "zryellowb.XXXXXX");
	trees_open(&t, tmpl);
	wr(t.t_fd[0], "big", buf, len, 0644);
	if (flip != NOFLIP)
		buf[flip] = (unsigned char)(buf[flip] ^ 0xff);
	wr(t.t_fd[1], "big", buf, len, 0644);
	free(buf);
	trees_walk(&t);
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);
	CHECK((hand(&t, 0, "/big") == hand(&t, 1, "/big")) == equal);
	CHECK(zr_oracle_bytes_read(t.t_o) == bytes);
	check_dense(&t);
	trees_close(&t);
}

/*
 * ZC23: base holds the same bytes under two names of two pools, from
 * holds them under two names of one. Neither base pool is ever
 * compared with the other -- they are of one tree, and the oracle
 * compares across trees -- but both are compared with from's, so all
 * three end in one class.
 */
static void
check_trans(void)
{
	char tmpl[256];
	char err[256];
	struct trees t;

	tmp_template(tmpl, sizeof (tmpl), "zryellowt.XXXXXX");
	trees_open(&t, tmpl);
	wrs(t.t_fd[0], "a", "shared\n", 0644);
	wrs(t.t_fd[0], "b", "shared\n", 0644);
	wrs(t.t_fd[1], "a", "shared\n", 0644);
	CHECK(linkat(t.t_fd[1], "a", t.t_fd[1], "b", 0) == 0);
	trees_walk(&t);
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);

	CHECK(poolof(&t, 0, "/a") != poolof(&t, 0, "/b"));
	CHECK(poolof(&t, 1, "/a") == poolof(&t, 1, "/b"));
	CHECK(hand(&t, 0, "/a") == hand(&t, 0, "/b"));
	CHECK(hand(&t, 0, "/a") == hand(&t, 1, "/a"));
	CHECK(handle_uses(&t, hand(&t, 0, "/a")) == 3);

	/* seven bytes from each side of each of the two pairs */
	CHECK(zr_oracle_bytes_read(t.t_o) == 4 * 7);
	check_dense(&t);
	trees_close(&t);
}

/*
 * ZC24: a file the oracle may not read is an error naming it, never
 * a verdict. Only the owner is refused here, so root proves nothing
 * and skips it.
 */
static void
check_readerr(void)
{
	char tmpl[256];
	char err[256];
	struct trees t;

	if (geteuid() == 0)
		return;
	tmp_template(tmpl, sizeof (tmpl), "zryellowe.XXXXXX");
	trees_open(&t, tmpl);
	wrs(t.t_fd[0], "blocked", "abc\n", 0000);
	wrs(t.t_fd[1], "blocked", "abd\n", 0000);
	trees_walk(&t);
	err[0] = '\0';
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == -1);
	CHECK(strstr(err, "/blocked") != NULL);
	CHECK(strstr(err, "base") != NULL);
	CHECK(strstr(err, strerror(EACCES)) != NULL);
	CHECK(zr_oracle_bytes_read(t.t_o) == 0);
	trees_close(&t);
}


/*
 * ---------------------------------------------------------------
 * The pruning, ZC26 to ZC37. One directory is walked as base and
 * then again as each side, because the rule is about one object
 * seen twice: the same object number, the same generation number,
 * the same change time, the same link count, type and names. Only
 * one directory read twice offers that on a filesystem a test may
 * write to; on ZFS a snapshot and a clone of it offer it, which is
 * the case the box replays.
 * ---------------------------------------------------------------
 */

/*
 * What the changed case is worth reading, and nothing else is:
 * /linked against base from each side, /touched between the two
 * sides -- base's copy of it is a size apart, which is settled
 * before a byte -- and /added, which only the two sides have. Every
 * pool that pruned is unread.
 *
 * The three walks are of one directory, so a file's bytes are
 * whatever is on disk when the oracle reads them, while its
 * attributes are the ones its own walk captured. That is where the
 * base of this test differs from a real base, which is a snapshot
 * and cannot move; nothing below leans on the bytes of a pool whose
 * attributes changed.
 */
#define	PRUNE_BYTES	((uint64_t)(2 * 2 * 5 + 2 * 5 + 2 * 4))
#define	PRUNE_POOLS	8	/* what build_prune leaves behind */

/*
 * The tree the pruning walks: one of every shape the rule tells
 * apart, so that a pool which must not prune is never the only pool
 * in the tree.
 */
static void
build_prune(int dfd)
{
	int d;

	wrs(dfd, "keep", "keep\n", 0644);
	wrs(dfd, "touched", "aaa\n", 0644);
	wrs(dfd, "linked", "link\n", 0644);
	wrs(dfd, "h1", "hh\n", 0644);
	CHECK(linkat(dfd, "h1", dfd, "h2", 0) == 0);
	CHECK(symlinkat("target", dfd, "sym") == 0);
	mkd(dfd, "sub", 0755);
	d = openat(dfd, "sub", O_RDONLY | O_DIRECTORY);
	CHECK(d >= 0);
	wrs(d, "deep", "deep\n", 0644);
	CHECK(close(d) == 0);
}

/* One more walk of the one directory, into the tree at which. */
static void
prune_walk(struct trees *t, int which)
{
	char err[256];

	err[0] = '\0';
	if (zr_walk(t->t_dir[0], t->t_ns, &t->t_w[which], err,
	    sizeof (err)) != 0)
		printf("  walk: %s\n", err);
	CHECK(err[0] == '\0');
}

/*
 * The tree built and walked once, as base. The caller changes what
 * it likes and walks the two sides itself, which is the whole point:
 * the change has to fall between the two walks.
 */
static void
prune_open(struct trees *t, char *tmpl)
{
	int i;

	trees_open(t, tmpl);
	build_prune(t->t_fd[0]);
	for (i = 0; i < 3; i++) {
		CHECK(close(t->t_fd[i]) == 0);
		t->t_fd[i] = -1;
	}
	t->t_ns = zr_names_create();
	CHECK(t->t_ns != NULL);
	prune_walk(t, 0);
}

/* The two sides walked, and the oracle over the three. */
static void
prune_sides(struct trees *t)
{
	prune_walk(t, 1);
	prune_walk(t, 2);
	CHECK(zr_oracle_init(&t->t_o, &t->t_w[0], &t->t_w[1],
	    &t->t_w[2]) == 0);
}

/* One pair asked of the oracle: 1 equal, 0 different, never -1. */
static int
pair(struct trees *t, int ta, const char *pa, int tb, const char *pb)
{
	char err[256];
	int rc;

	err[0] = '\0';
	rc = zr_oracle_equal(t->t_o, ta, poolof(t, ta, pa), tb,
	    poolof(t, tb, pb), err, sizeof (err));
	if (rc < 0)
		printf("  equal: %s\n", err);
	CHECK(rc >= 0);
	return (rc);
}

static const struct zr_attr *
attrof(const struct trees *t, int which, const char *path)
{
	return (&t->t_w[which].zw_attrs[poolof(t, which, path)]);
}

/*
 * ZC26: nothing changed between the walks, so every pool of both
 * sides prunes and the oracle reads not one byte to hand out its
 * handles. This is the cell that says the fast path runs at all.
 */
static void
check_prune_all(void)
{
	char tmpl[256];
	char err[256];
	struct trees t;
	uint32_t npools, marked, n, k;
	zr_pool_t p0, pk;
	zr_name_t nm;

	tmp_template(tmpl, sizeof (tmpl), "zryellowp.XXXXXX");
	prune_open(&t, tmpl);
	prune_sides(&t);
	npools = t.t_w[0].zw_tree.zt_npools;
	CHECK(npools == PRUNE_POOLS);
	CHECK(t.t_w[1].zw_tree.zt_npools == npools);
	CHECK(t.t_w[2].zw_tree.zt_npools == npools);

	marked = 0;
	CHECK(zr_oracle_prune(t.t_o, 1, &marked) == 0);
	CHECK(marked == npools);
	marked = 0;
	CHECK(zr_oracle_prune(t.t_o, 2, &marked) == 0);
	CHECK(marked == npools);

	/* the arguments the oracle cannot place */
	CHECK(zr_oracle_prune(NULL, 1, &marked) == -1);
	CHECK(zr_oracle_prune(t.t_o, 0, &marked) == -1);
	CHECK(zr_oracle_prune(t.t_o, 3, &marked) == -1);
	CHECK(zr_oracle_prune(t.t_o, 1, NULL) == -1);

	err[0] = 'x';
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');
	CHECK(zr_oracle_bytes_read(t.t_o) == 0);

	/* one class per pool, and every name of it in all three trees */
	n = zr_names_count(t.t_ns);
	for (nm = 0; nm < n; nm++) {
		p0 = zr_tree_pool(&t.t_w[0].zw_tree, nm);
		if (p0 == ZR_POOL_NONE)
			continue;
		for (k = 1; k < 3; k++) {
			pk = zr_tree_pool(&t.t_w[k].zw_tree, nm);
			CHECK(pk != ZR_POOL_NONE);
			CHECK(t.t_w[k].zw_tree.zt_pools[pk].zp_content ==
			    t.t_w[0].zw_tree.zt_pools[p0].zp_content);
		}
	}
	CHECK(handle_uses(&t, hand(&t, 0, "/keep")) == 3);
	CHECK(handle_uses(&t, hand(&t, 0, "/h1")) == 3);
	check_dense(&t);
	trees_close(&t);
}

/*
 * ZC27 to ZC30: three changes between the base walk and the two side
 * walks -- bytes written through one name at the same length, a
 * second name linked onto another object, and an object base never
 * had. Their pools do not prune, nor does the root, which gained two
 * names and moved its own ctime doing it; every other pool does. The
 * handles afterwards are still right: a pruned pool takes base's
 * without a read, the rewritten one does not take it, and the pool
 * that only gained a name is equal by comparison anyway.
 */
static void
check_prune_changed(void)
{
	char tmpl[256];
	char err[256];
	struct trees t;
	uint32_t npools, marked, other;
	zr_name_t nm;
	int d, fd;

	tmp_template(tmpl, sizeof (tmpl), "zryellowc.XXXXXX");
	prune_open(&t, tmpl);
	npools = t.t_w[0].zw_tree.zt_npools;
	CHECK(npools == PRUNE_POOLS);

	d = open(t.t_dir[0], O_RDONLY | O_DIRECTORY);
	CHECK(d >= 0);
	/* ZC27: one byte written through a name, which moves the ctime */
	fd = openat(d, "touched", O_WRONLY | O_APPEND);
	CHECK(fd >= 0);
	CHECK(write(fd, "b", 1) == 1);
	CHECK(close(fd) == 0);
	/* ZC28: a second name, which moves the link count and the ctime */
	CHECK(linkat(d, "linked", d, "linked2", 0) == 0);
	/* ZC30: an object base never had at all */
	wrs(d, "added", "add\n", 0644);
	CHECK(close(d) == 0);

	prune_sides(&t);
	CHECK(t.t_w[1].zw_tree.zt_npools == npools + 1);

	/*
	 * The four that stay out: /touched, /linked, /added and the
	 * root. Both sides are the one tree, so both say the same.
	 */
	marked = 0;
	CHECK(zr_oracle_prune(t.t_o, 1, &marked) == 0);
	CHECK(marked == npools + 1 - 4);
	other = 0;
	CHECK(zr_oracle_prune(t.t_o, 2, &other) == 0);
	CHECK(other == marked);

	/* a pruned pool answers with no read; the two that moved are read */
	CHECK(zr_oracle_bytes_read(t.t_o) == 0);
	CHECK(pair(&t, 0, "/keep", 1, "/keep") == 1);
	CHECK(pair(&t, 0, "/h1", 1, "/h1") == 1);
	CHECK(pair(&t, 0, "/sub/deep", 1, "/sub/deep") == 1);
	CHECK(zr_oracle_bytes_read(t.t_o) == 0);
	CHECK(pair(&t, 0, "/touched", 1, "/touched") == 0);
	CHECK(zr_oracle_bytes_read(t.t_o) == 0);	/* a size apart */
	CHECK(pair(&t, 0, "/linked", 1, "/linked") == 1);
	CHECK(zr_oracle_bytes_read(t.t_o) == 2 * 5);

	/* ZC29: why the root stayed out -- a name went into it */
	CHECK(attrof(&t, 0, "/")->za_ctime.tv_sec !=
	    attrof(&t, 1, "/")->za_ctime.tv_sec ||
	    attrof(&t, 0, "/")->za_ctime.tv_nsec !=
	    attrof(&t, 1, "/")->za_ctime.tv_nsec);
	/* and the two that did stay in are the same object untouched */
	CHECK(attrof(&t, 0, "/keep")->za_ctime.tv_sec ==
	    attrof(&t, 1, "/keep")->za_ctime.tv_sec);
	CHECK(attrof(&t, 0, "/keep")->za_ctime.tv_nsec ==
	    attrof(&t, 1, "/keep")->za_ctime.tv_nsec);
	CHECK(attrof(&t, 0, "/keep")->za_gen ==
	    attrof(&t, 1, "/keep")->za_gen);

	err[0] = 'x';
	CHECK(zr_oracle_assign(t.t_o, err, sizeof (err)) == 0);
	CHECK(err[0] == '\0');

	CHECK(hand(&t, 0, "/keep") == hand(&t, 1, "/keep"));
	CHECK(hand(&t, 0, "/keep") == hand(&t, 2, "/keep"));
	CHECK(hand(&t, 0, "/h1") == hand(&t, 1, "/h1"));
	CHECK(hand(&t, 0, "/sub/deep") == hand(&t, 1, "/sub/deep"));
	CHECK(hand(&t, 0, "/touched") != hand(&t, 1, "/touched"));
	CHECK(hand(&t, 1, "/touched") == hand(&t, 2, "/touched"));
	CHECK(hand(&t, 0, "/linked") == hand(&t, 1, "/linked"));
	CHECK(hand(&t, 0, "/") == hand(&t, 1, "/"));
	CHECK(hand(&t, 1, "/added") == hand(&t, 2, "/added"));

	/* the new name is on the old pool, and base has no such name */
	CHECK(poolof(&t, 1, "/linked") == poolof(&t, 1, "/linked2"));
	nm = zr_names_lookup(t.t_ns, "/linked2", strlen("/linked2"));
	CHECK(nm != ZR_NAME_NONE);
	CHECK(zr_tree_pool(&t.t_w[0].zw_tree, nm) == ZR_POOL_NONE);

	CHECK(zr_oracle_bytes_read(t.t_o) == PRUNE_BYTES);
	check_dense(&t);
	trees_close(&t);
}

/* The conditions of the rule, one per pass of the loop below. */
enum {
	PF_INO,
	PF_GEN,
	PF_SEC,
	PF_NSEC,
	PF_NLINK,
	PF_TYPE,
	PF_NAMES,
	PF_MAP,
	PF_N
};

/*
 * ZC31 to ZC37: the rule condition by condition. No filesystem call
 * moves one of these fields and leaves the rest alone -- a write
 * moves the ctime, a link moves the ctime and the link count, and
 * nothing at all moves an object number or a generation number
 * without making a new object -- so each field is moved here, in the
 * side walk the rule reads, one at a time. Every one of them alone
 * must keep its own pool out of the unchanged set and leave every
 * other pool in it; the other side, untouched, prunes whole through
 * all of it.
 */
static void
check_prune_fields(void)
{
	char tmpl[256];
	struct trees t;
	struct zr_pool *sp, *hp;
	struct zr_attr *sa;
	struct timespec ct;
	uint64_t ino, gen;
	uint32_t npools, marked, nlink, nnames;
	zr_type_t type;
	zr_name_t keep, was;
	int i;

	tmp_template(tmpl, sizeof (tmpl), "zryellowf.XXXXXX");
	prune_open(&t, tmpl);
	prune_sides(&t);
	zr_oracle_fini(t.t_o);
	t.t_o = NULL;
	npools = t.t_w[1].zw_tree.zt_npools;
	sp = &t.t_w[1].zw_tree.zt_pools[poolof(&t, 1, "/keep")];
	sa = &t.t_w[1].zw_attrs[poolof(&t, 1, "/keep")];
	hp = &t.t_w[1].zw_tree.zt_pools[poolof(&t, 1, "/h1")];
	CHECK(hp->zp_nnames == 2);
	keep = zr_names_lookup(t.t_ns, "/keep", strlen("/keep"));
	CHECK(keep != ZR_NAME_NONE);
	ino = sp->zp_ino;
	gen = sa->za_gen;
	ct = sa->za_ctime;
	nlink = sp->zp_nlink;
	type = sp->zp_type;
	nnames = hp->zp_nnames;
	was = hp->zp_names[1];
	for (i = 0; i < PF_N; i++) {
		switch (i) {
		case PF_INO:
			sp->zp_ino = ino ^ 1;
			break;
		case PF_GEN:
			sa->za_gen = gen ^ 1;
			break;
		case PF_SEC:
			sa->za_ctime.tv_sec = ct.tv_sec + 1;
			break;
		case PF_NSEC:
			sa->za_ctime.tv_nsec = ct.tv_nsec + 1;
			break;
		case PF_NLINK:
			sp->zp_nlink = nlink + 1;
			break;
		case PF_TYPE:
			sp->zp_type = ZR_T_FIFO;
			break;
		case PF_NAMES:
			hp->zp_nnames = nnames - 1;
			break;
		case PF_MAP:
			hp->zp_names[1] = keep;
			break;
		}
		CHECK(zr_oracle_init(&t.t_o, &t.t_w[0], &t.t_w[1],
		    &t.t_w[2]) == 0);
		marked = 0;
		CHECK(zr_oracle_prune(t.t_o, 1, &marked) == 0);
		CHECK(marked == npools - 1);
		marked = 0;
		CHECK(zr_oracle_prune(t.t_o, 2, &marked) == 0);
		CHECK(marked == npools);
		zr_oracle_fini(t.t_o);
		t.t_o = NULL;
		sp->zp_ino = ino;
		sa->za_gen = gen;
		sa->za_ctime = ct;
		sp->zp_nlink = nlink;
		sp->zp_type = type;
		hp->zp_nnames = nnames;
		hp->zp_names[1] = was;
	}
	/* whole again, so the loop put every field back */
	CHECK(zr_oracle_init(&t.t_o, &t.t_w[0], &t.t_w[1], &t.t_w[2]) == 0);
	marked = 0;
	CHECK(zr_oracle_prune(t.t_o, 1, &marked) == 0);
	CHECK(marked == npools);
	trees_close(&t);
}

int
main(void)
{
	(void) umask(022);
	check_main();
	check_big(MEG, NOFLIP, 1, 2 * (uint64_t)MEG);	/* ZC18 */
	check_big(MEG, MEG - 1, 0, 2 * (uint64_t)MEG);	/* ZC16 */
	check_big(MEG, 0, 0, 2 * (uint64_t)CHUNK);
	check_trans();
	check_readerr();
	check_prune_all();
	check_prune_changed();
	check_prune_fields();
	printf("check_yellow: %d checks passed\n", checks);
	return (0);
}
