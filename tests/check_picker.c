/*
 * The picker's model: the built-in picker with no terminal in it.
 *
 * One world per scenario -- four directories built by hand, a
 * manifest written as text and a resolution beside it -- opened
 * through the child's own argv, then driven with key codes and asked
 * what it drew, what it queued and what it wrote. No curses is
 * linked here and no terminal is opened: that is what plan section
 * 3.5 buys by keeping model.c free of both.
 *
 * The family is ZP of tests/MATRIX.md. Covered: ZP1 to ZP12, ZP14 to
 * ZP19, ZP21 to ZP39, ZP41 to ZP60, and ZP112. ZP13 and ZP40 are
 * covered for the model's half, which is what the model answers; the
 * merge view itself is picker-merge's. ZP69 is covered for the
 * queue's half, the printing after endwin being picker-list's, and
 * ZP111 for zr_pk_open's half of the argv refusal. ZP20 is here as
 * the message at open. What is left of the family is the terminal
 * (ZP61 to ZP77), the merge (ZP78 to ZP109), the standalone binary
 * (ZP110, ZP113, ZP114) and ZP70's refusal with no terminal at all,
 * which wants the real entry point picker-list writes.
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

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "manifest.h"
#include "plugins/picker/picker.h"

#define	PATHMAX		1024

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

/*
 * A template under TMPDIR, or /tmp without it: the box's /tmp may be
 * a tmpfs, and the socket a tree holds wants a path short enough for
 * sun_path either way.
 */
static void
tmp_template(char *buf, size_t len, const char *leaf)
{
	const char *d = getenv("TMPDIR");

	(void) snprintf(buf, len, "%s/%s", d != NULL && d[0] != '\0' ?
	    d : "/tmp", leaf);
}

static void
join(char *out, size_t outlen, const char *a, const char *b)
{
	int n;

	n = snprintf(out, outlen, "%s%s", a, b);
	CHECK(n > 0 && (size_t)n < outlen);
}

/* Everything under one root, children before their parents. */
static void
rmtree(const char *root)
{
	char path[PATHMAX];
	struct dirent *de;
	struct stat st;
	DIR *d;

	d = opendir(root);
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		(void) snprintf(path, sizeof (path), "%s/%s", root,
		    de->d_name);
		if (lstat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			rmtree(path);
		else
			(void) unlink(path);
	}
	(void) closedir(d);
	(void) rmdir(root);
}

static void
put(const char *path, const char *bytes, size_t len)
{
	FILE *f = fopen(path, "w");

	CHECK(f != NULL);
	CHECK(fwrite(bytes, 1, len, f) == len);
	CHECK(fclose(f) == 0);
}

static char *
slurp(const char *path, size_t *lenp)
{
	long n;
	char *out;
	FILE *f;

	*lenp = 0;
	f = fopen(path, "r");
	if (f == NULL)
		return (NULL);
	CHECK(fseek(f, 0, SEEK_END) == 0);
	n = ftell(f);
	CHECK(n >= 0);
	CHECK(fseek(f, 0, SEEK_SET) == 0);
	out = malloc((size_t)n + 1);
	CHECK(out != NULL);
	CHECK(fread(out, 1, (size_t)n, f) == (size_t)n);
	CHECK(fclose(f) == 0);
	out[n] = '\0';
	*lenp = (size_t)n;
	return (out);
}

/* Two documents held against each other, with the first difference. */
static void
same(const char *tag, const char *got, size_t gotlen, const char *want,
    size_t wantlen)
{
	size_t i;

	checks++;
	if (gotlen == wantlen && memcmp(got, want, gotlen) == 0)
		return;
	for (i = 0; i < gotlen && i < wantlen; i++)
		if (got[i] != want[i])
			break;
	printf("%s: the documents differ at byte %zu\n", tag, i);
	printf("--- got (%zu bytes)\n%.*s\n", gotlen, (int)gotlen, got);
	printf("--- want (%zu bytes)\n%.*s\n", wantlen, (int)wantlen, want);
	exit(1);
}

/*
 * ---------------------------------------------------------------
 * One world: the four trees, the two documents, and the argv the
 * launcher would have built (src/launch.h, zr_launch_argv).
 * ---------------------------------------------------------------
 */

#define	W_ARGV0		"zfs_rebase-picker"

struct world {
	char	w_root[PATHMAX];
	char	w_tree[ZR_PK_NTREE][PATHMAX];
	char	w_res[PATHMAX];
	char	w_man[PATHMAX];
	char	w_arg[ZR_PK_ARGC][PATHMAX];
	char	*w_av[ZR_PK_ARGC];
};

static const char *const w_treename[ZR_PK_NTREE] = {
	"/base", "/from", "/onto", "/result"
};

static void
world_init(struct world *w)
{
	char tmpl[PATHMAX];
	int t;

	memset(w, 0, sizeof (*w));
	tmp_template(tmpl, sizeof (tmpl), "zrpicker.XXXXXX");
	CHECK(mkdtemp(tmpl) != NULL);
	join(w->w_root, sizeof (w->w_root), tmpl, "");
	for (t = 0; t < ZR_PK_NTREE; t++) {
		join(w->w_tree[t], sizeof (w->w_tree[t]), w->w_root,
		    w_treename[t]);
		CHECK(mkdir(w->w_tree[t], 0755) == 0);
	}
	join(w->w_res, sizeof (w->w_res), w->w_root, "/resolution");
	join(w->w_man, sizeof (w->w_man), w->w_root, "/manifest");
	(void) snprintf(w->w_arg[0], sizeof (w->w_arg[0]), "%s", W_ARGV0);
	(void) snprintf(w->w_arg[1], sizeof (w->w_arg[1]), "%s", w->w_res);
	for (t = 0; t < ZR_PK_NTREE; t++)
		(void) snprintf(w->w_arg[ZR_PK_ARGV_TREE + t],
		    sizeof (w->w_arg[0]), "%s", w->w_tree[t]);
	for (t = 0; t < ZR_PK_ARGC; t++)
		w->w_av[t] = w->w_arg[t];
}

static void
world_docs(struct world *w, const char *man, const char *res)
{
	put(w->w_man, man, strlen(man));
	put(w->w_res, res, strlen(res));
}

static void
world_fini(struct world *w)
{
	rmtree(w->w_root);
}

/* One name in one tree of the world. */
static void
w_path(const struct world *w, int t, const char *rel, char *out, size_t len)
{
	join(out, len, w->w_tree[t], rel);
}

static void
w_text(const struct world *w, int t, const char *rel, const char *text)
{
	char path[PATHMAX];

	w_path(w, t, rel, path, sizeof (path));
	put(path, text, strlen(text));
}

static void
w_bin(const struct world *w, int t, const char *rel)
{
	static const char bytes[] = { 'B', 'M', '\0', 'x', '\n' };
	char path[PATHMAX];

	w_path(w, t, rel, path, sizeof (path));
	put(path, bytes, sizeof (bytes));
}

static void
w_dir(const struct world *w, int t, const char *rel)
{
	char path[PATHMAX];

	w_path(w, t, rel, path, sizeof (path));
	CHECK(mkdir(path, 0755) == 0);
}

static void
w_link(const struct world *w, int t, const char *rel, const char *to)
{
	char path[PATHMAX];

	w_path(w, t, rel, path, sizeof (path));
	CHECK(symlink(to, path) == 0);
}

static void
w_fifo(const struct world *w, int t, const char *rel)
{
	char path[PATHMAX];

	w_path(w, t, rel, path, sizeof (path));
	CHECK(mkfifo(path, 0644) == 0);
}

