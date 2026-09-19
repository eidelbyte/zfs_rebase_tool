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
 * the document opened with is in the file it writes, with its own
 * choice, none added and none removed -- a hand may change a choice
 * and may add a line, but only a gate may take a conflict away
 * (v4-manifest.md section 8), and this program acts for a hand. The
 * rows are therefore built from the resolution's lines and never
 * from the manifest's marks. The ORDER is the library writer's and
 * not the file's: zr_resolution_write lays the lines out in the walk
 * order it derives from the names, so siblings a document holds out
 * of that order come back sorted, and a directory line that scopes
 * nothing is not a line to the parser and is not written (the review
 * of 2026-09-11, M8; the writer is shared with every gate, so the
 * picker is in step with the tool, which is what matters). The
 * second: no count of the model's ever reaches the document. #names
 * and #unanswered are recomputed by the library's emitter from the
 * lines it writes, so the two can never drift apart.
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
#include "attrset.h"
#include "walk.h"

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

/*
 * The words for the four trees. The first three are the sides a merge
 * view wants (ZR_PK_NSIDE) and every loop over them stops there; the
 * fourth is for the lines that name the result tree.
 */
static const char *const pk_treeword[ZR_PK_NTREE] = { "base", "from", "onto",
	"result" };

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

/*
 * What one tree holds at one name, how big it is, and -- where the
 * caller asks for it with a non-NULL idp -- the lstat itself, which is
 * how the result tree's rows learn which file they are and how many
 * names it has (v4-manifest.md section 2).
 */
