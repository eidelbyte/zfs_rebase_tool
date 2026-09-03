/* Syntax-check stub. Not part of the build. See tests/stub/README. */

#ifndef	ZR_STUB_SYS_NVPAIR_H
#define	ZR_STUB_SYS_NVPAIR_H

#include <sys/stdtypes.h>

#define	_SYS_NVPAIR_H

/*
 * Ours: the real nvlist_t is a struct with fields nothing here
 * touches, so an incomplete type stands in for it.
 */
typedef struct nvlist nvlist_t;

/* Verbatim from include/sys/nvpair.h. */
#define	NV_UNIQUE_NAME		0x1

_SYS_NVPAIR_H int nvlist_alloc(nvlist_t **, uint_t, int);
_SYS_NVPAIR_H void nvlist_free(nvlist_t *);
_SYS_NVPAIR_H int nvlist_add_boolean(nvlist_t *, const char *);
_SYS_NVPAIR_H int nvlist_add_string(nvlist_t *, const char *, const char *);
_SYS_NVPAIR_H int nvlist_add_nvlist(nvlist_t *, const char *, const nvlist_t *);

#endif	/* ZR_STUB_SYS_NVPAIR_H */
