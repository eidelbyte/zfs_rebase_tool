/*
 * The built-in picker's model: the half of it that has no terminal.
 *
 * It is given the child's argv (plan section 2.3), reads the
 * resolution it names and the manifest beside it, and joins the two
 * into one row per resolution line: the line's kind and its choice
 * from the document, the why line, the class and the three trees
 * from the manifest's record for its group, and the object at that
 * name on each of the four trees from lstat and, for a regular file,
 * git's rule -- a NUL in the first 8000 bytes makes it binary. Then
 * it takes key codes and writes the document back.
 *
 * What it depends on is the two document parsers and the name codec
 * and nothing else (ground rule 1): no curses, no driver, no ZFS
 * layer. That is what lets check_picker.c drive screen 1 on a
 * machine with no pool and no terminal, and what would let the
 * picker be lifted out whole.
 *
 * Two invariants run through the whole file. The first: every line
 * the document opened with is in the file it writes, in the file's
 * order, none added and none removed -- a hand may change a choice
 * and may add a line, but only a gate may take a conflict away
 * (v4-manifest.md section 8), and this program acts for a hand. The
 * rows are therefore built from the resolution's lines and never
 * from the manifest's marks. The second: no count of the model's
 * ever reaches the document. #names and #unanswered are recomputed
 * by the library's emitter from the lines it writes, so the two can
 * never drift apart.
 */

#define	_XOPEN_SOURCE	700
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "picker.h"
#include "vis.h"

/*
 * The two names the sibling rule knows: the resolution is
 * <rundir>/resolution beside <rundir>/manifest, or FILE.resolution
 * beside a -o FILE (src/run.c's resolution_of, which this reads
 * backwards). The rule is written out again here rather than
 * included, because run.h is the driver's.
 */
#define	PK_RESOLUTION	"resolution"
#define	PK_MANIFEST	"manifest"
#define	PK_DOTRES	".resolution"

/* Room for one vis-encoded name inside a message. */
#define	PK_NAMEBUF	128

/* The three trees a merge view wants, and the words for them. */
#define	PK_NSIDE	3

static const char *const pk_treeword[PK_NSIDE] = { "base", "from", "onto" };

/* What an object is, said in a sentence fragment. */
static const char *const pk_objword[] = {
	"absent", "text", "binary", "a directory", "a link", "special",
	"of two types"
};

/*
 * ---------------------------------------------------------------
 * Small things: memory, messages, names.
 * ---------------------------------------------------------------
 */

static char *
pk_dup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *out = malloc(n);

	if (out != NULL)
		memcpy(out, s, n);
	return (out);
}

/* One line into the caller's error buffer. */
static void
pk_err(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	(void) vsnprintf(err, errlen, fmt, ap);
	va_end(ap);
}

/*
 * One line into the queue the screen drains after endwin. Nothing
 * here writes to stdout or stderr: ground rule 6 forbids it while
 * curses is up, and the model does not know whether it is. The queue
 * is fixed, so a full one drops its oldest line and no allocation
 * can fail in a key handler.
 */
static void
pk_say(struct zr_picker *pk, const char *fmt, ...)
{
	va_list ap;
	uint32_t slot;

	if (pk->pk_nmsg == ZR_PK_NMSG) {
		slot = pk->pk_msghead;
		pk->pk_msghead = (pk->pk_msghead + 1) % ZR_PK_NMSG;
	} else {
		slot = (pk->pk_msghead + pk->pk_nmsg) % ZR_PK_NMSG;
		pk->pk_nmsg++;
	}
	va_start(ap, fmt);
	(void) vsnprintf(pk->pk_msg[slot], sizeof (pk->pk_msg[slot]), fmt, ap);
	va_end(ap);
}

/*
 * One name for a message, in the manifest's own escaping: a name may
 * hold any byte but NUL, and a message is a line of text.
 */
static void
pk_name(const unsigned char *name, size_t len, char *buf, size_t buflen)
{
	(void) zr_vis_encode(name, len, buf, buflen);
}

static void
pk_rowname(const struct zr_pk_row *row, char *buf, size_t buflen)
{
	pk_name(row->zk_name, row->zk_namelen, buf, buflen);
}

/*
 * ---------------------------------------------------------------
 * The two documents.
 * ---------------------------------------------------------------
 */