static enum zr_pk_obj
pk_object(const char *tree, const unsigned char *name, size_t namelen,
    uint64_t *sizep, struct stat *idp)
{
	enum zr_pk_obj kind;
	struct stat st;
	char *path;

	*sizep = 0;
	if (idp != NULL)
		memset(idp, 0, sizeof (*idp));
	path = pk_join(tree, name, namelen);
	if (path == NULL)
		return (ZR_PK_O_ABSENT);
	if (lstat(path, &st) != 0) {
		free(path);
		return (ZR_PK_O_ABSENT);
	}
	if (idp != NULL)
		*idp = st;
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

	for (i = 0; i < ZR_PK_NSIDE; i++) {
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

/*
 * The manifest's record for one group, or NULL where it has none.
 *
 * The parser numbers the records 1 to #records and refuses a manifest
 * whose next record is not the one after the last ("expected conflict
 * N", manifest.c), so the number IS the index and the lookup is a
 * subscript. It was a scan of every record per row, which is
 * quadratic over a pool that is its own group: 50000 rows in 50000
 * groups opened in 1.2 seconds against 0.08 in one group (the review
 * of 2026-09-11, M4). The scan is kept as the fallback for the one
 * case the subscript cannot answer -- a manifest whose numbering is
 * not the parser's -- so that this reads the same document the parser
 * accepted and no more.
 */
static const struct zr_record *
pk_record(const struct zr_parsed *m, uint32_t group)
{
	uint32_t i;

	if (group == 0 || m->zp_nrecords == 0)
		return (NULL);
	if (group <= m->zp_nrecords &&
	    m->zp_records[group - 1].zr_num == group)
		return (&m->zp_records[group - 1]);
	for (i = 0; i < m->zp_nrecords; i++)
		if (m->zp_records[i].zr_num == group)
			return (&m->zp_records[i]);
	return (NULL);
}

/* Group numbers, sorted, so that the distinct ones can be counted. */
static int
pk_group_cmp(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	if (x != y)
		return (x < y ? -1 : 1);
	return (0);
}

/*
 * The counts the header shows. Only the unanswered one moves while
 * the picker is up, so the rest are taken once; none of them is ever
 * written into the document.
 */
/*
 * The unanswered count the picker shows, which is not the document's
 * own: a scoping directory is never counted (ruling 48), because it
 * has no action and the person is not asked to answer it.
 */
static uint32_t
pk_unanswered(const struct zr_picker *pk)
{
	uint32_t i, n = 0;

	for (i = 0; i < pk->pk_nrows; i++) {
		const struct zr_pk_row *row = &pk->pk_rows[i];

		if (row->zk_line->zl_choice == ZR_CH_NONE &&
		    !zr_pk_isscope(row))
			n++;
	}
	return (n);
}

static void
pk_count(struct zr_picker *pk)
{
	struct zr_pk_counts *c = &pk->pk_counts;
	uint32_t *groups;
	uint32_t i, n = 0;

	memset(c, 0, sizeof (*c));
	c->zc_names = pk->pk_nrows;
	c->zc_unanswered = pk_unanswered(pk);
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
	/*
	 * The distinct groups, counted by sorting rather than by the
	 * double loop this was: one row per group made it quadratic in
	 * the rows, which is the shape a conflicted pool takes (M4).
	 */
	if (n > 1)
		qsort(groups, n, sizeof (*groups), pk_group_cmp);
	for (i = 0; i < n; i++)
		if (i == 0 || groups[i] != groups[i - 1])
			c->zc_groups++;
	free(groups);
}

/*
 * Can each tree be read at all?
 *
 * Every lstat of a name comes back absent when it fails, whatever the
 * reason, so a tree that is not there or that this process cannot
 * search reads as a tree holding nothing: a name that merges cleanly
 * with base readable shows up as add/add with two conflicts that are
 * not conflicts when base is not, and nothing was said either way
 * (the review of 2026-09-11, M6). Distinguishing the errno per name
 * would not do it, because the likeliest cause is a tree path that is
 * simply wrong, which fails at every name alike.
 *
 * So each tree is asked once, here, before a single row is built: it
 * must be there, be a directory, and be readable and searchable. One
 * queued line names the tree, its path and what the system said; the
 * rows are then built as before, absences and all, with the person
 * holding the reason for them. A tree given as "" is a tree the run
 * has no path for and is not asked about (cell ZP14).
 */
static void
pk_trees_ok(struct zr_picker *pk)
{
	struct stat st;
	int t;

	for (t = 0; t < ZR_PK_NTREE; t++) {
		const char *path = pk->pk_tree[t];

		if (path[0] == '\0')
			continue;
		if (stat(path, &st) != 0) {
			pk_say(pk, "the %s tree at %s cannot be read: %s; "
			    "every name there reads as absent",
			    pk_treeword[t], path, strerror(errno));
			continue;
		}
		if (!S_ISDIR(st.st_mode)) {
			pk_say(pk, "the %s tree at %s is not a directory; "
			    "every name there reads as absent",
			    pk_treeword[t], path);
			continue;
		}
		if (access(path, R_OK | X_OK) != 0)
			pk_say(pk, "the %s tree at %s cannot be read: %s; "
			    "every name there reads as absent",
			    pk_treeword[t], path, strerror(errno));
	}
}

/*
 * Compare the content of two regular files streaming, with a bounded
 * buffer. Returns 1 if they differ, 0 if the same, -1 on error.
 */
#define	PK_CMP_BUF	(64u * 1024u)

static int
pk_files_differ(const char *pa, const char *pb)
{
	unsigned char ba[PK_CMP_BUF], bb[PK_CMP_BUF];
	int fa, fb, rc = 0;
	ssize_t na, nb;

	fa = open(pa, O_RDONLY | O_NOFOLLOW);
	if (fa < 0)
		return (-1);
	fb = open(pb, O_RDONLY | O_NOFOLLOW);
	if (fb < 0) {
		(void) close(fa);
		return (-1);
	}
	for (;;) {
		do {
			na = read(fa, ba, sizeof (ba));
		} while (na < 0 && errno == EINTR);
		do {
			nb = read(fb, bb, sizeof (bb));
		} while (nb < 0 && errno == EINTR);
		if (na < 0 || nb < 0) {
			rc = -1;
			break;
		}
		if (na != nb) {
			rc = 1;
			break;
		}
		if (na == 0)
			break;	/* both at EOF */
		if (memcmp(ba, bb, (size_t)na) != 0) {
			rc = 1;
			break;
		}
	}
	(void) close(fa);
	(void) close(fb);
	return (rc);
}

/*
 * The DIFF column for one row: C where the two sides' content
 * differs, M where their attributes differ, CM where both, "-"
 * where neither or a side is absent.
 */
static enum zr_pk_diff
pk_diff(const struct zr_picker *pk, const struct zr_pk_row *row)
{
	int cdiff = 0, mdiff = 0;
	enum zr_pk_obj fobj, oobj;

	fobj = row->zk_obj[ZR_PK_T_FROM];
	oobj = row->zk_obj[ZR_PK_T_ONTO];

	/* both must be present */
	if (fobj == ZR_PK_O_ABSENT || oobj == ZR_PK_O_ABSENT)
		return (ZR_PK_DIFF_NONE);

	/* different types means C */
	if (fobj != oobj) {
		cdiff = 1;
	} else if (fobj == ZR_PK_O_TEXT || fobj == ZR_PK_O_BINARY) {
		/* regular files: compare by size then content */
		if (row->zk_size[ZR_PK_T_FROM] !=
		    row->zk_size[ZR_PK_T_ONTO]) {
			cdiff = 1;
		} else {
			char *pa, *pb;
			pa = pk_join(pk->pk_tree[ZR_PK_T_FROM],
			    row->zk_name, row->zk_namelen);
			pb = pk_join(pk->pk_tree[ZR_PK_T_ONTO],
			    row->zk_name, row->zk_namelen);
			if (pa != NULL && pb != NULL) {
				if (pk_files_differ(pa, pb) > 0)
					cdiff = 1;
			}
			free(pa);
			free(pb);
		}
	} else if (fobj == ZR_PK_O_LINK) {
		/* symlinks: compare targets */
		if (row->zk_has_at[ZR_PK_T_FROM] &&
		    row->zk_has_at[ZR_PK_T_ONTO]) {
			const char *ta = row->zk_at[ZR_PK_T_FROM].za_target;
			const char *tb = row->zk_at[ZR_PK_T_ONTO].za_target;
			if (ta == NULL || tb == NULL) {
				if (ta != tb) cdiff = 1;
			} else if (strcmp(ta, tb) != 0) {
				cdiff = 1;
			}
		}
	}
	/* dirs, specials with same type: no content to compare */

	/* attributes differ? */
	if (row->zk_has_at[ZR_PK_T_FROM] &&
	    row->zk_has_at[ZR_PK_T_ONTO] &&
	    zr_attrs_differ(&row->zk_at[ZR_PK_T_FROM],
	    &row->zk_at[ZR_PK_T_ONTO]))
		mdiff = 1;

	if (cdiff && mdiff)
		return (ZR_PK_DIFF_CM);
	if (cdiff)
		return (ZR_PK_DIFF_C);
	if (mdiff)
		return (ZR_PK_DIFF_M);
	return (ZR_PK_DIFF_NONE);
}

/*
 * One row per line of the resolution, in the writer's own order: the
 * document is what says which names are answered here.
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
		row->zk_saved = line->zl_choice;
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
		for (t = 0; t < ZR_PK_NTREE; t++) {
			struct stat st;

			row->zk_obj[t] = pk_object(pk->pk_tree[t],
			    row->zk_name, row->zk_namelen, &row->zk_size[t],
			    t == ZR_PK_T_RESULT ? &st : NULL);
			if (t != ZR_PK_T_RESULT ||
			    row->zk_obj[t] == ZR_PK_O_ABSENT)
				continue;
			row->zk_dev = st.st_dev;
			row->zk_ino = st.st_ino;
			row->zk_nlink = (uint32_t)st.st_nlink;
		}
		row->zk_ty = pk_ty(row->zk_obj);
		row->zk_fo[0] = pk_fo(row->zk_obj[ZR_PK_T_BASE],
		    row->zk_obj[ZR_PK_T_FROM]);
		row->zk_fo[1] = pk_fo(row->zk_obj[ZR_PK_T_BASE],
		    row->zk_obj[ZR_PK_T_ONTO]);

		/*
		 * Read attributes for each tree where the object
		 * exists. The result tree's attrs are read too, for
		 * the metadata write.
		 */
		for (t = 0; t < ZR_PK_NTREE; t++) {
			char *p;
			if (row->zk_obj[t] == ZR_PK_O_ABSENT) {
				row->zk_has_at[t] = 0;
				continue;
			}
			p = pk_join(pk->pk_tree[t],
			    row->zk_name, row->zk_namelen);
			if (p == NULL) {
				row->zk_has_at[t] = 0;
				continue;
			}
			if (zr_attr_read(p, &row->zk_at[t],
			    NULL, 0) == 0)
				row->zk_has_at[t] = 1;
			else
				row->zk_has_at[t] = 0;
			free(p);
		}

		/* DIFF: from vs onto, both must be present */
		row->zk_diff = pk_diff(pk, row);
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
	pk_trees_ok(out);
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
	uint32_t i;

	if (pk == NULL || pk->pk_respath == NULL) {
		pk_err(err, errlen, "there is no document to write");
		return (-1);
	}
	if (zr_doc_write(pk->pk_respath, pk_emit, &pk->pk_res, err,
	    errlen) != 0)
		return (-1);
	pk->pk_saved = 1;
	/*
	 * What is on the disk now is what each row was asked to say,
	 * so that is the floor the unsaved count is taken from until
	 * the next write (the author, 2026-09-15, on M9).
	 */
	for (i = 0; i < pk->pk_nrows; i++)
		pk->pk_rows[i].zk_saved = pk->pk_rows[i].zk_line->zl_choice;
	return (0);
}

void
zr_pk_fini(struct zr_picker *pk)
{
	int t;

	if (pk == NULL)
		return;
	zr_pk_merge_close(pk);
	{
		struct zr_pk_row *rp;
		uint32_t ri;
		int ti;
		for (ri = 0; ri < pk->pk_nrows; ri++) {
			rp = &pk->pk_rows[ri];
			for (ti = 0; ti < ZR_PK_NTREE; ti++)
				if (rp->zk_has_at[ti])
					zr_attr_free(&rp->zk_at[ti]);
		}
	}
	zr_resolution_fini(&pk->pk_res);
	zr_parsed_fini(&pk->pk_man);
	free(pk->pk_rows);
	free(pk->pk_respath);
	free(pk->pk_manpath);
	for (t = 0; t < ZR_PK_NTREE; t++)
		free(pk->pk_tree[t]);
	memset(pk, 0, sizeof (*pk));
}

void
zr_pk_set_refresh(struct zr_picker *pk,
    int (*fn)(void *, char *, size_t), void *arg)
{
	if (pk == NULL)
		return;
	pk->pk_refresh = fn;
	pk->pk_refresharg = arg;
}

/*
 * Re-read the two documents from disk and rebuild the rows.  The
 * cursor stays on the same name where it still exists, or moves to
 * the nearest row.  The merge, if any, is closed first: the trees
 * may have changed and the buffers it borrows are freed with the old
 * rows.
 *
 * If call_hook is true and a hook is armed, the hook is called first
 * so the tool rewrites the resolution over the current trees.  A
 * hook failure is one queued line and the rows are left as they were.
 *
 * Returns 0 on success, or -1 with one queued line on failure.
 */
int
zr_pk_reload(struct zr_picker *pk, int call_hook)
{
	struct zr_resolution oldres;
	struct zr_parsed oldman;
	struct zr_pk_row *oldrows;
	uint32_t oldnrows, oldcursor;
	unsigned char *curname = NULL;
	size_t curnamelen = 0;
	char err[ZR_PK_MSGLEN];
	uint32_t i;

	if (pk == NULL)
		return (-1);

	/* Close the merge if one is open. */
	zr_pk_merge_close(pk);

	/* Remember the cursor's name so we can find it in the new rows. */
	if (pk->pk_nrows != 0 && pk->pk_cursor < pk->pk_nrows) {
		const struct zr_pk_row *cur = &pk->pk_rows[pk->pk_cursor];

		curname = malloc(cur->zk_namelen + 1);
		if (curname != NULL) {
			memcpy(curname, cur->zk_name, cur->zk_namelen);
			curname[cur->zk_namelen] = '\0';
			curnamelen = cur->zk_namelen;
		}
	}

	/* Call the hook if asked and armed. */
	if (call_hook && pk->pk_refresh != NULL) {
		if (pk->pk_refresh(pk->pk_refresharg, err,
		    sizeof (err)) != 0) {
			pk_say(pk, "refresh failed: %s", err);
			free(curname);
			return (-1);
		}
	}

	/*
	 * Save the old state and clear the fields that pk_build
	 * overwrites, so that a failed re-read can be rolled back.
	 */
	oldres = pk->pk_res;
	oldman = pk->pk_man;
	oldrows = pk->pk_rows;
	oldnrows = pk->pk_nrows;
	oldcursor = pk->pk_cursor;
	memset(&pk->pk_res, 0, sizeof (pk->pk_res));
	memset(&pk->pk_man, 0, sizeof (pk->pk_man));
	pk->pk_rows = NULL;
	pk->pk_nrows = 0;
	pk->pk_cursor = 0;

	/* Re-read the two documents. */
	if (pk_read(pk->pk_respath, pk_parse_res, &pk->pk_res, err,
	    sizeof (err)) != 0 ||
	    pk_read(pk->pk_manpath, pk_parse_man, &pk->pk_man, err,
	    sizeof (err)) != 0 ||
	    pk_same_rebase(pk, err, sizeof (err)) != 0 ||
	    pk_build(pk, err, sizeof (err)) != 0) {
		/*
		 * The re-read failed: roll back to the old state and
		 * say why.
		 */
		zr_resolution_fini(&pk->pk_res);
		zr_parsed_fini(&pk->pk_man);
		free(pk->pk_rows);
		pk->pk_res = oldres;
		pk->pk_man = oldman;
		pk->pk_rows = oldrows;
		pk->pk_nrows = oldnrows;
		pk->pk_cursor = oldcursor;
		pk_say(pk, "reload failed: %s", err);
		free(curname);
		return (-1);
	}

	/* The old state is replaced; free it. */
	zr_resolution_fini(&oldres);
	zr_parsed_fini(&oldman);
	free(oldrows);

	pk_trees_ok(pk);
	pk_count(pk);

	/*
	 * Find the old cursor's name in the new rows.  Where it has
	 * gone, the nearest row is the one at the same index or the
	 * last one.
	 */
	if (curname != NULL) {
		int found = 0;

		for (i = 0; i < pk->pk_nrows; i++) {
			if (pk->pk_rows[i].zk_namelen == curnamelen &&
			    memcmp(pk->pk_rows[i].zk_name, curname,
			    curnamelen) == 0) {
				pk->pk_cursor = i;
				found = 1;
				break;
			}
		}
		if (!found) {
			if (oldcursor < pk->pk_nrows)
				pk->pk_cursor = oldcursor;
			else if (pk->pk_nrows > 0)
				pk->pk_cursor = pk->pk_nrows - 1;
		}
	}
	free(curname);

	/*
	 * After a reload the document on disk is the authority, so
	 * every row's saved marker is its current choice.
	 */
	for (i = 0; i < pk->pk_nrows; i++)
		pk->pk_rows[i].zk_saved = pk->pk_rows[i].zk_line->zl_choice;

	return (0);
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
 * One choice on one row. The line is the one place a choice lives, so
 * this is the one place that writes one; the unanswered count is
 * taken again from the document, never carried. Screen 2's write
 * comes through here too, with ZR_CH_KEEP.
 */
static void
pk_set(struct zr_picker *pk, struct zr_pk_row *row, enum zr_choice ch)
{
	row->zk_line->zl_choice = ch;
	pk->pk_counts.zc_unanswered = pk_unanswered(pk);
}

/* The cursor's row, and nothing to do where the choice is the one it has. */
static enum zr_pk_act
pk_choose(struct zr_picker *pk, enum zr_choice ch)
{
	struct zr_pk_row *row = pk_here(pk);
	char name[PK_NAMEBUF];

	if (row == NULL)
		return (ZR_PK_NOTHING);
	/*
	 * A scoping directory has no action of its own; a key on it
	 * does nothing but say so (ruling 48).  This is before the
	 * "already this choice" shortcut, because a scoping row that
	 * reads "-" must still say why and not silently do nothing.
	 */
	if (zr_pk_isscope(row)) {
		pk_rowname(row, name, sizeof (name));
		pk_say(pk, "%s is a scoping directory: it has no action of "
		    "its own", name);
		return (ZR_PK_REDRAW);
	}
	if (row->zk_line->zl_choice == ch)
		return (ZR_PK_NOTHING);
	pk_set(pk, row, ch);
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
	int t, all_text;

	if (row == NULL) {
		(void) snprintf(buf, buflen, "there is no such row");
		return (buf);
	}
	if (row->zk_kind == ZR_PK_L_DRIFT) {
		(void) snprintf(buf, buflen, "a drift line is answered, not "
		    "merged");
		return (buf);
	}
	/*
	 * add/add text: the two-way compare (plan section 3.4).
	 */
	if (row->zk_obj[ZR_PK_T_BASE] == ZR_PK_O_ABSENT &&
	    row->zk_obj[ZR_PK_T_FROM] == ZR_PK_O_TEXT &&
	    row->zk_obj[ZR_PK_T_ONTO] == ZR_PK_O_TEXT)
		return (NULL);
	/*
	 * All three sides text: the content merge (the original rule).
	 */
	all_text = 1;
	for (t = 0; t < ZR_PK_NSIDE; t++) {
		if (row->zk_obj[t] != ZR_PK_O_TEXT)
			all_text = 0;
	}
	if (all_text)
		return (NULL);
	/*
	 * Both sides present with the same content and different
	 * metadata: opens on the metadata view alone (design E).
	 * A non-text object whose content differs is refused.
	 */
	if (row->zk_obj[ZR_PK_T_FROM] != ZR_PK_O_ABSENT &&
	    row->zk_obj[ZR_PK_T_ONTO] != ZR_PK_O_ABSENT &&
	    row->zk_diff == ZR_PK_DIFF_M)
		return (NULL);
	for (t = 0; t < ZR_PK_NSIDE; t++) {
		if (row->zk_obj[t] == ZR_PK_O_TEXT)
			continue;
		if (row->zk_obj[t] == ZR_PK_O_ABSENT)
			continue;
		(void) snprintf(buf, buflen, "%s is %s; it is picked "
		    "whole on screen 1 with f or o", pk_treeword[t],
		    pk_objword[row->zk_obj[t]]);
		return (buf);
	}
	/* absent sides: delete/edit, etc. */
	for (t = 0; t < ZR_PK_NSIDE; t++) {
		if (row->zk_obj[t] == ZR_PK_O_ABSENT) {
			(void) snprintf(buf, buflen, "%s is %s",
			    pk_treeword[t], pk_objword[row->zk_obj[t]]);
			return (buf);
		}
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
	if (zr_pk_isscope(row)) {
		pk_rowname(row, name, sizeof (name));
		pk_say(pk, "%s is a scoping directory: it has no action of "
		    "its own", name);
		return (ZR_PK_REDRAW);
	}
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
			if (zr_pk_isscope(&pk->pk_rows[i]))
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

/*
 * ---------------------------------------------------------------
 * Screen 2: the merge a row carries while it is open.
 * ---------------------------------------------------------------
 */

/*
 * One object read whole. The merge borrows these bytes and copies
 * none of them (merge.h), so the buffer lives until zr_m3_fini and
 * the read happens once per name and not once per draw.
 *
 * The size the line table can hold is a uint32_t of bytes, which is
 * the library's own limit, and it is checked here as well so that a
 * refusal names the tree and the name rather than coming back out of
 * the walk. Ruling 11 of 2026-09-10 stands over all of it: a large
 * text is future work and nothing here promises more than the read
 * and the library finish.
 */
static int
pk_slurp(const char *path, unsigned char **bytesp, size_t *lenp, char *err,
    size_t errlen)
{
	unsigned char *buf;
	struct stat st;
	size_t len, at = 0;
	ssize_t n;
	int fd;

	*bytesp = NULL;
	*lenp = 0;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		pk_err(err, errlen, "%s", strerror(errno));
		return (-1);
	}
	if (fstat(fd, &st) != 0) {
		pk_err(err, errlen, "%s", strerror(errno));
		(void) close(fd);
		return (-1);
	}
	if (!S_ISREG(st.st_mode)) {
		pk_err(err, errlen, "it is not a regular file");
		(void) close(fd);
		return (-1);
	}
	if ((uint64_t)st.st_size > (uint64_t)UINT32_MAX) {
		pk_err(err, errlen, "it is too large to merge");
		(void) close(fd);
		return (-1);
	}
	len = (size_t)st.st_size;
	buf = malloc(len != 0 ? len : 1);
	if (buf == NULL) {
		pk_err(err, errlen, "out of memory");
		(void) close(fd);
		return (-1);
	}
	while (at < len) {
		n = read(fd, buf + at, len - at);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			pk_err(err, errlen, "%s", strerror(errno));
			free(buf);
			(void) close(fd);
			return (-1);
		}
		if (n == 0)
			break;		/* it shrank under us: what is there */
		at += (size_t)n;
	}
	(void) close(fd);
	*bytesp = buf;
	*lenp = at;
	return (0);
}

/*
 * The merged bytes into the object that is already there, in place,
 * so that its mode, its owner, its times and its extended attributes
 * stand (plan section 3.4, cell ZP101).
 *
 * O_WRONLY | O_TRUNC and never O_CREAT: the result's object exists at
 * the conflicts gate -- the tool put it there -- and a name that is
 * not there is a tree that is not the one this resolution belongs to,
 * which is a refusal and not a file to create. fsync before the close
 * because the tool's own gates are durable and a merge somebody hand
 * answered should not be the one thing a crash loses.
 *
 * O_NOFOLLOW and O_NONBLOCK with them, and an fstat of what was
 * opened. The gate is where the plan hands the tree to the person, so
 * what stands at the name is whatever a hand left there: without
 * O_NOFOLLOW an absolute symbolic link had the merge written through
 * it, outside the tree, with the real object untouched and the row
 * set to keep; without O_NONBLOCK a fifo there wedged the picker
 * inside open(2) with curses up and no key to get it back, which is
 * the door the fatal-signal handlers stand behind (the review of
 * 2026-09-11, M2 with G6). The caller has already refused what the
 * row's own recorded kind says is not a regular file; this is the
 * same question asked of the object that was actually opened, since
 * the trees are the person's between the two.
 *
 * *damaged says whether the object was truncated before the failure,
 * which is the whole of the caller's recovery decision (G5): the open
 * refusing leaves it as it was, and anything after that leaves a
 * short object.
 */
static int
pk_write_object(const char *path, const unsigned char *bytes, size_t len,
    int *damaged, struct stat *idp, char *err, size_t errlen)
{
	struct stat st;
	size_t at = 0;
	ssize_t n;
	int fd;

	*damaged = 0;
	memset(idp, 0, sizeof (*idp));
	fd = open(path, O_WRONLY | O_TRUNC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT)
			pk_err(err, errlen, "the result tree holds no object "
			    "at this name to write into");
		else if (errno == ELOOP)
			pk_err(err, errlen, "the result tree holds a symbolic "
			    "link at this name, which is never followed");
		else
			pk_err(err, errlen, "%s", strerror(errno));
		return (-1);
	}
	if (fstat(fd, &st) != 0) {
		pk_err(err, errlen, "%s", strerror(errno));
		(void) close(fd);
		return (-1);
	}
	if (!S_ISREG(st.st_mode)) {
		pk_err(err, errlen, "the result tree holds no regular file at "
		    "this name to write into");
		(void) close(fd);
		return (-1);
	}
	/*
	 * Which file this turned out to be, from the descriptor that is
	 * about to be written and not from what the row remembered: it
	 * is what the caller matches the other rows against, and the
	 * trees are the person's between the open and now.
	 */
	*idp = st;
	*damaged = 1;
	while (at < len) {
		n = write(fd, bytes + at, len - at);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			pk_err(err, errlen, "%s", strerror(errno));
			(void) close(fd);
			return (-1);
		}
		at += (size_t)n;
	}
	if (fsync(fd) != 0) {
		pk_err(err, errlen, "%s", strerror(errno));
		(void) close(fd);
		return (-1);
	}
	if (close(fd) != 0) {
		pk_err(err, errlen, "%s", strerror(errno));
		return (-1);
	}
	return (0);
}