/* A socket at a name, bound the way src/apply.c binds one. */
static void
w_sock(const struct world *w, int t, const char *rel)
{
	struct sockaddr_un sun;
	char path[PATHMAX];
	size_t len;
	int fd;

	w_path(w, t, rel, path, sizeof (path));
	len = strlen(path);
	CHECK(len < sizeof (sun.sun_path));
	memset(&sun, 0, sizeof (sun));
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, path, len);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	CHECK(fd >= 0);
	CHECK(bind(fd, (const struct sockaddr *)&sun,
	    (socklen_t)sizeof (sun)) == 0);
	CHECK(close(fd) == 0);
}

static void
open_ok(const char *tag, struct world *w, struct zr_picker *pk)
{
	char err[512];

	err[0] = '\0';
	checks++;
	if (zr_pk_open(pk, ZR_PK_ARGC, w->w_av, err, sizeof (err)) != 0) {
		printf("%s: the picker refused to open: %s\n", tag, err);
		exit(1);
	}
}

/*
 * Everything the open queued, taken off before a key is pressed:
 * the tests below are about the messages their own keys make, and
 * the open's own line is test_rows's and test_messages's.
 */
static void
drain(struct zr_picker *pk)
{
	while (zr_pk_msg(pk) != NULL)
		continue;
}

/*
 * One line of a document swapped for another, which is how the want
 * text of a write is built: from the bytes that went in, changed
 * where the keys touched them and nowhere else.
 */
static void
swap_line(char *doc, size_t cap, const char *from, const char *to)
{
	size_t flen = strlen(from), tlen = strlen(to);
	char *at = strstr(doc, from);

	CHECK(at != NULL);
	CHECK(strlen(doc) + tlen - flen + 1 <= cap);
	memmove(at + tlen, at + flen, strlen(at + flen) + 1);
	memcpy(at, to, tlen);
}

/* One row's name, as the row carries it: decoded bytes and a length. */
static int
named(const struct zr_pk_row *row, const char *want)
{
	size_t n = strlen(want);

	return (row != NULL && row->zk_namelen == n &&
	    memcmp(row->zk_name, want, n) == 0);
}

/*
 * ---------------------------------------------------------------
 * The main scenario's two documents. Eleven lines over ten
 * conflicted names, one of every object kind, a group of two, a
 * drift line, a hand-added line for a name the manifest never
 * marked, and one marked name the document does not have.
 * ---------------------------------------------------------------
 */

#define	M_HDR(acts, confs)						\
	"#rebase-manifest 5\n#result -\n#form posix\n"			\
	"#base base 11\n#from from 22\n#onto onto 33\n"			\
	"#made -\n#tag -\n#take -\n#written -\n#mode strict\n"		\
	"#actions " acts "\n#conflicts " confs "\n"

#define	R_HDR(names, unans)						\
	"#rebase-resolution 5\n#base base 11\n#from from 22\n"		\
	"#onto onto 33\n#mode strict\n#names " names "\n"		\
	"#unanswered " unans "\n"

/* One record with nothing to say, for the names the tests do not read. */
#define	REC(n, cls, why)						\
	"conflict " n " " cls "\n  why  " why "\n"			\
	"  base ()\n  from ()\n  onto ()\n"

#define	M_WHY1		"/a.txt changed on both sides"
#define	M_BASE1		"({/a.txt}x)"
#define	M_FROM1		"({/a.txt}y)"
#define	M_ONTO1		"({/a.txt}z)"

static const char man_main[] =
	M_HDR("0", "9")
	"/\n"
	"    a.txt conflict 1\n"
	"    add.txt conflict 2\n"
	"    bin.dat conflict 3\n"
	"    d/ conflict 4\n"
	"        f.txt conflict 4\n"
	"        ..\n"
	"    del.txt conflict 5\n"
	"    gone.txt conflict 6\n"
	"    lnk conflict 7\n"
	"    pipe conflict 8\n"
	"    sock conflict 9\n"
	"    ..\n"
	"\n"
	"# a pool is one file and all its names: {names}letter; same\n"
	"# letter, same bytes\n"
	"conflict 1 changed-both\n"
	"  why  " M_WHY1 "\n"
	"  base " M_BASE1 "\n"
	"  from " M_FROM1 "\n"
	"  onto " M_ONTO1 "\n"
	REC("2", "contested-home", "/add.txt was created on both sides")
	REC("3", "changed-both", "/bin.dat changed on both sides")
	REC("4", "disagree", "/d disagrees with itself")
	REC("5", "changed-both", "/del.txt went one way and changed another")
	REC("6", "changed-both", "/gone.txt changed on both sides")
	REC("7", "changed-both", "/lnk changed on both sides")
	REC("8", "changed-both", "/pipe changed on both sides")
	REC("9", "changed-both", "/sock changed on both sides");

static const char res_main[] =
	R_HDR("11", "6")
	"/\n"
	"    a.txt conflict 1 -\n"
	"    add.txt conflict 2 keep\n"
	"    bin.dat conflict 3 onto\n"
	"    d/ conflict 4 from\n"
	"        f.txt conflict 4 from\n"
	"        ..\n"
	"    del.txt conflict 5 -\n"
	"    hand.txt conflict 42 -\n"
	"    k.txt drift keep\n"
	"    lnk conflict 7 -\n"
	"    pipe conflict 8 -\n"
	"    sock conflict 9 -\n"
	"    ..\n";

/* The eleven names, in the order the file has them. */
static const char *const res_names[] = {
	"/a.txt", "/add.txt", "/bin.dat", "/d", "/d/f.txt", "/del.txt",
	"/hand.txt", "/k.txt", "/lnk", "/pipe", "/sock"
};

/* Where each of the eleven lines sits, once the rows are built. */
#define	R_A		0
#define	R_ADD		1
#define	R_BIN		2
#define	R_D		3
#define	R_F		4
#define	R_DEL		5
#define	R_HAND		6
#define	R_K		7
#define	R_LNK		8
#define	R_PIPE		9
#define	R_SOCK		10
#define	R_N		11

static void
build_main(struct world *w)
{
	int t;

	world_docs(w, man_main, res_main);
	for (t = ZR_PK_T_BASE; t <= ZR_PK_T_ONTO; t++) {
		w_text(w, t, "/a.txt", t == ZR_PK_T_BASE ? "one\ntwo\n" :
		    (t == ZR_PK_T_FROM ? "one\nfrom\n" : "one\nonto\n"));
		w_bin(w, t, "/bin.dat");
		w_dir(w, t, "/d");
		w_text(w, t, "/d/f.txt", "deep\n");
		w_text(w, t, "/k.txt", "drifted\n");
		w_link(w, t, "/lnk", "a.txt");
		w_fifo(w, t, "/pipe");
		w_sock(w, t, "/sock");
		if (t != ZR_PK_T_BASE)
			w_text(w, t, "/add.txt", "added\n");
		if (t != ZR_PK_T_FROM)
			w_text(w, t, "/del.txt", "deleted on from\n");
	}
	w_text(w, ZR_PK_T_RESULT, "/a.txt", "one\nonto\n");
	w_text(w, ZR_PK_T_RESULT, "/k.txt", "drifted\n");
}

/*
 * ZP1 to ZP8, ZP10, ZP12, ZP20 and ZP27: the join. One row per
 * resolution line, in the file's order; the line kinds and the
 * choices the document opened at; the group and the manifest's
 * record behind a marked line and neither behind the other two; the
 * object kind on each tree and the row's own; the F/O pair; and the
 * marked name the document does not have, which is a message and not
 * a row.
 */