/*
 * The manifest beside a resolution, by the sibling rule read
 * backwards: <dir>/resolution is <dir>/manifest, and FILE.resolution
 * is FILE. A path that is neither is not a resolution this tool
 * wrote, and there is nowhere to look for its manifest.
 */
static char *
pk_manifest_of(const char *res)
{
	const char *leaf, *slash;
	size_t len, dirlen, suflen;
	char *out;

	len = strlen(res);
	slash = strrchr(res, '/');
	leaf = slash != NULL ? slash + 1 : res;
	dirlen = (size_t)(leaf - res);
	if (strcmp(leaf, PK_RESOLUTION) == 0) {
		out = malloc(dirlen + sizeof (PK_MANIFEST));
		if (out == NULL)
			return (NULL);
		memcpy(out, res, dirlen);
		memcpy(out + dirlen, PK_MANIFEST, sizeof (PK_MANIFEST));
		return (out);
	}
	suflen = sizeof (PK_DOTRES) - 1;
	if (len > suflen && strcmp(res + len - suflen, PK_DOTRES) == 0) {
		out = malloc(len - suflen + 1);
		if (out == NULL)
			return (NULL);
		memcpy(out, res, len - suflen);
		out[len - suflen] = '\0';
		return (out);
	}
	return (NULL);
}

/* One document, opened and parsed, with the path in every refusal. */
static int
pk_read(const char *path, int (*parse)(FILE *, void *, char *, size_t),
    void *out, char *err, size_t errlen)
{
	char why[256];
	FILE *f;
	int rc;

	f = fopen(path, "r");
	if (f == NULL) {
		pk_err(err, errlen, "%s: %s", path, strerror(errno));
		return (-1);
	}
	why[0] = '\0';
	rc = parse(f, out, why, sizeof (why));
	(void) fclose(f);
	if (rc != 0)
		pk_err(err, errlen, "%s: %s", path, why);
	return (rc);
}

static int
pk_parse_res(FILE *in, void *out, char *err, size_t errlen)
{
	return (zr_resolution_parse(in, out, err, errlen));
}

static int
pk_parse_man(FILE *in, void *out, char *err, size_t errlen)
{
	return (zr_manifest_parse(in, out, err, errlen));
}

/*
 * The one thing that says the two documents are one rebase's: the
 * three snapshots, name and guid alike (v4-manifest.md section 8). A
 * resolution written for another rebase describes another tree, and
 * a name whose guid differs is another snapshot wearing that name,
 * which is refused with both numbers printed -- the shape run.c's
 * res_input uses against the record.
 */
static int
pk_same_input(const struct zr_picker *pk, const char *word, const char *res,
    uint64_t rguid, const char *man, uint64_t mguid, char *err, size_t errlen)
{
	if (res == NULL || man == NULL || strcmp(res, man) != 0) {
		pk_err(err, errlen, "%s names %s as the %s and %s names %s",
		    pk->pk_respath, res != NULL ? res : "nothing", word,
		    pk->pk_manpath, man != NULL ? man : "nothing");
		return (-1);
	}
	if (rguid != mguid) {
		pk_err(err, errlen, "%s gives the %s %s the guid %llu and %s "
		    "gives it %llu", pk->pk_respath, word, res,
		    (unsigned long long)rguid, pk->pk_manpath,
		    (unsigned long long)mguid);
		return (-1);
	}
	return (0);
}

static int
pk_same_rebase(const struct zr_picker *pk, char *err, size_t errlen)
{
	const struct zr_resolution *r = &pk->pk_res;
	const struct zr_parsed *m = &pk->pk_man;

	if (pk_same_input(pk, "base", r->zs_base, r->zs_base_guid, m->zp_base,
	    m->zp_base_guid, err, errlen) != 0 ||
	    pk_same_input(pk, "from", r->zs_from, r->zs_from_guid, m->zp_from,
	    m->zp_from_guid, err, errlen) != 0 ||
	    pk_same_input(pk, "onto", r->zs_onto, r->zs_onto_guid, m->zp_onto,
	    m->zp_onto_guid, err, errlen) != 0)
		return (-1);
	return (0);
}

/*
 * ---------------------------------------------------------------
 * The objects on the four trees.
 * ---------------------------------------------------------------
 */

/*
 * One tree path and one tree-relative name joined. The name is the
 * document's own path: absolute inside the tree, decoded, with no
 * trailing slash, so the join is one concatenation and the parse has
 * already refused the names a join could not survive. A tree given
 * as "" has no path at all and is never joined: "" plus "/etc" is
 * "/etc", which is a real directory on any machine, and the picker
 * would read the running system for a tree that is not there.
 */
