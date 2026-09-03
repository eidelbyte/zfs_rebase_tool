/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include "decide.h"

#define	ZR_NAME_MAX	1024	/* dataset names and mountpoints */

/*
 * The two sides are snapshots the user took; the tool takes none.
 * The base is not given: it is the branch point, and the run works
 * it out from the origin chains of the two. result is the dataset
 * the rebased clone is created as, and may be NULL only for a dry
 * run, which creates nothing.
 */
struct zr_run_opts {
	const char	*from;		/* pool/fs@snap */
	const char	*onto;		/* pool/fs@snap */
	const char	*result;	/* pool/fs, the clone to create */
	const char	*outpath;	/* manifest file, or NULL */
	zr_mode_t	mode;
	int		dryrun;		/* manifest only, no clone, no apply */
	int		verbose;
};

int zr_run(const struct zr_run_opts *);

#endif	/* ZR_RUN_H */