/*
 * Every other name of the file just written: set it to keep too.
 *
 * A pool is one file and every name it has (v4-manifest.md section
 * 2), and the merge went into the file, so each of its other names in
 * the result tree is now the merged bytes whether or not anybody
 * answered that row. Leaving those rows unanswered would put the
 * person in front of names whose object had already changed under
 * them; leaving them to be answered some other way would let the
 * resolution say two things about one file. Ruled by the author on
 * 2026-09-15, on the review's question 14: "fixing one named member
 * of a linkpool should update all other members of that linkpool in
 * the manifest".
 *
 * What pools two rows here is the FILE and not the group: the
 * manifest groups by what conflicted, and a hand may have linked two
 * names the tool never grouped together. The candidates are the rows
 * whose recorded device and inode are the written object's, which is
 * what the open paid for; each candidate is then lstat'd again,
 * because the trees are the person's between the open and now and a
 * stale record would answer a name that is no longer that file. The
 * few names of one pool are what is re-read, not every row.
 *
 * The caller writes the document once for all of them, so the tree
 * and the resolution still agree the way finding M9 asks. Returns how
 * many OTHER rows were set.
 */
static uint32_t
pk_pool_keep(struct zr_picker *pk, const struct zr_pk_row *written,
    const struct stat *id)
{
	uint32_t i, n = 0;

	for (i = 0; i < pk->pk_nrows; i++) {
		struct zr_pk_row *row = &pk->pk_rows[i];
		struct stat st;
		char *path;

		if (row == written)
			continue;
		if (row->zk_obj[ZR_PK_T_RESULT] == ZR_PK_O_ABSENT)
			continue;
		if (row->zk_dev != id->st_dev || row->zk_ino != id->st_ino)
			continue;
		path = pk_join(pk->pk_tree[ZR_PK_T_RESULT], row->zk_name,
		    row->zk_namelen);
		if (path == NULL)
			continue;
		if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
		    st.st_dev != id->st_dev || st.st_ino != id->st_ino) {
			free(path);
			continue;
		}
		free(path);
		row->zk_nlink = (uint32_t)st.st_nlink;
		if (row->zk_line->zl_choice != ZR_CH_KEEP)
			pk_set(pk, row, ZR_CH_KEEP);
		n++;
	}
	return (n);
}