static char *
pk_join(const char *tree, const unsigned char *name, size_t namelen)
{
	size_t treelen = strlen(tree);
	char *out;

	if (treelen == 0)
		return (NULL);
	out = malloc(treelen + namelen + 1);
	if (out == NULL)
		return (NULL);
	memcpy(out, tree, treelen);
	memcpy(out + treelen, name, namelen);
	out[treelen + namelen] = '\0';
	return (out);
}

/*
 * Text or binary, git's rule: a NUL byte in the first ZR_PK_SNIFF
 * bytes makes a regular file binary (plan section 3.4). A file that
 * will not open is binary too -- what cannot be read cannot be shown
 * three ways -- and says so by having no merge view, which is the
 * only thing the answer is used for.
 */
static enum zr_pk_obj
pk_sniff(const char *path)
{
	unsigned char buf[ZR_PK_SNIFF];
	size_t have = 0;
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (ZR_PK_O_BINARY);
	for (;;) {
		n = read(fd, buf + have, sizeof (buf) - have);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		have += (size_t)n;
		if (have == sizeof (buf))
			break;
	}
	(void) close(fd);
	if (n < 0)
		return (ZR_PK_O_BINARY);
	return (memchr(buf, '\0', have) != NULL ? ZR_PK_O_BINARY :
	    ZR_PK_O_TEXT);
}

/* What one tree holds at one name, and how big it is. */
static enum zr_pk_obj
pk_object(const char *tree, const unsigned char *name, size_t namelen,
    uint64_t *sizep)
{
	enum zr_pk_obj kind;
	struct stat st;
	char *path;

	*sizep = 0;
	path = pk_join(tree, name, namelen);
	if (path == NULL)
		return (ZR_PK_O_ABSENT);
	if (lstat(path, &st) != 0) {
		free(path);
		return (ZR_PK_O_ABSENT);
	}
	if (st.st_size > 0)
		*sizep = (uint64_t)st.st_size;
	if (S_ISDIR(st.st_mode))
		kind = ZR_PK_O_DIR;
	else if (S_ISLNK(st.st_mode))
		kind = ZR_PK_O_LINK;
	else if (S_ISREG(st.st_mode))
		kind = pk_sniff(path);
	else
		kind = ZR_PK_O_SPECIAL;
	free(path);
	return (kind);
}

/*
 * The row's own TY, over the three trees a rebase is made of: text
 * and binary are one type, so a name that is text on one side and
 * binary on the other is binary and has no merge view (ZP10); a name
 * whose trees disagree on the type itself -- a file here, a directory
 * there -- is neither, and says so. The result is not in it: the row
 * is about the object being rebased, and the result is where the
 * answer lands.
 */
static enum zr_pk_obj
pk_ty(const enum zr_pk_obj *obj)
{
	enum zr_pk_obj seen = ZR_PK_O_ABSENT;
	int binary = 0, i;

	for (i = 0; i < PK_NSIDE; i++) {
		enum zr_pk_obj k = obj[i];

		if (k == ZR_PK_O_ABSENT)
			continue;
		if (k == ZR_PK_O_BINARY) {
			binary = 1;
			k = ZR_PK_O_TEXT;
		}
		if (seen == ZR_PK_O_ABSENT)
			seen = k;
		else if (seen != k)
			return (ZR_PK_O_MIXED);
	}
	if (seen == ZR_PK_O_TEXT && binary != 0)
		return (ZR_PK_O_BINARY);
	return (seen);
}

/* What one side did to the name, base against that side. */
static enum zr_pk_fo
pk_fo(enum zr_pk_obj base, enum zr_pk_obj side)
{
	int b = base != ZR_PK_O_ABSENT, s = side != ZR_PK_O_ABSENT;

	if (b != 0 && s != 0)
		return (ZR_PK_FO_EDIT);
	if (s != 0)
		return (ZR_PK_FO_ADD);
	if (b != 0)
		return (ZR_PK_FO_DEL);
	return (ZR_PK_FO_NONE);
}

/*
 * ---------------------------------------------------------------
 * The join: the manifest's conflict marks, and the rows.
 * ---------------------------------------------------------------
 */