static void
test_rows(void)
{
	const struct zr_pk_row *row;
	const struct zr_pk_counts *c;
	struct zr_picker pk;
	const char *msg;
	struct world w;
	uint32_t i;

	world_init(&w);
	build_main(&w);
	open_ok("main", &w, &pk);
	/* ZP1: eleven lines in, eleven rows out, in the file's order */
	CHECK(zr_pk_nrows(&pk) == R_N);
	for (i = 0; i < R_N; i++)
		CHECK(named(zr_pk_row(&pk, i), res_names[i]));
	CHECK(zr_pk_row(&pk, R_N) == NULL);
	/* ZP2: the four choices, as the document spelled them */
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_NONE);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_ADD)) == ZR_CH_KEEP);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_BIN)) == ZR_CH_ONTO);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_D)) == ZR_CH_FROM);
	/* ZP3: a drift line, with no group and the gate's own choice */
	row = zr_pk_row(&pk, R_K);
	CHECK(row->zk_kind == ZR_PK_L_DRIFT);
	CHECK(row->zk_group == 0);
	CHECK(row->zk_rec == NULL);
	CHECK(zr_pk_choice(row) == ZR_CH_KEEP);
	/* ZP4: a hand-added line, whose group no record answers to */
	row = zr_pk_row(&pk, R_HAND);
	CHECK(row->zk_kind == ZR_PK_L_HAND);
	CHECK(row->zk_group == 0);
	CHECK(row->zk_line->zl_group == 42);
	/* ZP6: neither of the two has a detail to show */
	CHECK(row->zk_rec == NULL);
	/* ZP5: the marked line's why line, class and three trees */
	row = zr_pk_row(&pk, R_A);
	CHECK(row->zk_kind == ZR_PK_L_CONFLICT);
	CHECK(row->zk_group == 1);
	CHECK(row->zk_rec != NULL);
	CHECK(strcmp(row->zk_rec->zr_why, M_WHY1) == 0);
	CHECK(row->zk_rec->zr_flags == ZR_CF_CHANGED_BOTH);
	CHECK(strcmp(row->zk_rec->zr_base, M_BASE1) == 0);
	CHECK(strcmp(row->zk_rec->zr_from, M_FROM1) == 0);
	CHECK(strcmp(row->zk_rec->zr_onto, M_ONTO1) == 0);
	/* ZP7: two names of one group, and how many the group holds */
	CHECK(zr_pk_row(&pk, R_D)->zk_group == 4);
	CHECK(zr_pk_row(&pk, R_F)->zk_group == 4);
	CHECK(zr_pk_group_names(&pk, 4) == 2);
	CHECK(zr_pk_group_names(&pk, 1) == 1);
	CHECK(zr_pk_group_names(&pk, 42) == 0);
	/* ZP27: the directory line's trailing slash is on the row */
	CHECK(zr_pk_row(&pk, R_D)->zk_isdir == 1);
	CHECK(zr_pk_row(&pk, R_F)->zk_isdir == 0);
	/* ZP8: one row of each object kind, off the three trees */
	CHECK(zr_pk_row(&pk, R_A)->zk_ty == ZR_PK_O_TEXT);
	CHECK(zr_pk_row(&pk, R_BIN)->zk_ty == ZR_PK_O_BINARY);
	CHECK(zr_pk_row(&pk, R_D)->zk_ty == ZR_PK_O_DIR);
	CHECK(zr_pk_row(&pk, R_LNK)->zk_ty == ZR_PK_O_LINK);
	CHECK(zr_pk_row(&pk, R_PIPE)->zk_ty == ZR_PK_O_SPECIAL);
	CHECK(zr_pk_row(&pk, R_SOCK)->zk_ty == ZR_PK_O_SPECIAL);
	/* ZP15: a name none of the three trees holds has no kind */
	CHECK(zr_pk_row(&pk, R_HAND)->zk_ty == ZR_PK_O_ABSENT);
	/* the link was not followed: it is a link and not the text it names */
	row = zr_pk_row(&pk, R_LNK);
	CHECK(row->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_LINK);
	CHECK(row->zk_obj[ZR_PK_T_FROM] == ZR_PK_O_LINK);
	CHECK(row->zk_obj[ZR_PK_T_ONTO] == ZR_PK_O_LINK);
	/* ZP10: the binary is binary on every tree that has it */
	row = zr_pk_row(&pk, R_BIN);
	CHECK(row->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_BINARY);
	CHECK(row->zk_obj[ZR_PK_T_ONTO] == ZR_PK_O_BINARY);
	CHECK(row->zk_obj[ZR_PK_T_RESULT] == ZR_PK_O_ABSENT);
	/* ZP12: a side absent, and the kind read off the sides there are */
	row = zr_pk_row(&pk, R_ADD);
	CHECK(row->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_ABSENT);
	CHECK(row->zk_ty == ZR_PK_O_TEXT);
	CHECK(row->zk_fo[0] == ZR_PK_FO_ADD);
	CHECK(row->zk_fo[1] == ZR_PK_FO_ADD);
	row = zr_pk_row(&pk, R_DEL);
	CHECK(row->zk_obj[ZR_PK_T_FROM] == ZR_PK_O_ABSENT);
	CHECK(row->zk_ty == ZR_PK_O_TEXT);
	CHECK(row->zk_fo[0] == ZR_PK_FO_DEL);
	CHECK(row->zk_fo[1] == ZR_PK_FO_EDIT);
	row = zr_pk_row(&pk, R_A);
	CHECK(row->zk_fo[0] == ZR_PK_FO_EDIT);
	CHECK(row->zk_fo[1] == ZR_PK_FO_EDIT);
	CHECK(row->zk_size[ZR_PK_T_BASE] == strlen("one\ntwo\n"));
	row = zr_pk_row(&pk, R_HAND);
	CHECK(row->zk_fo[0] == ZR_PK_FO_NONE);
	CHECK(row->zk_fo[1] == ZR_PK_FO_NONE);
	/* ZP43: the counts the header shows */
	c = zr_pk_counts(&pk);
	CHECK(c->zc_names == R_N);
	CHECK(c->zc_conflicts == 10);
	CHECK(c->zc_groups == 8);
	CHECK(c->zc_drift == 1);
	CHECK(c->zc_unanswered == 6);
	/*
	 * ZP20: the manifest marks /gone.txt and the document has no
	 * line for it. It is not a row and nothing is added -- putting
	 * the line back is the gate's work at the next read -- and the
	 * open says so.
	 */
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/gone.txt") != NULL);
	CHECK(zr_pk_msg(&pk) == NULL);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_dirty(&pk) == 0);
	/* nothing written, nothing saved: the status of an abandon */
	CHECK(zr_pk_status(&pk) == 2);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP29, ZP30: the cursor. Up on the first row and down on the last
 * stay where they are -- a list of names is not a ring -- and the
 * two ends are reachable in one key.
 */
static void
test_cursor(void)
{
	struct zr_picker pk;
	struct world w;

	world_init(&w);
	build_main(&w);
	open_ok("cursor", &w, &pk);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_UP) == ZR_PK_NOTHING);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_DOWN) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == 1);
	CHECK(zr_pk_key(&pk, ZR_PK_UP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_BOTTOM) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == R_N - 1);
	CHECK(zr_pk_key(&pk, ZR_PK_DOWN) == ZR_PK_NOTHING);
	CHECK(zr_pk_cursor(&pk) == R_N - 1);
	CHECK(zr_pk_key(&pk, ZR_PK_BOTTOM) == ZR_PK_NOTHING);
	CHECK(zr_pk_key(&pk, ZR_PK_TOP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_TOP) == ZR_PK_NOTHING);
	/* ZP50: a key the picker does not know changes nothing */
	CHECK(zr_pk_key(&pk, (enum zr_pk_key)99) == ZR_PK_NOTHING);
	CHECK(zr_pk_cursor(&pk) == 0);
	CHECK(zr_pk_dirty(&pk) == 0);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/* The cursor to one row by name, since the tests read better that way. */