/* The first conflicting hunk, or past the end where there is none. */
static void
pk_merge_first(struct zr_pk_merge *mg)
{
	uint32_t i;

	mg->pm_cursor = mg->pm_m3.nchunks;
	for (i = 0; i < mg->pm_m3.nchunks; i++) {
		if (mg->pm_m3.chunks[i].kind == ZR_M3_CONFLICT) {
			mg->pm_cursor = i;
			return;
		}
	}
}

int
zr_pk_merge_open(struct zr_picker *pk)
{
	char name[PK_NAMEBUF], err[ZR_PK_MSGLEN], why[ZR_PK_MSGLEN];
	struct zr_pk_merge *mg;
	struct zr_pk_row *row;
	const char *no;
	char *path;
	int t, all_text, has_content;

	if (pk == NULL)
		return (-1);
	mg = &pk->pk_merge;
	if (mg->pm_open != 0)
		return (0);
	row = pk_here(pk);
	if (row == NULL)
		return (-1);
	pk_rowname(row, name, sizeof (name));
	no = zr_pk_why_not(pk, pk->pk_cursor, why, sizeof (why));
	if (no != NULL) {
		pk_say(pk, "%s has no merge view: %s", name, no);
		return (-1);
	}
	memset(mg, 0, sizeof (*mg));
	mg->pm_row = pk->pk_cursor;

	/*
	 * Determine if the row has a content view: both sides must be
	 * text (including add/add where base is absent).
	 */
	all_text = 1;
	for (t = ZR_PK_T_FROM; t <= ZR_PK_T_ONTO; t++) {
		if (row->zk_obj[t] != ZR_PK_O_TEXT)
			all_text = 0;
	}
	if (row->zk_obj[ZR_PK_T_BASE] != ZR_PK_O_ABSENT &&
	    row->zk_obj[ZR_PK_T_BASE] != ZR_PK_O_TEXT)
		all_text = 0;

	has_content = all_text;
	mg->pm_has_content = has_content;
	mg->pm_content_same = (row->zk_diff == ZR_PK_DIFF_NONE ||
	    row->zk_diff == ZR_PK_DIFF_M);

	/* slurp content for text objects */
	if (has_content) {
		for (t = 0; t < ZR_PK_NSIDE; t++) {
			if (row->zk_obj[t] == ZR_PK_O_ABSENT)
				continue;
			path = pk_join(pk->pk_tree[t], row->zk_name,
			    row->zk_namelen);
			if (path == NULL) {
				pk_say(pk, "%s: %s has no tree path", name,
				    pk_treeword[t]);
				goto fail;
			}
			if (pk_slurp(path, &mg->pm_bytes[t],
			    &mg->pm_len[t], err, sizeof (err)) != 0) {
				pk_say(pk, "%s: %s: %s", name,
				    pk_treeword[t], err);
				free(path);
				goto fail;
			}
			free(path);
		}
		if (zr_m3_open(&mg->pm_m3, mg->pm_bytes[ZR_PK_T_BASE],
		    mg->pm_len[ZR_PK_T_BASE], mg->pm_bytes[ZR_PK_T_FROM],
		    mg->pm_len[ZR_PK_T_FROM], mg->pm_bytes[ZR_PK_T_ONTO],
		    mg->pm_len[ZR_PK_T_ONTO], err, sizeof (err)) != 0) {
			pk_say(pk, "%s: %s", name, err);
			goto fail;
		}
		pk_merge_first(mg);
	}

	/* open the metadata three-way */
	mg->pm_has_meta = (row->zk_has_at[ZR_PK_T_FROM] ||
	    row->zk_has_at[ZR_PK_T_ONTO]);
	if (mg->pm_has_meta) {
		if (zr_pk_meta_open(&mg->pm_meta,
		    row->zk_has_at[ZR_PK_T_BASE] ?
		    &row->zk_at[ZR_PK_T_BASE] : NULL,
		    row->zk_has_at[ZR_PK_T_BASE],
		    row->zk_has_at[ZR_PK_T_FROM] ?
		    &row->zk_at[ZR_PK_T_FROM] : NULL,
		    row->zk_has_at[ZR_PK_T_FROM],
		    row->zk_has_at[ZR_PK_T_ONTO] ?
		    &row->zk_at[ZR_PK_T_ONTO] : NULL,
		    row->zk_has_at[ZR_PK_T_ONTO]) != 0) {
			pk_say(pk, "%s: metadata three-way failed", name);
			goto fail;
		}
	}

	/*
	 * Which view opens first (design B): the view with unpicked
	 * conflicts opens first; content before metadata when both
	 * have some; content when neither has.
	 */
	if (has_content && mg->pm_m3.nconflict > 0)
		mg->pm_view = ZR_PK_VIEW_CONTENT;
	else if (mg->pm_has_meta && mg->pm_meta.mm_nconflict > 0)
		mg->pm_view = ZR_PK_VIEW_META;
	else if (has_content)
		mg->pm_view = ZR_PK_VIEW_CONTENT;
	else
		mg->pm_view = ZR_PK_VIEW_META;

	mg->pm_open = 1;
	return (0);
fail:
	zr_pk_merge_close(pk);
	return (-1);
}