/*
 * One name the manifest marks as conflicted. The marks are gathered
 * and sorted so that the join is a search and not a scan of one
 * document per line of the other, and each carries whether the
 * resolution had a line for it: a mark with none is a message at
 * open and nothing more, since putting the line back is the gate's
 * work at the next read (documents-design.md section 11.5) and the
 * picker does not know what take mode to put it back with.
 */
struct pk_mark {
	const unsigned char	*pm_path;
	size_t			pm_len;
	int			pm_found;
};

/* Any total order will do: this one is a search key's, not a walk's. */
static int
pk_mark_cmp(const void *a, const void *b)
{
	const struct pk_mark *x = a, *y = b;
	size_t n = x->pm_len < y->pm_len ? x->pm_len : y->pm_len;
	int r = n != 0 ? memcmp(x->pm_path, y->pm_path, n) : 0;

	if (r != 0)
		return (r);
	if (x->pm_len != y->pm_len)
		return (x->pm_len < y->pm_len ? -1 : 1);
	return (0);
}

static int
pk_marks(const struct zr_parsed *m, struct pk_mark **outp, uint32_t *np)
{
	struct pk_mark *marks;
	uint32_t i, n = 0;

	*outp = NULL;
	*np = 0;
	if (m->zp_nactions == 0)
		return (0);
	marks = malloc((size_t)m->zp_nactions * sizeof (*marks));
	if (marks == NULL)
		return (-1);
	for (i = 0; i < m->zp_nactions; i++) {
		if (m->zp_actions[i].za_kind != ZR_ACT_CONFLICT)
			continue;
		marks[n].pm_path = m->zp_actions[i].za_path;
		marks[n].pm_len = m->zp_actions[i].za_pathlen;
		marks[n].pm_found = 0;
		n++;
	}
	if (n > 1)
		qsort(marks, n, sizeof (*marks), pk_mark_cmp);
	*outp = marks;
	*np = n;
	return (0);
}

static struct pk_mark *
pk_mark_find(struct pk_mark *marks, uint32_t n, const unsigned char *path,
    size_t len)
{
	struct pk_mark key;

	if (marks == NULL || n == 0)
		return (NULL);
	key.pm_path = path;
	key.pm_len = len;
	key.pm_found = 0;
	return (bsearch(&key, marks, n, sizeof (*marks), pk_mark_cmp));
}

/* The manifest's record for one group, or NULL where it has none. */
static const struct zr_record *
pk_record(const struct zr_parsed *m, uint32_t group)
{
	uint32_t i;

	if (group == 0)
		return (NULL);
	for (i = 0; i < m->zp_nrecords; i++)
		if (m->zp_records[i].zr_num == group)
			return (&m->zp_records[i]);
	return (NULL);
}

/*
 * The counts the header shows. Only the unanswered one moves while
 * the picker is up, so the rest are taken once; none of them is ever
 * written into the document.
 */
static void
pk_count(struct zr_picker *pk)
{
	struct zr_pk_counts *c = &pk->pk_counts;
	uint32_t *groups;
	uint32_t i, n = 0;

	memset(c, 0, sizeof (*c));
	c->zc_names = pk->pk_nrows;
	c->zc_unanswered = zr_resolution_unanswered(&pk->pk_res);
	groups = pk->pk_nrows != 0 ? malloc((size_t)pk->pk_nrows *
	    sizeof (*groups)) : NULL;
	for (i = 0; i < pk->pk_nrows; i++) {
		const struct zr_pk_row *row = &pk->pk_rows[i];

		if (row->zk_kind == ZR_PK_L_DRIFT) {
			c->zc_drift++;
			continue;
		}
		c->zc_conflicts++;
		if (row->zk_group != 0 && groups != NULL)
			groups[n++] = row->zk_group;
	}
	if (groups == NULL)
		return;
	for (i = 0; i < n; i++) {
		uint32_t j;

		for (j = 0; j < i; j++)
			if (groups[j] == groups[i])
				break;
		if (j == i)
			c->zc_groups++;
	}
	free(groups);
}

/*
 * One row per line of the resolution, in the file's order and in no
 * other: the document is what says which names are answered here.
 */