static void
cursor_to(struct zr_picker *pk, uint32_t i)
{
	CHECK(zr_pk_key(pk, ZR_PK_TOP) != ZR_PK_EXIT);
	while (zr_pk_cursor(pk) < i)
		CHECK(zr_pk_key(pk, ZR_PK_DOWN) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(pk) == i);
}

/*
 * ZP31 to ZP36 and ZP43: the choice keys. f, o and k answer any line
 * whatever kind it is; "-" is a conflict line's alone, the tool's own
 * and a hand's alike, and a drift line refuses it with the reason,
 * since only a conflict line starts unanswered.
 */
static void
test_choose(void)
{
	struct zr_picker pk;
	const char *msg;
	struct world w;

	world_init(&w);
	build_main(&w);
	open_ok("choose", &w, &pk);
	drain(&pk);
	/* ZP31: f, o and k on a conflict line the manifest marks */
	cursor_to(&pk, R_A);
	CHECK(zr_pk_key(&pk, ZR_PK_FROM) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_FROM);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 5);
	CHECK(zr_pk_dirty(&pk) == 1);
	CHECK(zr_pk_key(&pk, ZR_PK_ONTO) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_ONTO);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_KEEP);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_NOTHING);
	/* ZP34: "-" clears it, and it counts unanswered again */
	CHECK(zr_pk_key(&pk, ZR_PK_CLEAR) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_NONE);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 6);
	/* ZP32: f, o and k on a drift line */
	cursor_to(&pk, R_K);
	CHECK(zr_pk_key(&pk, ZR_PK_FROM) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_K)) == ZR_CH_FROM);
	CHECK(zr_pk_key(&pk, ZR_PK_ONTO) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_K)) == ZR_CH_KEEP);
	/* ZP35: and "-" on it is refused, with the reason */
	CHECK(zr_pk_key(&pk, ZR_PK_CLEAR) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_K)) == ZR_CH_KEEP);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 6);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/k.txt") != NULL);
	CHECK(strstr(msg, "drift") != NULL);
	/* ZP33 and ZP36: the same three, and "-", on a hand-added line */
	cursor_to(&pk, R_HAND);
	CHECK(zr_pk_key(&pk, ZR_PK_ONTO) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_HAND)) == ZR_CH_ONTO);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 5);
	CHECK(zr_pk_key(&pk, ZR_PK_FROM) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_CLEAR) == ZR_PK_REDRAW);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_HAND)) == ZR_CH_NONE);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 6);
	/* the counts that do not move: a choice is not a line */
	CHECK(zr_pk_counts(&pk)->zc_names == R_N);
	CHECK(zr_pk_counts(&pk)->zc_conflicts == 10);
	CHECK(zr_pk_counts(&pk)->zc_groups == 8);
	CHECK(zr_pk_counts(&pk)->zc_drift == 1);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP37 to ZP39: g, which walks one group and nothing else. The names
 * of a group are answered together, so the movement inside one is
 * its own key; a group of one keeps the cursor, and a line with no
 * group says so.
 */
static void
test_group(void)
{
	struct zr_picker pk;
	const char *msg;
	struct world w;

	world_init(&w);
	build_main(&w);
	open_ok("group", &w, &pk);
	drain(&pk);
	/* ZP37: group 4 holds /d and /d/f.txt, and g wraps inside it */
	cursor_to(&pk, R_D);
	CHECK(zr_pk_key(&pk, ZR_PK_GROUP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == R_F);
	CHECK(zr_pk_key(&pk, ZR_PK_GROUP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == R_D);
	/* ZP39: a group of one name keeps the cursor where it is */
	cursor_to(&pk, R_A);
	CHECK(zr_pk_key(&pk, ZR_PK_GROUP) == ZR_PK_NOTHING);
	CHECK(zr_pk_cursor(&pk) == R_A);
	/* ZP38: a drift line and a hand-added line are in no group */
	cursor_to(&pk, R_K);
	CHECK(zr_pk_key(&pk, ZR_PK_GROUP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == R_K);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/k.txt") != NULL);
	CHECK(strstr(msg, "no group") != NULL);
	cursor_to(&pk, R_HAND);
	CHECK(zr_pk_key(&pk, ZR_PK_GROUP) == ZR_PK_REDRAW);
	CHECK(zr_pk_cursor(&pk) == R_HAND);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/hand.txt") != NULL);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP40 (the model's half), ZP41, ZP42 and ZP15: Enter. The model
 * says yes or no; a merge view wants a conflict line whose base,
 * from and onto are all text, and everything else is told why not.
 */
static void
test_enter(void)
{
	char why[ZR_PK_MSGLEN];
	struct zr_picker pk;
	const char *msg;
	struct world w;

	world_init(&w);
	build_main(&w);
	open_ok("enter", &w, &pk);
	drain(&pk);
	/* ZP40: text on all three, and a conflict line */
	cursor_to(&pk, R_A);
	CHECK(zr_pk_can_open(&pk, R_A) == 1);
	CHECK(zr_pk_why_not(&pk, R_A, why, sizeof (why)) == NULL);
	CHECK(zr_pk_key(&pk, ZR_PK_ENTER) == ZR_PK_OPEN);
	CHECK(zr_pk_msg(&pk) == NULL);
	CHECK(zr_pk_can_open(&pk, R_F) == 1);
	/* ZP41: a binary, a directory, a link, a fifo and a socket */
	cursor_to(&pk, R_BIN);
	CHECK(zr_pk_can_open(&pk, R_BIN) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_ENTER) == ZR_PK_REDRAW);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/bin.dat") != NULL);
	CHECK(strstr(msg, "binary") != NULL);
	CHECK(zr_pk_can_open(&pk, R_D) == 0);
	CHECK(zr_pk_can_open(&pk, R_LNK) == 0);
	CHECK(zr_pk_can_open(&pk, R_PIPE) == 0);
	CHECK(zr_pk_can_open(&pk, R_SOCK) == 0);
	CHECK(strstr(zr_pk_why_not(&pk, R_D, why, sizeof (why)),
	    "directory") != NULL);
	/* ZP42: delete/edit is a choice and not a merge */
	cursor_to(&pk, R_DEL);
	CHECK(zr_pk_can_open(&pk, R_DEL) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_ENTER) == ZR_PK_REDRAW);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "from is absent") != NULL);
	/* ZP15: a name no tree holds opens nothing */
	CHECK(zr_pk_can_open(&pk, R_HAND) == 0);
	/* a drift line is answered, not merged */
	CHECK(zr_pk_can_open(&pk, R_K) == 0);
	CHECK(strstr(zr_pk_why_not(&pk, R_K, why, sizeof (why)),
	    "drift") != NULL);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP44 to ZP49 and ZP52 to ZP58: the write, which is the picker's
 * only output. s writes and stays, w writes and goes when nothing is
 * unanswered and says which name is the first that is when something
 * is, q leaves with 2 or with 1 after a save, and the document that
 * comes out differs from the one that went in at exactly the lines
 * the keys touched and at the count line the library owns.
 */
static void
test_write(void)
{
	char tmp[PATHMAX], want[sizeof (res_main) + 64], err[256];
	struct zr_picker pk;
	struct world w;
	const char *msg;
	size_t gotlen;
	char *got;

	world_init(&w);
	build_main(&w);
	join(tmp, sizeof (tmp), w.w_res, ".tmp");
	open_ok("write", &w, &pk);
	drain(&pk);
	/* ZP48: w with something unanswered writes nothing and stays */
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_REDRAW);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/a.txt") != NULL);
	CHECK(strstr(msg, "6") != NULL);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	same("untouched", got, gotlen, res_main, sizeof (res_main) - 1);
	free(got);
	CHECK(zr_pk_status(&pk) == 2);
	/* ZP44 and ZP52: s writes the answers so far and stays on the list */
	cursor_to(&pk, R_A);
	CHECK(zr_pk_key(&pk, ZR_PK_FROM) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_SAVE) == ZR_PK_REDRAW);
	CHECK(zr_pk_msg(&pk) == NULL);
	CHECK(zr_pk_nrows(&pk) == R_N);
	CHECK(zr_pk_cursor(&pk) == R_A);
	CHECK(zr_pk_dirty(&pk) == 0);
	CHECK(zr_pk_status(&pk) == 1);
	got = slurp(tmp, &gotlen);
	CHECK(got == NULL);
	/*
	 * ZP53, ZP54 and ZP55: the document that came back is the one
	 * that went in with one line changed and the count line the
	 * emitter keeps, and nothing else -- not the order, not a line,
	 * not a number the picker carried. The two declared counts are
	 * doctored first, so that a picker that wrote its own would
	 * write those.
	 */
	pk.pk_res.zs_names_declared = 999;
	pk.pk_res.zs_unanswered_declared = 999;
	err[0] = '\0';
	CHECK(zr_pk_write(&pk, err, sizeof (err)) == 0);
	(void) snprintf(want, sizeof (want), "%s", res_main);
	swap_line(want, sizeof (want), "#unanswered 6\n", "#unanswered 5\n");
	swap_line(want, sizeof (want), "    a.txt conflict 1 -\n",
	    "    a.txt conflict 1 from\n");
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	same("one line", got, gotlen, want, strlen(want));
	free(got);
	/* ZP45: q after a save is the "saved and stop" status */
	CHECK(zr_pk_key(&pk, ZR_PK_QUIT) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 1);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP46, ZP56 and ZP58: a document nobody answered. q writes nothing
 * and is the abandon; w over an untouched complete document gives
 * back the bytes it was opened with, line for line and in the same
 * order.
 */
