/*
 * check_battery: the M1 gate. Reads the batteries the Python checkers
 * exported, one case per line, builds the three trees, runs
 * zr_decide in the file's mode, and compares the result pools (with
 * contents for yellow) or the conflict classes. Stops at the first
 * mismatch and prints the case.
 *
 * usage: check_battery FILE...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decide.h"
#include "name.h"

#define	MAXPOOLS	64
#define	MAXNAMES	64
#define	LINEMAX		4096

struct pool_spec {
	char		names[MAXNAMES][8];
	int		nnames;
	int		content;	/* -1 none, else 0.. for x, y, z */
};

struct tree_spec {
	struct pool_spec	pools[MAXPOOLS];
	int			npools;
};

static int failures;
static long cases;

/* Parse "({A B}x,{C})" starting at *s; advance past it. */
static int
parse_tree(const char **s, struct tree_spec *t)
{
	const char *p = *s;

	memset(t, 0, sizeof (*t));
	while (*p == ' ')
		p++;
	if (*p != '(')
		return (-1);
	p++;
	while (*p != ')') {
		struct pool_spec *ps;

		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != '{' || t->npools == MAXPOOLS)
			return (-1);
		ps = &t->pools[t->npools++];
		ps->content = -1;
		p++;
		while (*p != '}') {
			const char *q = p;
			size_t len;

			if (*p == ' ') {
				p++;
				continue;
			}
			while (*q != ' ' && *q != '}' && *q != '\0')
				q++;
			len = (size_t)(q - p);
			if (len == 0 || len >= sizeof (ps->names[0]) ||
			    ps->nnames == MAXNAMES)
				return (-1);
			memcpy(ps->names[ps->nnames], p, len);
			ps->names[ps->nnames][len] = '\0';
			ps->nnames++;
			p = q;
		}
		p++;
		if (*p >= 'a' && *p <= 'z') {
			ps->content = *p - 'x';
			if (ps->content < 0)
				ps->content = *p - 'a' + 3;
			p++;
		}
	}
	*s = p + 1;
	return (0);
}

/* Skip blanks, match the keyword, parse the tree after it. */
static int
expect_tree(const char **s, const char *kw, struct tree_spec *t)
{
	const char *p = *s;

	while (*p == ' ')
		p++;
	if (strncmp(p, kw, strlen(kw)) != 0)
		return (-1);
	p += strlen(kw);
	if (parse_tree(&p, t) != 0)
		return (-1);
	*s = p;
	return (0);
}

static int
build_tree(struct zr_tree *t, struct zr_names *names,
    const struct tree_spec *spec)
{
	int i, j;

	if (zr_tree_init(t, names) != 0)
		return (-1);
	for (i = 0; i < spec->npools; i++) {
		const struct pool_spec *ps = &spec->pools[i];
		zr_pool_t q = ZR_POOL_NONE;

		for (j = 0; j < ps->nnames; j++) {
			char path[16];
			zr_name_t n;

			(void) snprintf(path, sizeof (path), "/%s",
			    ps->names[j]);
			n = zr_names_intern(names, path, strlen(path));
			if (n == ZR_NAME_NONE)
				return (-1);
			q = zr_tree_add(t, n, (uint64_t)i + 1, ZR_T_FILE,
			    (uint32_t)ps->nnames);
			if (q == ZR_POOL_NONE)
				return (-1);
		}
		if (q != ZR_POOL_NONE)
			t->zt_pools[q].zp_content =
			    ps->content < 0 ? 0 : (uint32_t)ps->content;
	}
	return (zr_tree_seal(t));
}

static int name_str_cmp(const void *, const void *);
static const struct zr_names *sort_names;