static int
pk_build(struct zr_picker *pk, char *err, size_t errlen)
{
	char name[PK_NAMEBUF];
	struct pk_mark *marks = NULL;
	uint32_t i, nmarks = 0;

	if (pk_marks(&pk->pk_man, &marks, &nmarks) != 0) {
		pk_err(err, errlen, "out of memory");
		return (-1);
	}
	if (pk->pk_res.zs_nlines != 0) {
		pk->pk_rows = malloc((size_t)pk->pk_res.zs_nlines *
		    sizeof (*pk->pk_rows));
		if (pk->pk_rows == NULL) {
			free(marks);
			pk_err(err, errlen, "out of memory");
			return (-1);
		}
	}
	for (i = 0; i < pk->pk_res.zs_nlines; i++) {
		struct zr_rline *line = &pk->pk_res.zs_lines[i];
		struct zr_pk_row *row = &pk->pk_rows[i];
		struct pk_mark *mark;
		int t;

		memset(row, 0, sizeof (*row));
		row->zk_line = line;
		row->zk_name = line->zl_path;
		row->zk_namelen = line->zl_pathlen;
		row->zk_isdir = line->zl_isdir;
		mark = pk_mark_find(marks, nmarks, line->zl_path,
		    line->zl_pathlen);
		if (mark != NULL)
			mark->pm_found = 1;
		if (line->zl_kind == ZR_RL_DRIFT)
			row->zk_kind = ZR_PK_L_DRIFT;
		else if (mark != NULL)
			row->zk_kind = ZR_PK_L_CONFLICT;
		else
			row->zk_kind = ZR_PK_L_HAND;
		if (row->zk_kind == ZR_PK_L_CONFLICT) {
			row->zk_group = line->zl_group;
			row->zk_rec = pk_record(&pk->pk_man, line->zl_group);
		}
		for (t = 0; t < ZR_PK_NTREE; t++)
			row->zk_obj[t] = pk_object(pk->pk_tree[t],
			    row->zk_name, row->zk_namelen, &row->zk_size[t]);
		row->zk_ty = pk_ty(row->zk_obj);
		row->zk_fo[0] = pk_fo(row->zk_obj[ZR_PK_T_BASE],
		    row->zk_obj[ZR_PK_T_FROM]);
		row->zk_fo[1] = pk_fo(row->zk_obj[ZR_PK_T_BASE],
		    row->zk_obj[ZR_PK_T_ONTO]);
		pk->pk_nrows++;
	}
	for (i = 0; i < nmarks; i++) {
		if (marks[i].pm_found != 0)
			continue;
		pk_name(marks[i].pm_path, marks[i].pm_len, name,
		    sizeof (name));
		pk_say(pk, "%s is conflicted in the manifest and has no line "
		    "here; the next gate puts it back", name);
	}
	free(marks);
	return (0);
}

/*
 * ---------------------------------------------------------------
 * Open, write, finish.
 * ---------------------------------------------------------------
 */

int
zr_pk_open(struct zr_picker *out, int argc, char **argv, char *err,
    size_t errlen)
{
	int t;

	if (out == NULL)
		return (-1);
	memset(out, 0, sizeof (*out));
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (argv == NULL || argc != ZR_PK_ARGC) {
		pk_err(err, errlen, "the picker wants RESOLUTION BASE FROM "
		    "ONTO RESULT, %d arguments and not %d", ZR_PK_ARGC - 1,
		    argc > 0 ? argc - 1 : 0);
		return (-1);
	}
	if (argv[ZR_PK_ARGV_RES] == NULL ||
	    argv[ZR_PK_ARGV_RES][0] == '\0') {
		pk_err(err, errlen, "the picker was given no resolution");
		return (-1);
	}
	out->pk_respath = pk_dup(argv[ZR_PK_ARGV_RES]);
	for (t = 0; t < ZR_PK_NTREE; t++)
		out->pk_tree[t] = pk_dup(argv[ZR_PK_ARGV_TREE + t] != NULL ?
		    argv[ZR_PK_ARGV_TREE + t] : "");
	if (out->pk_respath == NULL) {
		pk_err(err, errlen, "out of memory");
		return (-1);
	}
	for (t = 0; t < ZR_PK_NTREE; t++) {
		if (out->pk_tree[t] == NULL) {
			pk_err(err, errlen, "out of memory");
			return (-1);
		}
	}
	out->pk_manpath = pk_manifest_of(out->pk_respath);
	if (out->pk_manpath == NULL) {
		pk_err(err, errlen, "%s: no manifest is beside it: a "
		    "resolution is <rundir>/%s or FILE%s", out->pk_respath,
		    PK_RESOLUTION, PK_DOTRES);
		return (-1);
	}
	if (pk_read(out->pk_respath, pk_parse_res, &out->pk_res, err,
	    errlen) != 0)
		return (-1);
	if (pk_read(out->pk_manpath, pk_parse_man, &out->pk_man, err,
	    errlen) != 0)
		return (-1);
	if (pk_same_rebase(out, err, errlen) != 0)
		return (-1);
	if (pk_build(out, err, errlen) != 0)
		return (-1);
	pk_count(out);
	return (0);
}