static void
test_quit_and_identity(void)
{
	struct zr_picker pk;
	struct world w;
	size_t gotlen;
	uint32_t i;
	char *got;

	world_init(&w);
	build_main(&w);
	open_ok("quit", &w, &pk);
	cursor_to(&pk, R_BIN);
	CHECK(zr_pk_key(&pk, ZR_PK_FROM) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_QUIT) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 2);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	same("abandoned", got, gotlen, res_main, sizeof (res_main) - 1);
	free(got);
	zr_pk_fini(&pk);
	/*
	 * ZP55: now answer the six that are unanswered, back to front,
	 * so that the order the file comes out in is the file's own and
	 * not the order the keys were pressed in.
	 */
	open_ok("identity", &w, &pk);
	for (i = R_N; i > 0; i--) {
		if (zr_pk_choice(zr_pk_row(&pk, i - 1)) != ZR_CH_NONE)
			continue;
		cursor_to(&pk, i - 1);
		CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	}
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	zr_pk_fini(&pk);
	/* ZP56: the same eleven names, in the same order, none added */
	open_ok("reopen", &w, &pk);
	CHECK(zr_pk_nrows(&pk) == R_N);
	for (i = 0; i < R_N; i++)
		CHECK(named(zr_pk_row(&pk, i), res_names[i]));
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	/* ZP58: w over a document nothing changed gives the same bytes */
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	zr_pk_fini(&pk);
	{
		char *again;
		size_t againlen;

		again = slurp(w.w_res, &againlen);
		CHECK(again != NULL);
		same("unchanged", again, againlen, got, gotlen);
		free(again);
	}
	free(got);
	world_fini(&w);
}

/*
 * ZP57 and ZP60: what the picker writes for a set of choices is what
 * the library's emitter writes for the same lines, byte for byte,
 * and a second save over the first leaves one document with the last
 * choices and no line doubled.
 */
static void
test_emitter(void)
{
	struct zr_resolution mine;
	struct zr_picker pk;
	char err[256], *got;
	struct world w;
	size_t gotlen, wantlen;
	uint32_t i;
	char *want;
	FILE *f;

	world_init(&w);
	build_main(&w);
	open_ok("emitter", &w, &pk);
	cursor_to(&pk, R_A);
	CHECK(zr_pk_key(&pk, ZR_PK_ONTO) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_SAVE) == ZR_PK_REDRAW);
	cursor_to(&pk, R_DEL);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_SAVE) == ZR_PK_REDRAW);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	/* the same lines, taken through the emitter by hand */
	memset(&mine, 0, sizeof (mine));
	f = fopen(w.w_man, "r");
	CHECK(f != NULL);
	{
		struct zr_resolution parsed;
		FILE *rf;

		(void) fclose(f);
		rf = fopen(w.w_res, "r");
		CHECK(rf != NULL);
		err[0] = '\0';
		CHECK(zr_resolution_parse(rf, &parsed, err,
		    sizeof (err)) == 0);
		(void) fclose(rf);
		f = tmpfile();
		CHECK(f != NULL);
		CHECK(zr_resolution_write(f, &parsed) == 0);
		CHECK(fseek(f, 0, SEEK_END) == 0);
		wantlen = (size_t)ftell(f);
		CHECK(fseek(f, 0, SEEK_SET) == 0);
		want = malloc(wantlen + 1);
		CHECK(want != NULL);
		CHECK(fread(want, 1, wantlen, f) == wantlen);
		(void) fclose(f);
		/* ZP60: no line doubled by the second save */
		CHECK(parsed.zs_nlines == R_N);
		zr_resolution_fini(&parsed);
	}
	same("emitter", got, gotlen, want, wantlen);
	free(want);
	free(got);
	/* and the rows the picker still holds are the same eleven */
	CHECK(zr_pk_nrows(&pk) == R_N);
	for (i = 0; i < R_N; i++)
		CHECK(zr_pk_row(&pk, i) != NULL);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_A)) == ZR_CH_ONTO);
	CHECK(zr_pk_choice(zr_pk_row(&pk, R_DEL)) == ZR_CH_KEEP);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP18 and ZP51: an empty resolution. No rows, every count 0, every
 * key but w and q does nothing at all, and w writes the empty
 * document and leaves with 0 -- nothing is unanswered in it.
 */
static const char man_empty[] = M_HDR("0", "0") "/\n    ..\n";
static const char res_empty[] = R_HDR("0", "0") "/\n    ..\n";