/* Render a decision's pools as the battery writes them, for messages. */
static void
render_result(const struct zr_decision *d, const struct zr_names *names,
    int yellow, char *out, size_t cap)
{
	size_t pos = 0;
	uint32_t i, j;

	pos += (size_t)snprintf(out + pos, cap - pos, "(");
	for (i = 0; i < d->zd_npools && pos < cap; i++) {
		const struct zr_result_pool *rp = &d->zd_pools[i];

		zr_name_t sorted[MAXNAMES];
		uint32_t nn = rp->zr_nnames;

		if (nn > MAXNAMES)
			nn = MAXNAMES;

		memcpy(sorted, rp->zr_names, nn * sizeof (zr_name_t));
		sort_names = names;
		qsort(sorted, nn, sizeof (zr_name_t), name_str_cmp);
		pos += (size_t)snprintf(out + pos, cap - pos, "%s{",
		    i ? "," : "");
		for (j = 0; j < nn && pos < cap; j++) {
			size_t len;
			const char *s = zr_names_str(names, sorted[j], &len);

			pos += (size_t)snprintf(out + pos, cap - pos, "%s%s",
			    j ? " " : "", s + 1);
		}
		pos += (size_t)snprintf(out + pos, cap - pos, "}");
		if (yellow && pos < cap)
			pos += (size_t)snprintf(out + pos, cap - pos, "%c",
			    rp->zr_content == ZR_CONTENT_NONE ? '?' :
			    (char)('x' + rp->zr_content));
	}
	if (pos < cap)
		(void) snprintf(out + pos, cap - pos, ")");
}

/*
 * The classes as the battery writes them: every green class that
 * fired in the fixed order; for yellow, changed-both before disagree,
 * only one of them.
 */
static void
render_classes(const struct zr_decision *d, char *out, size_t cap)
{
	static const uint32_t order[] = { ZR_CF_HEALED_SPLIT,
	    ZR_CF_ORPHANED_ADD, ZR_CF_CONTESTED_HOME, ZR_CF_UNEXPRESSED };
	uint32_t all = 0, i;
	size_t pos = 0;

	for (i = 0; i < d->zd_ngroups; i++)
		all |= d->zd_groups[i].zg_flags;
	out[0] = '\0';
	if (all & ZR_CF_GREEN) {
		for (i = 0; i < 4; i++) {
			if (all & order[i])
				pos += (size_t)snprintf(out + pos, cap - pos,
				    "%s%s", pos ? "," : "",
				    zr_conflict_name(order[i]));
		}
		return;
	}
	if (all & ZR_CF_CHANGED_BOTH)
		(void) snprintf(out, cap, "%s",
		    zr_conflict_name(ZR_CF_CHANGED_BOTH));
	else if (all & ZR_CF_DISAGREE)
		(void) snprintf(out, cap, "%s",
		    zr_conflict_name(ZR_CF_DISAGREE));
}

static int
pool_cmp(const void *a, const void *b)
{
	return (strcmp(*(const char *const *)a, *(const char *const *)b));
}

static int
name_str_cmp(const void *a, const void *b)
{
	size_t la, lb;
	const char *x = zr_names_str(sort_names, *(const zr_name_t *)a, &la);
	const char *y = zr_names_str(sort_names, *(const zr_name_t *)b, &lb);

	return (strcmp(x, y));
}

/*
 * Compare the expected tree text with the decision by rendering both
 * as sorted pool lists; the battery's pools are sorted by first name
 * and so are ours, so textual equality after normalization suffices.
 */
static int
same_result(const char *expected, const struct zr_decision *d,
    const struct zr_names *names, int yellow)
{
	char got[LINEMAX];
	char *ep[MAXPOOLS], *gp[MAXPOOLS];
	char ebuf[LINEMAX], gbuf[LINEMAX];
	int ne = 0, ng = 0, i;
	char *p;

	render_result(d, names, yellow, got, sizeof (got));
	(void) snprintf(ebuf, sizeof (ebuf), "%s", expected);
	(void) snprintf(gbuf, sizeof (gbuf), "%s", got);
	/* split "({..}x,{..}y)" into pool strings */
	for (p = strtok(ebuf + 1, ",)"); p != NULL && ne < MAXPOOLS;
	    p = strtok(NULL, ",)"))
		ep[ne++] = p;
	for (p = strtok(gbuf + 1, ",)"); p != NULL && ng < MAXPOOLS;
	    p = strtok(NULL, ",)"))
		gp[ng++] = p;
	if (ne != ng)
		return (0);
	qsort(ep, ne, sizeof (ep[0]), pool_cmp);
	qsort(gp, ng, sizeof (gp[0]), pool_cmp);
	for (i = 0; i < ne; i++)
		if (strcmp(ep[i], gp[i]) != 0)
			return (0);
	return (1);
}