/* The emitter zr_doc_write is handed: the library's own writer. */
static int
pk_emit(FILE *out, void *arg)
{
	return (zr_resolution_write(out, arg));
}

int
zr_pk_write(struct zr_picker *pk, char *err, size_t errlen)
{
	if (pk == NULL || pk->pk_respath == NULL) {
		pk_err(err, errlen, "there is no document to write");
		return (-1);
	}
	if (zr_doc_write(pk->pk_respath, pk_emit, &pk->pk_res, err,
	    errlen) != 0)
		return (-1);
	pk->pk_saved = 1;
	pk->pk_dirty = 0;
	return (0);
}

void
zr_pk_fini(struct zr_picker *pk)
{
	int t;

	if (pk == NULL)
		return;
	zr_resolution_fini(&pk->pk_res);
	zr_parsed_fini(&pk->pk_man);
	free(pk->pk_rows);
	free(pk->pk_respath);
	free(pk->pk_manpath);
	for (t = 0; t < ZR_PK_NTREE; t++)
		free(pk->pk_tree[t]);
	memset(pk, 0, sizeof (*pk));
}

int
zr_pk_status(const struct zr_picker *pk)
{
	if (pk == NULL)
		return (2);
	if (pk->pk_done != 0)
		return (0);
	if (pk->pk_saved != 0)
		return (1);
	return (2);
}

/*
 * ---------------------------------------------------------------
 * The keys of screen 1.
 * ---------------------------------------------------------------
 */

static struct zr_pk_row *
pk_here(struct zr_picker *pk)
{
	if (pk->pk_nrows == 0)
		return (NULL);
	return (&pk->pk_rows[pk->pk_cursor]);
}

static enum zr_pk_act
pk_goto(struct zr_picker *pk, uint32_t to)
{
	if (pk->pk_nrows == 0 || to == pk->pk_cursor)
		return (ZR_PK_NOTHING);
	pk->pk_cursor = to;
	return (ZR_PK_REDRAW);
}

/*
 * One choice. The line is the one place a choice lives, so this is
 * the one place that writes one; the unanswered count is taken again
 * from the document, never carried.
 */
static enum zr_pk_act
pk_choose(struct zr_picker *pk, enum zr_choice ch)
{
	struct zr_pk_row *row = pk_here(pk);

	if (row == NULL || row->zk_line->zl_choice == ch)
		return (ZR_PK_NOTHING);
	row->zk_line->zl_choice = ch;
	pk->pk_dirty = 1;
	pk->pk_counts.zc_unanswered = zr_resolution_unanswered(&pk->pk_res);
	return (ZR_PK_REDRAW);
}

/*
 * "-" is a conflict line's alone: only a conflict line starts
 * unanswered (v4-manifest.md section 8), and a drift line that took
 * one would be a name nobody had ever asked about. A hand-added
 * conflict line is a conflict line and clears like one.
 */
static enum zr_pk_act
pk_clear(struct zr_picker *pk)
{
	struct zr_pk_row *row = pk_here(pk);
	char name[PK_NAMEBUF];

	if (row == NULL)
		return (ZR_PK_NOTHING);
	if (row->zk_kind == ZR_PK_L_DRIFT) {
		pk_rowname(row, name, sizeof (name));
		pk_say(pk, "%s is a drift line: only a conflict line starts "
		    "unanswered", name);
		return (ZR_PK_REDRAW);
	}
	return (pk_choose(pk, ZR_CH_NONE));
}

/*
 * g: the next name of this row's group, wrapping inside the group.
 * The names of one group are answered together -- the ones that
 * choose the same side pool as that side pools them -- so walking
 * them is the one movement the list has that the cursor keys do not
 * give. A drift line and a hand-added line have no group: the
 * manifest is what says a group is, and a hand-added line pools with
 * nobody.
 */