static void
test_empty(void)
{
	static const enum zr_pk_key quiet[] = {
		ZR_PK_UP, ZR_PK_DOWN, ZR_PK_TOP, ZR_PK_BOTTOM, ZR_PK_FROM,
		ZR_PK_ONTO, ZR_PK_KEEP, ZR_PK_CLEAR, ZR_PK_GROUP, ZR_PK_ENTER
	};
	struct zr_picker pk;
	struct world w;
	size_t gotlen;
	char *got;
	size_t i;

	world_init(&w);
	world_docs(&w, man_empty, res_empty);
	open_ok("empty", &w, &pk);
	CHECK(zr_pk_nrows(&pk) == 0);
	CHECK(zr_pk_row(&pk, 0) == NULL);
	CHECK(zr_pk_counts(&pk)->zc_names == 0);
	CHECK(zr_pk_counts(&pk)->zc_conflicts == 0);
	CHECK(zr_pk_counts(&pk)->zc_groups == 0);
	CHECK(zr_pk_counts(&pk)->zc_drift == 0);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	CHECK(zr_pk_msg(&pk) == NULL);
	for (i = 0; i < sizeof (quiet) / sizeof (quiet[0]); i++)
		CHECK(zr_pk_key(&pk, quiet[i]) == ZR_PK_NOTHING);
	CHECK(zr_pk_msg(&pk) == NULL);
	CHECK(zr_pk_dirty(&pk) == 0);
	/* q on it is an abandon, and w on it is the document written */
	CHECK(zr_pk_key(&pk, ZR_PK_QUIT) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 2);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	same("empty", got, gotlen, res_empty, sizeof (res_empty) - 1);
	free(got);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP16, ZP17 and ZP19: the documents that are complete before a key
 * is pressed. -i opens the picker whether or not everything is
 * decided (ruling 2), so a --take-onto skeleton, a --take-from one
 * and a document a hand answered all open with their answers on
 * them; a document of drift lines only has rows and no groups.
 */
static const char man_two[] =
	M_HDR("0", "2")
	"/\n    p conflict 1\n    q conflict 2\n    ..\n"
	REC("1", "changed-both", "/p changed on both sides")
	REC("2", "changed-both", "/q changed on both sides");

static const char res_onto[] = R_HDR("2", "0")
	"/\n    p conflict 1 onto\n    q conflict 2 onto\n    ..\n";
static const char res_from[] = R_HDR("2", "0")
	"/\n    p conflict 1 from\n    q conflict 2 from\n    ..\n";
static const char res_hand[] = R_HDR("2", "0")
	"/\n    p conflict 1 keep\n    q conflict 2 from\n    ..\n";
static const char res_drift[] = R_HDR("2", "0")
	"/\n    p drift keep\n    q drift keep\n    ..\n";

static void
test_complete(void)
{
	struct zr_picker pk;
	struct world w;

	world_init(&w);
	/* ZP16: --take-onto answered it, and it opens all the same */
	world_docs(&w, man_two, res_onto);
	open_ok("take-onto", &w, &pk);
	CHECK(zr_pk_nrows(&pk) == 2);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 0)) == ZR_CH_ONTO);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 1)) == ZR_CH_ONTO);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	CHECK(zr_pk_counts(&pk)->zc_groups == 2);
	zr_pk_fini(&pk);
	/* ZP17: --take-from, and a document a hand answered */
	world_docs(&w, man_two, res_from);
	open_ok("take-from", &w, &pk);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 0)) == ZR_CH_FROM);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 1)) == ZR_CH_FROM);
	zr_pk_fini(&pk);
	world_docs(&w, man_two, res_hand);
	open_ok("hand", &w, &pk);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 0)) == ZR_CH_KEEP);
	CHECK(zr_pk_choice(zr_pk_row(&pk, 1)) == ZR_CH_FROM);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	zr_pk_fini(&pk);
	/* ZP19: drift lines only -- rows, no groups, nothing unanswered */
	world_docs(&w, man_two, res_drift);
	open_ok("drift", &w, &pk);
	CHECK(zr_pk_nrows(&pk) == 2);
	CHECK(zr_pk_row(&pk, 0)->zk_kind == ZR_PK_L_DRIFT);
	CHECK(zr_pk_row(&pk, 1)->zk_kind == ZR_PK_L_DRIFT);
	CHECK(zr_pk_counts(&pk)->zc_drift == 2);
	CHECK(zr_pk_counts(&pk)->zc_conflicts == 0);
	CHECK(zr_pk_counts(&pk)->zc_groups == 0);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 0);
	/*
	 * and nothing is said about the two marked names: a line
	 * covers each of them. What the gate puts back is a name with
	 * no line at all, whatever kind the line it has is
	 * (src/run.c's add_drift, which covers by path), so a drift
	 * line over a marked name is not a missing line.
	 */
	CHECK(zr_pk_msg(&pk) == NULL);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP9, ZP11, ZP13, ZP14 and ZP112: what the trees say. The text rule
 * is git's, on the byte and not near it; three trees that disagree on
 * the type make a row that is neither; a tree given as "" is a tree
 * with no path, whose every object is absent and which is never
 * joined onto a name -- "" and "/etc" would be the running system.
 */
static const char man_kinds[] =
	M_HDR("0", "4")
	"/\n    early conflict 1\n    etc/ conflict 2\n        ..\n"
	"    late conflict 3\n    swap conflict 4\n    ..\n"
	REC("1", "changed-both", "/early changed on both sides")
	REC("2", "changed-both", "/etc changed on both sides")
	REC("3", "changed-both", "/late changed on both sides")
	REC("4", "changed-both", "/swap changed on both sides");

static const char res_kinds[] =
	R_HDR("4", "4")
	"/\n    early conflict 1 -\n    etc/ conflict 2 -\n        ..\n"
	"    late conflict 3 -\n    swap conflict 4 -\n    ..\n";

#define	K_EARLY		0
#define	K_ETC		1
#define	K_LATE		2
#define	K_SWAP		3

/* A file of n 'x' bytes with one NUL after them, at offset n. */
static void
w_nul(const struct world *w, int t, const char *rel, size_t n)
{
	char path[PATHMAX];
	char *bytes;

	bytes = malloc(n + 1);
	CHECK(bytes != NULL);
	memset(bytes, 'x', n);
	bytes[n] = '\0';
	w_path(w, t, rel, path, sizeof (path));
	put(path, bytes, n + 1);
	free(bytes);
}

static void
build_kinds(struct world *w)
{
	int t;

	world_docs(w, man_kinds, res_kinds);
	for (t = ZR_PK_T_BASE; t <= ZR_PK_T_ONTO; t++) {
		w_nul(w, t, "/early", ZR_PK_SNIFF - 1);
		w_nul(w, t, "/late", ZR_PK_SNIFF);
		w_dir(w, t, "/etc");
	}
	/* the three disagree: a file on base and from, a directory on onto */
	w_text(w, ZR_PK_T_BASE, "/swap", "a file\n");
	w_text(w, ZR_PK_T_FROM, "/swap", "still a file\n");
	w_dir(w, ZR_PK_T_ONTO, "/swap");
}