static int
run_case(const char *line, zr_mode_t mode, int yellow, const char *file,
    long lineno)
{
	struct tree_spec sb, sf, so;
	struct zr_names *names;
	struct zr_tree tb, tf, to;
	struct zr_decision d;
	const char *p = line, *arrow;
	char got[LINEMAX];
	int ok;

	if (expect_tree(&p, "base", &sb) != 0 ||
	    expect_tree(&p, "from", &sf) != 0 ||
	    expect_tree(&p, "onto", &so) != 0)
		goto bad;
	arrow = strstr(p, "=> ");
	if (arrow == NULL)
		goto bad;
	arrow += 3;
	if (strncmp(arrow, "- | ", 4) == 0)	/* the exporter's spelling */
		arrow += 4;

	names = zr_names_create();
	if (names == NULL || build_tree(&tb, names, &sb) != 0 ||
	    build_tree(&tf, names, &sf) != 0 ||
	    build_tree(&to, names, &so) != 0) {
		fprintf(stderr, "%s:%ld: cannot build trees\n", file, lineno);
		return (-1);
	}
	if (zr_decide(&tb, &tf, &to, mode, &d) != 0) {
		fprintf(stderr, "%s:%ld: zr_decide failed\n", file, lineno);
		return (-1);
	}
	if (strncmp(arrow, "conflict ", 9) == 0) {
		render_classes(&d, got, sizeof (got));
		ok = d.zd_nconflicts > 0 && strcmp(got, arrow + 9) == 0;
		if (!ok)
			(void) snprintf(got, sizeof (got), "%s",
			    d.zd_nconflicts ? got : "no conflict");
		if (ok == 0) {
			char pools[LINEMAX];

			render_result(&d, names, yellow, pools,
			    sizeof (pools));
			(void) snprintf(got + strlen(got),
			    sizeof (got) - strlen(got), " %s", pools);
		}
	} else {
		ok = d.zd_nconflicts == 0 &&
		    same_result(arrow, &d, names, yellow);
		if (!ok) {
			if (d.zd_nconflicts) {
				char cls[LINEMAX];

				render_classes(&d, cls, sizeof (cls));
				(void) snprintf(got, sizeof (got),
				    "conflict %s", cls);
			} else {
				render_result(&d, names, yellow, got,
				    sizeof (got));
			}
		}
	}
	if (!ok) {
		fprintf(stderr, "%s:%ld: MISMATCH\n  case: %s\n  got:  %s\n",
		    file, lineno, line, got);
		failures++;
	}
	zr_decision_fini(&d);
	zr_tree_fini(&tb);
	zr_tree_fini(&tf);
	zr_tree_fini(&to);
	zr_names_destroy(names);
	cases++;
	return (ok ? 0 : -1);
bad:
	fprintf(stderr, "%s:%ld: unparsable line: %s\n", file, lineno, line);
	return (-1);
}

static int
run_file(const char *file)
{
	FILE *fp = fopen(file, "r");
	char line[LINEMAX];
	zr_mode_t mode = ZR_MODE_STRICT;
	int yellow = 0, header = 0;
	long lineno = 0, before = cases;

	if (fp == NULL) {
		perror(file);
		return (-1);
	}
	while (fgets(line, sizeof (line), fp) != NULL) {
		size_t len = strlen(line);

		lineno++;
		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		if (line[0] == '#') {
			if (strstr(line, "# battery ") == line) {
				yellow = strstr(line, "yellow") != NULL;
				mode = strstr(line, "mode=permissive") ?
				    ZR_MODE_PERMISSIVE : ZR_MODE_STRICT;
				header = 1;
			}
			continue;
		}
		if (len == 0)
			continue;
		if (!header) {
			fprintf(stderr, "%s: no battery header\n", file);
			(void) fclose(fp);
			return (-1);
		}
		if (run_case(line, mode, yellow, file, lineno) != 0) {
			(void) fclose(fp);
			return (-1);
		}
	}
	(void) fclose(fp);
	printf("ok   %s (%ld cases, %s)\n", file, cases - before,
	    mode == ZR_MODE_STRICT ? "strict" : "permissive");
	return (0);
}

int
main(int argc, char **argv)
{
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: check_battery FILE...\n");
		return (2);
	}
	for (i = 1; i < argc; i++)
		if (run_file(argv[i]) != 0)
			return (1);
	printf("check_battery: %ld cases passed\n", cases);
	return (failures ? 1 : 0);
}