static enum zr_pk_act
pk_group(struct zr_picker *pk)
{
	struct zr_pk_row *row = pk_here(pk);
	char name[PK_NAMEBUF];
	uint32_t k;

	if (row == NULL)
		return (ZR_PK_NOTHING);
	if (row->zk_kind != ZR_PK_L_CONFLICT || row->zk_group == 0) {
		pk_rowname(row, name, sizeof (name));
		pk_say(pk, "%s is in no group", name);
		return (ZR_PK_REDRAW);
	}
	for (k = 1; k <= pk->pk_nrows; k++) {
		uint32_t j = (pk->pk_cursor + k) % pk->pk_nrows;

		if (pk->pk_rows[j].zk_kind == ZR_PK_L_CONFLICT &&
		    pk->pk_rows[j].zk_group == row->zk_group)
			return (pk_goto(pk, j));
	}
	return (ZR_PK_NOTHING);
}

const char *
zr_pk_why_not(const struct zr_picker *pk, uint32_t i, char *buf, size_t buflen)
{
	const struct zr_pk_row *row = zr_pk_row(pk, i);
	int t;

	if (row == NULL) {
		(void) snprintf(buf, buflen, "there is no such row");
		return (buf);
	}
	if (row->zk_kind == ZR_PK_L_DRIFT) {
		(void) snprintf(buf, buflen, "a drift line is answered, not "
		    "merged");
		return (buf);
	}
	for (t = 0; t < PK_NSIDE; t++) {
		if (row->zk_obj[t] == ZR_PK_O_TEXT)
			continue;
		(void) snprintf(buf, buflen, "%s is %s", pk_treeword[t],
		    pk_objword[row->zk_obj[t]]);
		return (buf);
	}
	return (NULL);
}

int
zr_pk_can_open(const struct zr_picker *pk, uint32_t i)
{
	char why[ZR_PK_MSGLEN];

	return (zr_pk_why_not(pk, i, why, sizeof (why)) == NULL);
}

/*
 * Enter. The model says yes or no and nothing else: what screen 2
 * opens is its own issue's. A merge view wants a conflict line whose
 * base, from and onto are all text, which is the rule of plan
 * section 3.4 stated over the objects the trees hold.
 */
static enum zr_pk_act
pk_enter(struct zr_picker *pk)
{
	char name[PK_NAMEBUF], why[ZR_PK_MSGLEN];
	struct zr_pk_row *row = pk_here(pk);
	const char *no;

	if (row == NULL)
		return (ZR_PK_NOTHING);
	no = zr_pk_why_not(pk, pk->pk_cursor, why, sizeof (why));
	if (no == NULL)
		return (ZR_PK_OPEN);
	pk_rowname(row, name, sizeof (name));
	pk_say(pk, "%s has no merge view: %s", name, no);
	return (ZR_PK_REDRAW);
}

/* s: the answers so far on the disk, and the list still up. */
static enum zr_pk_act
pk_save(struct zr_picker *pk)
{
	char err[ZR_PK_MSGLEN];

	if (zr_pk_write(pk, err, sizeof (err)) != 0)
		pk_say(pk, "%s", err);
	return (ZR_PK_REDRAW);
}

/*
 * w: the document written and the tool's turn again, which needs a
 * complete document -- the move from the conflicts gate is a
 * complete document and a --continue, and the picker's ok is exactly
 * that. An incomplete one is refused with the first name that is
 * unanswered, and the picker stays up.
 */
static enum zr_pk_act
pk_writekey(struct zr_picker *pk)
{
	char name[PK_NAMEBUF], err[ZR_PK_MSGLEN];
	uint32_t i;

	if (pk->pk_counts.zc_unanswered != 0) {
		name[0] = '\0';
		for (i = 0; i < pk->pk_nrows; i++) {
			if (pk->pk_rows[i].zk_line->zl_choice != ZR_CH_NONE)
				continue;
			pk_rowname(&pk->pk_rows[i], name, sizeof (name));
			break;
		}
		pk_say(pk, "%u of the names are unanswered; the first is %s",
		    pk->pk_counts.zc_unanswered, name);
		return (ZR_PK_REDRAW);
	}
	if (zr_pk_write(pk, err, sizeof (err)) != 0) {
		pk_say(pk, "%s", err);
		return (ZR_PK_REDRAW);
	}
	pk->pk_done = 1;
	return (ZR_PK_EXIT);
}