void
zr_pk_merge_close(struct zr_picker *pk)
{
	struct zr_pk_merge *mg;
	int t;

	if (pk == NULL)
		return;
	mg = &pk->pk_merge;
	/* the chunks first: the three buffers are what they point into */
	zr_m3_fini(&mg->pm_m3);
	for (t = 0; t < ZR_PK_NSIDE; t++)
		free(mg->pm_bytes[t]);
	zr_pk_meta_close(&mg->pm_meta);
	memset(mg, 0, sizeof (*mg));
}

struct zr_pk_merge *
zr_pk_merge(struct zr_picker *pk)
{
	if (pk == NULL || pk->pk_merge.pm_open == 0)
		return (NULL);
	return (&pk->pk_merge);
}

uint32_t
zr_pk_merge_hunk(const struct zr_picker *pk)
{
	const struct zr_pk_merge *mg;
	uint32_t i, n = 0;

	if (pk == NULL || pk->pk_merge.pm_open == 0)
		return (0);
	mg = &pk->pk_merge;
	if (mg->pm_cursor >= mg->pm_m3.nchunks)
		return (0);
	for (i = 0; i <= mg->pm_cursor; i++)
		if (mg->pm_m3.chunks[i].kind == ZR_M3_CONFLICT)
			n++;
	return (n);
}

