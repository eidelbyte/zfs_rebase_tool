/* The real run's options and entry point. */

#ifndef ZR_RUN_H
#define	ZR_RUN_H

#include "decide.h"

#define	ZR_NAME_MAX	1024	/* dataset names and mountpoints */

struct zr_run_opts {
	const char	*base;		/* pool/fs@snap */
	const char	*from;		/* pool/fs */
	const char	*onto;		/* pool/fs */
	const char	*outpath;	/* manifest file, or NULL for stdout */
	zr_mode_t	mode;
	int		dryrun;		/* manifest only, no clone, no apply */
	int		verbose;
};

int zr_run(const struct zr_run_opts *);

#endif	/* ZR_RUN_H */