enum zr_pk_act
zr_pk_key(struct zr_picker *pk, enum zr_pk_key key)
{
	if (pk == NULL)
		return (ZR_PK_NOTHING);
	switch (key) {
	case ZR_PK_UP:
		return (pk->pk_cursor == 0 ? ZR_PK_NOTHING :
		    pk_goto(pk, pk->pk_cursor - 1));
	case ZR_PK_DOWN:
		return (pk->pk_cursor + 1 >= pk->pk_nrows ? ZR_PK_NOTHING :
		    pk_goto(pk, pk->pk_cursor + 1));
	case ZR_PK_TOP:
		return (pk_goto(pk, 0));
	case ZR_PK_BOTTOM:
		return (pk->pk_nrows == 0 ? ZR_PK_NOTHING :
		    pk_goto(pk, pk->pk_nrows - 1));
	case ZR_PK_FROM:
		return (pk_choose(pk, ZR_CH_FROM));
	case ZR_PK_ONTO:
		return (pk_choose(pk, ZR_CH_ONTO));
	case ZR_PK_KEEP:
		return (pk_choose(pk, ZR_CH_KEEP));
	case ZR_PK_CLEAR:
		return (pk_clear(pk));
	case ZR_PK_GROUP:
		return (pk_group(pk));
	case ZR_PK_ENTER:
		return (pk_enter(pk));
	case ZR_PK_SAVE:
		return (pk_save(pk));
	case ZR_PK_WRITE:
		return (pk_writekey(pk));
	case ZR_PK_QUIT:
		return (ZR_PK_EXIT);
	default:
		return (ZR_PK_NOTHING);
	}
}

/*
 * ---------------------------------------------------------------
 * What the screen reads.
 * ---------------------------------------------------------------
 */

uint32_t
zr_pk_nrows(const struct zr_picker *pk)
{
	return (pk != NULL ? pk->pk_nrows : 0);
}

const struct zr_pk_row *
zr_pk_row(const struct zr_picker *pk, uint32_t i)
{
	if (pk == NULL || i >= pk->pk_nrows)
		return (NULL);
	return (&pk->pk_rows[i]);
}

uint32_t
zr_pk_cursor(const struct zr_picker *pk)
{
	return (pk != NULL ? pk->pk_cursor : 0);
}

const struct zr_pk_counts *
zr_pk_counts(const struct zr_picker *pk)
{
	return (pk != NULL ? &pk->pk_counts : NULL);
}

int
zr_pk_dirty(const struct zr_picker *pk)
{
	return (pk != NULL ? pk->pk_dirty : 0);
}

enum zr_choice
zr_pk_choice(const struct zr_pk_row *row)
{
	return (row != NULL ? row->zk_line->zl_choice : ZR_CH_NONE);
}

uint32_t
zr_pk_group_names(const struct zr_picker *pk, uint32_t group)
{
	uint32_t i, n = 0;

	if (pk == NULL || group == 0)
		return (0);
	for (i = 0; i < pk->pk_nrows; i++)
		if (pk->pk_rows[i].zk_kind == ZR_PK_L_CONFLICT &&
		    pk->pk_rows[i].zk_group == group)
			n++;
	return (n);
}

const char *
zr_pk_path(const struct zr_picker *pk, enum zr_pk_tree tree)
{
	if (pk == NULL || (unsigned)tree >= ZR_PK_NTREE ||
	    pk->pk_tree[tree] == NULL)
		return ("");
	return (pk->pk_tree[tree]);
}

const char *
zr_pk_respath(const struct zr_picker *pk)
{
	if (pk == NULL || pk->pk_respath == NULL)
		return ("");
	return (pk->pk_respath);
}

const char *
zr_pk_manpath(const struct zr_picker *pk)
{
	if (pk == NULL || pk->pk_manpath == NULL)
		return ("");
	return (pk->pk_manpath);
}

const char *
zr_pk_msg(struct zr_picker *pk)
{
	const char *s;

	if (pk == NULL || pk->pk_nmsg == 0)
		return (NULL);
	s = pk->pk_msg[pk->pk_msghead];
	pk->pk_msghead = (pk->pk_msghead + 1) % ZR_PK_NMSG;
	pk->pk_nmsg--;
	return (s);
}

const char *
zr_pk_last(const struct zr_picker *pk)
{
	if (pk == NULL || pk->pk_nmsg == 0)
		return (NULL);
	return (pk->pk_msg[(pk->pk_msghead + pk->pk_nmsg - 1) % ZR_PK_NMSG]);
}