static void
test_kinds(void)
{
	struct zr_picker pk;
	struct world w;
	char err[512];
	int t;

	world_init(&w);
	build_kinds(&w);
	open_ok("kinds", &w, &pk);
	/*
	 * ZP9: the NUL at offset 7999 is within the first 8000 bytes
	 * and the one at 8000 is not, which is git's rule on the byte.
	 */
	CHECK(zr_pk_row(&pk, K_EARLY)->zk_ty == ZR_PK_O_BINARY);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_ty == ZR_PK_O_TEXT);
	CHECK(zr_pk_can_open(&pk, K_EARLY) == 0);
	CHECK(zr_pk_can_open(&pk, K_LATE) == 1);
	/* ZP11: a file on two trees and a directory on the third */
	CHECK(zr_pk_row(&pk, K_SWAP)->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_TEXT);
	CHECK(zr_pk_row(&pk, K_SWAP)->zk_obj[ZR_PK_T_ONTO] == ZR_PK_O_DIR);
	CHECK(zr_pk_row(&pk, K_SWAP)->zk_ty == ZR_PK_O_MIXED);
	CHECK(zr_pk_can_open(&pk, K_SWAP) == 0);
	zr_pk_fini(&pk);
	/*
	 * ZP13, ZP14 and ZP112: the tree paths given as "". Every
	 * object on such a tree is absent, no merge view is possible,
	 * and the name /etc is not read off the running system: the
	 * row's kind is absent and not the directory that is there.
	 */
	for (t = 0; t < ZR_PK_NTREE; t++)
		w.w_arg[ZR_PK_ARGV_TREE + t][0] = '\0';
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC, w.w_av, err, sizeof (err)) == 0);
	CHECK(zr_pk_nrows(&pk) == 4);
	CHECK(zr_pk_row(&pk, K_ETC)->zk_ty == ZR_PK_O_ABSENT);
	CHECK(zr_pk_row(&pk, K_ETC)->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_ABSENT);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_ty == ZR_PK_O_ABSENT);
	CHECK(zr_pk_can_open(&pk, K_LATE) == 0);
	CHECK(strcmp(zr_pk_path(&pk, ZR_PK_T_BASE), "") == 0);
	zr_pk_fini(&pk);
	/* base alone with no path: the other two are still read */
	(void) snprintf(w.w_arg[ZR_PK_ARGV_TREE + ZR_PK_T_FROM],
	    sizeof (w.w_arg[0]), "%s", w.w_tree[ZR_PK_T_FROM]);
	(void) snprintf(w.w_arg[ZR_PK_ARGV_TREE + ZR_PK_T_ONTO],
	    sizeof (w.w_arg[0]), "%s", w.w_tree[ZR_PK_T_ONTO]);
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC, w.w_av, err, sizeof (err)) == 0);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_obj[ZR_PK_T_BASE] ==
	    ZR_PK_O_ABSENT);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_obj[ZR_PK_T_FROM] == ZR_PK_O_TEXT);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_ty == ZR_PK_O_TEXT);
	CHECK(zr_pk_row(&pk, K_LATE)->zk_fo[0] == ZR_PK_FO_ADD);
	CHECK(zr_pk_can_open(&pk, K_LATE) == 0);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP28: a name whose bytes want the escaping. The row carries the
 * decoded bytes -- that is what a screen draws and what a path is
 * joined from -- and the write puts the escaping back.
 */
static const char man_vis[] =
	M_HDR("0", "1")
	"/\n    a\\040b\\012c conflict 1\n    ..\n"
	REC("1", "changed-both", "the odd name changed on both sides");

static const char res_vis[] =
	R_HDR("1", "1") "/\n    a\\040b\\012c conflict 1 -\n    ..\n";

static const char res_vis_want[] = R_HDR("1", "0")
	"/\n    a\\040b\\012c conflict 1 keep\n    ..\n";

static void
test_escapes(void)
{
	const struct zr_pk_row *row;
	struct zr_picker pk;
	struct world w;
	size_t gotlen;
	char *got;

	world_init(&w);
	world_docs(&w, man_vis, res_vis);
	w_text(&w, ZR_PK_T_FROM, "/a b\nc", "odd\n");
	open_ok("escapes", &w, &pk);
	CHECK(zr_pk_nrows(&pk) == 1);
	row = zr_pk_row(&pk, 0);
	CHECK(named(row, "/a b\nc"));
	CHECK(row->zk_kind == ZR_PK_L_CONFLICT);
	CHECK(row->zk_obj[ZR_PK_T_FROM] == ZR_PK_O_TEXT);
	CHECK(row->zk_obj[ZR_PK_T_ONTO] == ZR_PK_O_ABSENT);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	same("escapes", got, gotlen, res_vis_want,
	    sizeof (res_vis_want) - 1);
	free(got);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP21 to ZP26 and ZP111: everything zr_pk_open refuses, before a
 * row exists and before a terminal is touched. The parser's own
 * refusals are its own words; the join's are the join's.
 */
static void
refuse(const char *tag, struct world *w, const char *man, const char *res,
    const char *want)
{
	struct zr_picker pk;
	char err[512];

	world_docs(w, man, res);
	err[0] = '\0';
	checks++;
	if (zr_pk_open(&pk, ZR_PK_ARGC, w->w_av, err, sizeof (err)) == 0) {
		printf("%s: the picker opened a document it must refuse\n",
		    tag);
		exit(1);
	}
	checks++;
	if (strstr(err, want) == NULL) {
		printf("%s: the refusal was \"%s\", which does not hold "
		    "\"%s\"\n", tag, err, want);
		exit(1);
	}
	/* a picker that never opened is an abandon, and frees clean */
	CHECK(zr_pk_status(&pk) == 2);
	CHECK(zr_pk_nrows(&pk) == 0);
	zr_pk_fini(&pk);
}

static void
test_refusals(void)
{
	struct zr_picker pk;
	struct world w;
	char err[512];
	char *av[ZR_PK_ARGC + 1];
	int i;

	world_init(&w);
	/* ZP21: two lines for one name */
	refuse("repeat", &w, man_two,
	    R_HDR("3", "3") "/\n    p conflict 1 -\n    p conflict 1 -\n"
	    "    q conflict 2 -\n    ..\n", "already in the tree section");
	/* ZP22: a name that is "..", and one that holds a "/" */
	refuse("dotdot", &w, man_two,
	    R_HDR("2", "2") "/\n    \\056\\056 conflict 1 -\n"
	    "    q conflict 2 -\n    ..\n", "line ");
	refuse("slash", &w, man_two,
	    R_HDR("2", "2") "/\n    a\\057b conflict 1 -\n"
	    "    q conflict 2 -\n    ..\n", "line ");
	/* ZP25: a header count that does not match the lines */
	refuse("names", &w, man_two,
	    R_HDR("3", "2") "/\n    p conflict 1 -\n    q conflict 2 -\n"
	    "    ..\n", "#names says 3");
	refuse("unanswered", &w, man_two,
	    R_HDR("2", "1") "/\n    p conflict 1 -\n    q conflict 2 -\n"
	    "    ..\n", "#unanswered says 1");
	/* a header that is not a resolution's at all */
	refuse("version", &w, man_two, M_HDR("0", "0") "/\n    ..\n",
	    "line 1");
	/* ZP23: a header naming another rebase */
	refuse("rebase", &w, man_two,
	    "#rebase-resolution 5\n#base base 11\n#from other 22\n"
	    "#onto onto 33\n#mode strict\n#names 2\n#unanswered 2\n"
	    "/\n    p conflict 1 -\n    q conflict 2 -\n    ..\n",
	    "names other as the from");
	/* ZP24: the same name, another guid, with both numbers printed */
	refuse("guid", &w, man_two,
	    "#rebase-resolution 5\n#base base 11\n#from from 99\n"
	    "#onto onto 33\n#mode strict\n#names 2\n#unanswered 2\n"
	    "/\n    p conflict 1 -\n    q conflict 2 -\n    ..\n",
	    "the guid 99");
	/* ZP26: no manifest beside the resolution */
	world_docs(&w, man_two, R_HDR("0", "0") "/\n    ..\n");
	CHECK(unlink(w.w_man) == 0);
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC, w.w_av, err, sizeof (err)) == -1);
	CHECK(strstr(err, w.w_man) != NULL);
	zr_pk_fini(&pk);
	/* and a resolution the sibling rule cannot place a manifest for */
	for (i = 0; i < ZR_PK_ARGC; i++)
		av[i] = w.w_av[i];
	av[ZR_PK_ARGV_RES] = w.w_tree[ZR_PK_T_BASE];
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC, av, err, sizeof (err)) == -1);
	CHECK(strstr(err, "no manifest is beside it") != NULL);
	zr_pk_fini(&pk);
	/* ZP111: too few arguments, and too many */
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC - 1, w.w_av, err,
	    sizeof (err)) == -1);
	CHECK(strstr(err, "RESOLUTION BASE FROM ONTO RESULT") != NULL);
	zr_pk_fini(&pk);
	av[ZR_PK_ARGC] = w.w_arg[0];
	err[0] = '\0';
	CHECK(zr_pk_open(&pk, ZR_PK_ARGC + 1, av, err, sizeof (err)) == -1);
	CHECK(err[0] != '\0');
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP26 again, from the other side: the resolution of a -o manifest
 * is FILE.resolution beside FILE, and the picker finds the manifest
 * back by the same rule (src/run.c, resolution_of).
 */