/*
 * f and o: the hunk the cursor is on, and no other (cell ZP83); and
 * - takes the answer back, so the hunk is conflicted again (the
 * author, on the box, 2026-09-10).
 */
static enum zr_pk_act
pk_merge_pick(struct zr_picker *pk, int pick)
{
	struct zr_pk_merge *mg = &pk->pk_merge;

	if (mg->pm_cursor >= mg->pm_m3.nchunks)
		return (ZR_PK_NOTHING);
	if (mg->pm_m3.chunks[mg->pm_cursor].pick == pick)
		return (ZR_PK_NOTHING);
	zr_m3_pick(&mg->pm_m3, mg->pm_cursor, pick);
	return (ZR_PK_REDRAW);
}

/*
 * n and p: the next and the previous conflicting hunk, stopping at
 * the ends. A merge with no conflicting hunk at all -- every chunk
 * decided by the walk -- has nowhere to go and says nothing (ZP87).
 */
static enum zr_pk_act
pk_merge_move(struct zr_picker *pk, int back)
{
	struct zr_pk_merge *mg = &pk->pk_merge;
	uint32_t i;

	if (mg->pm_cursor >= mg->pm_m3.nchunks)
		return (ZR_PK_NOTHING);
	if (back == 0) {
		for (i = mg->pm_cursor + 1; i < mg->pm_m3.nchunks; i++) {
			if (mg->pm_m3.chunks[i].kind == ZR_M3_CONFLICT) {
				mg->pm_cursor = i;
				return (ZR_PK_REDRAW);
			}
		}
		return (ZR_PK_NOTHING);
	}
	for (i = mg->pm_cursor; i > 0; i--) {
		if (mg->pm_m3.chunks[i - 1].kind == ZR_M3_CONFLICT) {
			mg->pm_cursor = i - 1;
			return (ZR_PK_REDRAW);
		}
	}
	return (ZR_PK_NOTHING);
}

/*
 * w on screen 2: the merged bytes into the result's object, the row
 * set to keep, and back to the list.
 *
 * The gate is the library's: zr_m3_result refuses while any conflict
 * chunk is unpicked and its line names the first, which is queued as
 * it comes and the cursor moved there so that the title says which
 * hunk it is (ruling 7, cell ZP91). No marker byte can reach the file
 * by this path or any other: the bytes written are zr_m3_result's and
 * nothing else, and no function of the merge emits a marker at all.
 *
 * keep, because that is what the result standing as it is means
 * (v4-manifest.md section 8) and what the tool's verify leaves alone.
 *
 * And then the document, through the one writer the list's s and w
 * use. Ruled by the author on 2026-09-15, on finding M9: "yep, this
 * should be fixed". Until then the tree held the merge while the
 * resolution still read "-", so a kill between the two lost the whole
 * of what the person had decided and the picker had said the name was
 * set to keep. The order is the object first and the document after
 * it: the object is the write that can damage something, and the
 * document is what speaks for it.
 *
 * The three ways it can end:
 *
 *   - The object write is refused before the open -- a name that is
 *     not there, a link, a fifo -- and nothing is touched: a queued
 *     line and a row that did not move.
 *   - The object write fails after the open truncated, so the object
 *     is short: the row goes back to unanswered whatever it read
 *     before and the line says so, and the document is not written at
 *     all (the review of 2026-09-11, G5). A row that already read
 *     keep would otherwise ship a damaged object without a word.
 *   - The object is written and the document is not: the row stays
 *     keep in memory, the line says the bytes are written and the
 *     resolution could not be saved and names the reason, and the
 *     next s or w from the list writes it. The tree is ahead of the
 *     document then, which is the state the person is told about
 *     rather than left to find.
 */
/*
 * Write the resolved attributes on the result's object, in the
 * apply's order: chown, chmod, xattrs, ACL, flags last.
 */
static int
pk_write_attrs(const char *path, const struct zr_attr *at, int islink,
    int isdir, char *err, size_t errlen)
{
	struct stat st;

	if (lstat(path, &st) != 0) {
		(void) snprintf(err, errlen, "stat: %s", strerror(errno));
		return (-1);
	}
	if (st.st_uid != at->za_uid || st.st_gid != at->za_gid) {
		if (lchown(path, at->za_uid, at->za_gid) != 0) {
			(void) snprintf(err, errlen, "chown: %s",
			    strerror(errno));
			return (-1);
		}
	}
	if (zr_chmod(path, at->za_mode, islink) != 0) {
		(void) snprintf(err, errlen, "chmod: %s", strerror(errno));
		return (-1);
	}
	if (zr_setxattrs(path, at) != 0) {
		(void) snprintf(err, errlen, "xattrs: %s", strerror(errno));
		return (-1);
	}
	if (zr_setacl(path, at, isdir) != 0) {
		(void) snprintf(err, errlen, "acl: %s", strerror(errno));
		return (-1);
	}
	if (zr_setflags(path, at->za_flags) != 0) {
		(void) snprintf(err, errlen, "flags: %s", strerror(errno));
		return (-1);
	}
	return (0);
}

