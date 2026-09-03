/* Syntax-check stub. Not part of the build. See tests/stub/README. */

#ifndef	ZR_STUB_LIBZFS_CORE_H
#define	ZR_STUB_LIBZFS_CORE_H

#include <sys/stdtypes.h>
#include <sys/nvpair.h>

#define	_LIBZFS_CORE_H

/* Verbatim from include/libzfs_core.h. */
_LIBZFS_CORE_H int libzfs_core_init(void);
_LIBZFS_CORE_H void libzfs_core_fini(void);

_LIBZFS_CORE_H int lzc_snapshot(nvlist_t *, nvlist_t *, nvlist_t **);
_LIBZFS_CORE_H int lzc_clone(const char *, const char *, nvlist_t *);

_LIBZFS_CORE_H int lzc_hold(nvlist_t *, int, nvlist_t **);
_LIBZFS_CORE_H int lzc_release(nvlist_t *, nvlist_t **);

_LIBZFS_CORE_H int lzc_destroy(const char *);

#endif	/* ZR_STUB_LIBZFS_CORE_H */