static void
test_sibling(void)
{
	char man[PATHMAX], res[PATHMAX];
	struct zr_picker pk;
	struct world w;

	world_init(&w);
	join(man, sizeof (man), w.w_root, "/mine.txt");
	join(res, sizeof (res), w.w_root, "/mine.txt.resolution");
	put(man, man_two, sizeof (man_two) - 1);
	put(res, res_empty, sizeof (res_empty) - 1);
	(void) snprintf(w.w_arg[ZR_PK_ARGV_RES], sizeof (w.w_arg[0]), "%s",
	    res);
	open_ok("sibling", &w, &pk);
	CHECK(strcmp(zr_pk_manpath(&pk), man) == 0);
	CHECK(strcmp(zr_pk_respath(&pk), res) == 0);
	CHECK(zr_pk_nrows(&pk) == 0);
	/* the two marked names of that manifest have no line here */
	CHECK(zr_pk_msg(&pk) != NULL);
	CHECK(zr_pk_msg(&pk) != NULL);
	CHECK(zr_pk_msg(&pk) == NULL);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP69, the queue's half: the messages come back in the order they
 * were queued, and a queue that fills drops its oldest line rather
 * than allocating in a key handler. The printing after endwin is the
 * screen's and is picker-list's cell.
 */
static void
test_messages(void)
{
	const char *msg, *last;
	struct zr_picker pk;
	struct world w;
	int i;

	world_init(&w);
	build_main(&w);
	open_ok("messages", &w, &pk);
	/* the open queued one line, about the marked name with no row */
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/gone.txt") != NULL);
	CHECK(zr_pk_msg(&pk) == NULL);
	CHECK(zr_pk_last(&pk) == NULL);
	/* two refusals, and they come back in the order they were made */
	cursor_to(&pk, R_K);
	CHECK(zr_pk_key(&pk, ZR_PK_CLEAR) == ZR_PK_REDRAW);
	cursor_to(&pk, R_BIN);
	CHECK(zr_pk_key(&pk, ZR_PK_ENTER) == ZR_PK_REDRAW);
	last = zr_pk_last(&pk);
	CHECK(last != NULL && strstr(last, "/bin.dat") != NULL);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/k.txt") != NULL);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/bin.dat") != NULL);
	CHECK(zr_pk_msg(&pk) == NULL);
	/* a full queue drops the oldest and keeps the newest */
	cursor_to(&pk, R_K);
	for (i = 0; i < ZR_PK_NMSG + 3; i++)
		CHECK(zr_pk_key(&pk, ZR_PK_CLEAR) == ZR_PK_REDRAW);
	for (i = 0; i < ZR_PK_NMSG; i++) {
		msg = zr_pk_msg(&pk);
		CHECK(msg != NULL && strstr(msg, "/k.txt") != NULL);
	}
	CHECK(zr_pk_msg(&pk) == NULL);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP59: a write that cannot be made. The destination is as it was,
 * no .tmp is left beside it, the reason is queued rather than
 * printed, and the picker is still up: what it exits with then is
 * what it would have exited with anyway, which is not 0. Root writes
 * into an unwritable directory anyway, so the cell is the box's
 * there.
 */
static void
test_writefail(void)
{
	char tmp[PATHMAX];
	struct zr_picker pk;
	struct world w;
	const char *msg;
	size_t gotlen;
	char *got;

	if (geteuid() == 0) {
		printf("skip the unwritable directory: running as root\n");
		return;
	}
	world_init(&w);
	world_docs(&w, man_two,
	    R_HDR("2", "2") "/\n    p conflict 1 -\n    q conflict 2 -\n"
	    "    ..\n");
	join(tmp, sizeof (tmp), w.w_res, ".tmp");
	open_ok("writefail", &w, &pk);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(chmod(w.w_root, 0500) == 0);
	CHECK(zr_pk_key(&pk, ZR_PK_SAVE) == ZR_PK_REDRAW);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && msg[0] != '\0');
	CHECK(zr_pk_status(&pk) == 2);
	CHECK(zr_pk_dirty(&pk) == 1);
	/* w fails the same way, and the picker stays up */
	CHECK(zr_pk_key(&pk, ZR_PK_DOWN) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_KEEP) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_REDRAW);
	CHECK(zr_pk_msg(&pk) != NULL);
	CHECK(zr_pk_status(&pk) == 2);
	CHECK(chmod(w.w_root, 0700) == 0);
	got = slurp(tmp, &gotlen);
	CHECK(got == NULL);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	CHECK(strstr(got, "#unanswered 2\n") != NULL);
	free(got);
	/* and once the directory takes a write again, w goes through */
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	CHECK(strstr(got, "#unanswered 0\n") != NULL);
	free(got);
	zr_pk_fini(&pk);
	world_fini(&w);
}

/*
 * ZP47 and ZP49: w over a document with one name left, and w again
 * once that name is answered. The first is refused with the name;
 * the second writes and is the status the tool goes on from.
 */
static void
test_last_name(void)
{
	struct zr_picker pk;
	struct world w;
	const char *msg;
	size_t gotlen;
	char *got;

	world_init(&w);
	world_docs(&w, man_two,
	    R_HDR("2", "1") "/\n    p conflict 1 keep\n    q conflict 2 -\n"
	    "    ..\n");
	open_ok("last", &w, &pk);
	CHECK(zr_pk_counts(&pk)->zc_unanswered == 1);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_REDRAW);
	msg = zr_pk_msg(&pk);
	CHECK(msg != NULL && strstr(msg, "/q") != NULL);
	CHECK(zr_pk_status(&pk) == 2);
	cursor_to(&pk, 1);
	CHECK(zr_pk_key(&pk, ZR_PK_ONTO) == ZR_PK_REDRAW);
	CHECK(zr_pk_key(&pk, ZR_PK_WRITE) == ZR_PK_EXIT);
	CHECK(zr_pk_status(&pk) == 0);
	got = slurp(w.w_res, &gotlen);
	CHECK(got != NULL);
	CHECK(strstr(got, "#unanswered 0\n") != NULL);
	CHECK(strstr(got, "    p conflict 1 keep\n") != NULL);
	CHECK(strstr(got, "    q conflict 2 onto\n") != NULL);
	free(got);
	zr_pk_fini(&pk);
	world_fini(&w);
}

int
main(void)
{
	test_rows();
	test_cursor();
	test_choose();
	test_group();
	test_enter();
	test_write();
	test_quit_and_identity();
	test_emitter();
	test_empty();
	test_complete();
	test_kinds();
	test_escapes();
	test_refusals();
	test_sibling();
	test_messages();
	test_writefail();
	test_last_name();
	printf("check_picker: %d checks passed\n", checks);
	return (0);
}