static enum zr_pk_act
pk_merge_write(struct zr_picker *pk)
{
	char name[PK_NAMEBUF], err[ZR_PK_MSGLEN];
	struct zr_pk_merge *mg = &pk->pk_merge;
	struct zr_pk_row *row = &pk->pk_rows[mg->pm_row];
	enum zr_pk_obj kind = row->zk_obj[ZR_PK_T_RESULT];
	unsigned char *bytes = NULL;
	size_t blen = 0;
	int has_bytes = 0, has_attrs = 0;
	struct zr_attr resolved;
	uint32_t first, names;
	struct stat id;

	memset(&id, 0, sizeof (id));
	memset(&resolved, 0, sizeof (resolved));
	pk_rowname(row, name, sizeof (name));

	/*
	 * Phase 1: compute both results, writing nothing. A refusal
	 * in either view leaves the tree untouched (review fix M9).
	 */

	/* (a) content result */
	if (mg->pm_has_content && !mg->pm_content_same) {
		if (zr_m3_result(&mg->pm_m3, &bytes, &blen, err,
		    sizeof (err)) != 0) {
			pk_say(pk, "%s: content: %s", name, err);
			if (zr_m3_first_unpicked(&mg->pm_m3,
			    &first) == 0)
				mg->pm_cursor = first;
			mg->pm_view = ZR_PK_VIEW_CONTENT;
			return (ZR_PK_REDRAW);
		}
		has_bytes = 1;
	}

	/* (b) metadata complete? */
	if (mg->pm_has_meta &&
	    !zr_pk_meta_complete(&mg->pm_meta)) {
		uint32_t unp = mg->pm_meta.mm_nconflict -
		    mg->pm_meta.mm_npicked;
		free(bytes);
		pk_say(pk, "%s: metadata: %u conflict%s unpicked",
		    name, unp, unp == 1 ? "" : "s");
		mg->pm_view = ZR_PK_VIEW_META;
		return (ZR_PK_REDRAW);
	}

	/* resolve metadata */
	if (mg->pm_has_meta && mg->pm_meta.mm_nrows > 0 &&
	    (row->zk_diff == ZR_PK_DIFF_M ||
	    row->zk_diff == ZR_PK_DIFF_CM)) {
		if (zr_pk_meta_result(&mg->pm_meta,
		    row->zk_has_at[ZR_PK_T_BASE] ?
		    &row->zk_at[ZR_PK_T_BASE] : NULL,
		    row->zk_has_at[ZR_PK_T_FROM] ?
		    &row->zk_at[ZR_PK_T_FROM] : NULL,
		    row->zk_has_at[ZR_PK_T_ONTO] ?
		    &row->zk_at[ZR_PK_T_ONTO] : NULL,
		    &resolved, err, sizeof (err)) != 0) {
			free(bytes);
			pk_say(pk, "%s: metadata: %s", name, err);
			return (ZR_PK_REDRAW);
		}
		has_attrs = 1;
	}

	/*
	 * Phase 2: both views clear, write the tree.
	 * (c) content bytes, if any.
	 */
	if (has_bytes) {
		char *path;
		int rc, damaged = 0;

		if (kind == ZR_PK_O_ABSENT) {
			free(bytes);
			zr_attr_free(&resolved);
			pk_say(pk, "%s: the result tree holds no object "
			    "at this name to write into", name);
			return (ZR_PK_REDRAW);
		}
		if (kind != ZR_PK_O_TEXT && kind != ZR_PK_O_BINARY) {
			free(bytes);
			zr_attr_free(&resolved);
			pk_say(pk, "%s: the result tree holds %s at "
			    "this name, and the merged bytes go into "
			    "a regular file or nowhere", name,
			    pk_objword[kind]);
			return (ZR_PK_REDRAW);
		}
		path = pk_join(pk->pk_tree[ZR_PK_T_RESULT],
		    row->zk_name, row->zk_namelen);
		if (path == NULL) {
			free(bytes);
			zr_attr_free(&resolved);
			pk_say(pk, "%s: there is no result tree to "
			    "write into", name);
			return (ZR_PK_REDRAW);
		}
		rc = pk_write_object(path, bytes, blen, &damaged,
		    &id, err, sizeof (err));
		free(bytes);
		free(path);
		bytes = NULL;
		if (rc != 0) {
			zr_attr_free(&resolved);
			if (damaged == 0) {
				pk_say(pk, "%s: %s", name, err);
				return (ZR_PK_REDRAW);
			}
			pk_set(pk, row, ZR_CH_NONE);
			pk_say(pk, "%s: the write failed part way "
			    "(%s); the object is damaged and the "
			    "name is unanswered again", name, err);
			return (ZR_PK_REDRAW);
		}
	}

	/* (d) attributes */
	if (has_attrs) {
		char *path;
		int islink, isdir;

		path = pk_join(pk->pk_tree[ZR_PK_T_RESULT],
		    row->zk_name, row->zk_namelen);
		if (path == NULL) {
			zr_attr_free(&resolved);
			pk_say(pk, "%s: there is no result tree to "
			    "write into", name);
			return (ZR_PK_REDRAW);
		}
		islink = (kind == ZR_PK_O_LINK);
		isdir = (kind == ZR_PK_O_DIR);
		if (pk_write_attrs(path, &resolved, islink, isdir,
		    err, sizeof (err)) != 0) {
			pk_say(pk, "%s: %s", name, err);
			free(path);
			zr_attr_free(&resolved);
			return (ZR_PK_REDRAW);
		}
		free(path);
		zr_attr_free(&resolved);
	}

	/* (e) K and the document */
	if (!has_bytes) {
		char *path = pk_join(pk->pk_tree[ZR_PK_T_RESULT],
		    row->zk_name, row->zk_namelen);
		if (path != NULL) {
			struct stat st2;
			if (lstat(path, &st2) == 0) {
				id.st_dev = st2.st_dev;
				id.st_ino = st2.st_ino;
			}
			free(path);
		}
	}

	pk_set(pk, row, ZR_CH_KEEP);
	names = 1 + pk_pool_keep(pk, row, &id);
	if (zr_pk_write(pk, err, sizeof (err)) != 0) {
		pk_say(pk, "%s: written and the resolution could not "
		    "be saved: %s", name, err);
		zr_pk_merge_close(pk);
		return (ZR_PK_REDRAW);
	}
	if (names == 1)
		pk_say(pk, "%s: written, the name reads keep and the "
		    "resolution is saved", name);
	else
		pk_say(pk, "%s: written; the object has %u names "
		    "here and all %u read keep, and the resolution "
		    "is saved", name, names, names);
	zr_pk_merge_close(pk);
	return (ZR_PK_REDRAW);
}

/*
 * Every key, while a merge is open. The list's own keys mean nothing
 * here: the cursor cannot move under a merge that was opened on the
 * row it stands on, and a choice pressed by hand would be a choice
 * the merge is about to overwrite.
 */
static enum zr_pk_act
pk_merge_key(struct zr_picker *pk, enum zr_pk_key key)
{
	struct zr_pk_merge *mg = &pk->pk_merge;

	/* view switching: c for content, m for metadata */
	if (key == ZR_PK_VIEW_C) {
		if (!mg->pm_has_content) {
			pk_say(pk, "this row has no content view");
			return (ZR_PK_REDRAW);
		}
		mg->pm_view = ZR_PK_VIEW_CONTENT;
		return (ZR_PK_REDRAW);
	}
	if (key == ZR_PK_VIEW_M) {
		if (!mg->pm_has_meta) {
			pk_say(pk, "this row has no metadata view");
			return (ZR_PK_REDRAW);
		}
		mg->pm_view = ZR_PK_VIEW_META;
		return (ZR_PK_REDRAW);
	}

	/* common keys */
	switch (key) {
	case ZR_PK_BACK:
	case ZR_PK_QUIT:
		zr_pk_merge_close(pk);
		return (ZR_PK_REDRAW);
	case ZR_PK_WRITE:
		return (pk_merge_write(pk));
	case ZR_PK_TOGGLE:
		/* a: conflicts only, in either view */
		mg->pm_only = mg->pm_only == 0;
		return (ZR_PK_REDRAW);
	default:
		break;
	}

	/* view-specific keys */
	if (mg->pm_view == ZR_PK_VIEW_META) {
		switch (key) {
		case ZR_PK_PICK_FROM:
			if (zr_pk_meta_pick(&mg->pm_meta, 0) == 0)
				return (ZR_PK_REDRAW);
			return (ZR_PK_NOTHING);
		case ZR_PK_PICK_ONTO:
			if (zr_pk_meta_pick(&mg->pm_meta, 1) == 0)
				return (ZR_PK_REDRAW);
			return (ZR_PK_NOTHING);
		case ZR_PK_CLEAR:
			if (zr_pk_meta_unpick(&mg->pm_meta) == 0)
				return (ZR_PK_REDRAW);
			return (ZR_PK_NOTHING);
		case ZR_PK_NEXT:
			return (zr_pk_meta_next(&mg->pm_meta) ?
			    ZR_PK_REDRAW : ZR_PK_NOTHING);
		case ZR_PK_PREV:
			return (zr_pk_meta_prev(&mg->pm_meta) ?
			    ZR_PK_REDRAW : ZR_PK_NOTHING);
		case ZR_PK_BASE:
			/* b does nothing in the metadata view */
			return (ZR_PK_NOTHING);
		default:
			return (ZR_PK_NOTHING);
		}
	}

	/* content view keys (the original behavior) */
	switch (key) {
	case ZR_PK_PICK_FROM:
		return (pk_merge_pick(pk, ZR_M3_PICK_FROM));
	case ZR_PK_PICK_ONTO:
		return (pk_merge_pick(pk, ZR_M3_PICK_ONTO));
	case ZR_PK_CLEAR:
		return (pk_merge_pick(pk, ZR_M3_PICK_NONE));
	case ZR_PK_BASE:
		mg->pm_base = mg->pm_base == 0;
		return (ZR_PK_REDRAW);
	case ZR_PK_NEXT:
		return (pk_merge_move(pk, 0));
	case ZR_PK_PREV:
		return (pk_merge_move(pk, 1));
	default:
		return (ZR_PK_NOTHING);
	}
}

/*
 * q, and Esc with it: the picker leaves, and says what it is leaving
 * behind. Ruled by the author on 2026-09-15, on finding M9: no
 * prompt and no question -- a person who pressed q meant q -- and one
 * line naming how many answers no write has saved. It is queued like
 * every other line of the model's and printed after the terminal is
 * the person's again, which ground rule 6 is about and cell ZP68
 * asserts; with nothing unsaved there is nothing to say and nothing
 * is said. The status is the one q has always had (plan section 3.2):
 * 2, or 1 where something was written.
 */
static enum zr_pk_act
pk_quit(struct zr_picker *pk)
{
	uint32_t n = zr_pk_dirty(pk);

	if (n != 0)
		pk_say(pk, "%u answer%s not saved; the resolution on disk is "
		    "as it was", n, n == 1 ? " is" : "s are");
	return (ZR_PK_EXIT);
}

enum zr_pk_act
zr_pk_key(struct zr_picker *pk, enum zr_pk_key key)
{
	if (pk == NULL)
		return (ZR_PK_NOTHING);
	if (pk->pk_merge.pm_open != 0)
		return (pk_merge_key(pk, key));
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
	case ZR_PK_BACK:
		/* and on the list, either leaves the picker */
		return (pk_quit(pk));
	case ZR_PK_REFRESH:
		/*
		 * The screen handles the dirty question and calls
		 * zr_pk_reload directly, so this case is for a refresh
		 * with nothing unsaved: just reload.
		 */
		if (zr_pk_reload(pk, 1) == 0) {
			pk_say(pk, "%u names, %u unanswered after reload",
			    pk->pk_counts.zc_names,
			    pk->pk_counts.zc_unanswered);
		}
		return (ZR_PK_REDRAW);
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

/*
 * How many conflict hunks of the open merge hold an answer that no
 * write has taken: what Esc and q on screen 2 would drop, and what
 * the screen asks about before they do. 0 with no merge open.
 */
uint32_t
zr_pk_merge_picked(const struct zr_picker *pk)
{
	const struct zr_pk_merge *mg = &pk->pk_merge;
	uint32_t n = 0;

	if (pk == NULL || mg->pm_open == 0)
		return (0);
	if (mg->pm_has_content)
		n += mg->pm_m3.nconflict -
		    zr_m3_unpicked(&mg->pm_m3);
	if (mg->pm_has_meta)
		n += mg->pm_meta.mm_npicked;
	return (n);
}

uint32_t
zr_pk_dirty(const struct zr_picker *pk)
{
	uint32_t i, n = 0;

	if (pk == NULL)
		return (0);
	for (i = 0; i < pk->pk_nrows; i++)
		if (pk->pk_rows[i].zk_line->zl_choice !=
		    pk->pk_rows[i].zk_saved)
			n++;
	return (n);
}

enum zr_choice
zr_pk_choice(const struct zr_pk_row *row)
{
	return (row != NULL ? row->zk_line->zl_choice : ZR_CH_NONE);
}

/*
 * A scoping directory: a directory line that is not itself a conflict
 * in the manifest.  It is there because the tree grammar scopes
 * children under it, not because it has an action of its own.  A key
 * on it does nothing, and it is never counted as unanswered (ruling
 * 48).  A directory that IS a conflict in the manifest -- one the
 * manifest marks -- answers like any name.
 */
int
zr_pk_isscope(const struct zr_pk_row *row)
{
	return (row != NULL && row->zk_isdir != 0 &&
	    row->zk_kind != ZR_PK_L_CONFLICT);
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

void
zr_pk_note(struct zr_picker *pk, const char *line)
{
	if (pk == NULL || line == NULL)
		return;
	pk_say(pk, "%s", line);
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
