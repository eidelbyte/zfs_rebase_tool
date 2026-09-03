/*
 * zfsops: the ZFS operations the real run needs, wrapped so that the
 * driver speaks one vocabulary and never execs zfs(8). Hold, clone,
 * mount, property get and set, existence, release, destroy and the
 * diff, over libzfs_core and libzfs. No snapshot: the three the run
 * works from are the user's.
 *
 * NOTHING IN THE ZR_FREEBSD HALF OF THIS FILE HAS EVER BEEN COMPILED
 * FOR REAL. It is written against the headers and the sources of the
 * FreeBSD tree, and every function it calls was read there and is
 * named in a comment where it is used; the Mac can only syntax-check
 * it against the copied declarations in tests/stub. The first honest
 * compile, and the first honest test, are the box's.
 *
 * Without ZR_FREEBSD the other half compiles: every call fails with
 * "not built with ZR_FREEBSD", so main.c links and runs in --posix
 * mode on a machine that has no ZFS at all.
 */

#define	_XOPEN_SOURCE	700
#ifdef __FreeBSD__
/*
 * The libzfs headers want the BSD extensions as well as POSIX 2008,
 * and _XOPEN_SOURCE alone switches them off; src/walk.c asks the
 * same way.
 */
#define	__BSD_VISIBLE	1
#endif
#ifdef __APPLE__
#define	_DARWIN_C_SOURCE
#endif

#include <stddef.h>
#include <stdio.h>

#include "diff.h"
#include "zfsops.h"

#ifdef ZR_FREEBSD

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libnvpair.h>
#include <libzfs.h>
#include <libzfs_core.h>
#include <sys/fs/zfs.h>
#include <sys/nvpair.h>

/* A dataset name, a snapshot name and a hold tag all fit in this. */
#define	ZZ_NAME_MAX	ZFS_MAX_DATASET_NAME_LEN

struct zr_zfs {
	libzfs_handle_t	*zz_hdl;
	int		zz_cleanupfd;
	char		zz_tag[ZZ_NAME_MAX];
};

/* A failure libzfs did not record: libc's, or one of our own. */
static int
zz_err(char *err, size_t errlen, const char *what, int e)
{
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "%s: %s", what, strerror(e));
	return (-1);
}

/*
 * A failure libzfs recorded. The handle keeps the description and
 * the action of its last error, which together are what zfs(8)
 * prints; libzfs_error_description and libzfs_error_action are
 * lib/libzfs/libzfs_util.c.
 */
static int
zz_hdl_err(struct zr_zfs *z, char *err, size_t errlen, const char *what)
{
	const char *act, *desc;

	if (err == NULL || errlen == 0)
		return (-1);
	desc = libzfs_error_description(z->zz_hdl);
	act = libzfs_error_action(z->zz_hdl);
	if (desc == NULL)
		desc = "";
	if (act != NULL && act[0] != '\0')
		(void) snprintf(err, errlen, "%s: %s: %s", what, act, desc);
	else
		(void) snprintf(err, errlen, "%s: %s", what, desc);
	return (-1);
}

int
zr_zfs_open(struct zr_zfs **out, const char *holdtag, char *err, size_t errlen)
{
	struct zr_zfs *z;
	size_t n;
	int e;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (out == NULL)
		return (zz_err(err, errlen, "zfs open", EINVAL));
	*out = NULL;
	if (holdtag == NULL)
		return (zz_err(err, errlen, "zfs open", EINVAL));
	n = strlen(holdtag);
	if (n == 0 || n >= ZZ_NAME_MAX)
		return (zz_err(err, errlen, "hold tag", EINVAL));
	z = malloc(sizeof (struct zr_zfs));
	if (z == NULL)
		return (zz_err(err, errlen, "zfs open", ENOMEM));
	(void) memset(z, 0, sizeof (struct zr_zfs));
	(void) memcpy(z->zz_tag, holdtag, n + 1);
	/*
	 * libzfs_init (lib/libzfs/libzfs_util.c) loads the module,
	 * opens its own descriptor on /dev/zfs and calls
	 * libzfs_core_init for us, so the lzc_ calls below are live
	 * from here on. It returns NULL with errno set.
	 */
	z->zz_hdl = libzfs_init();
	if (z->zz_hdl == NULL) {
		e = errno;
		free(z);
		return (zz_err(err, errlen, "libzfs_init", e));
	}
	/*
	 * The cleanup descriptor. lzc_hold files every hold against
	 * it, and the kernel drops them all when it closes, however
	 * the process dies. lib/libzfs/libzfs_diff.c opens the same
	 * descriptor the same way for its just-in-time snapshot.
	 */
	z->zz_cleanupfd = open(ZFS_DEV, O_RDWR | O_CLOEXEC);
	if (z->zz_cleanupfd < 0) {
		e = errno;
		libzfs_fini(z->zz_hdl);
		free(z);
		return (zz_err(err, errlen, ZFS_DEV, e));
	}
	*out = z;
	return (0);
}

void
zr_zfs_close(struct zr_zfs *z)
{
	if (z == NULL)
		return;
	if (z->zz_cleanupfd >= 0)
		(void) close(z->zz_cleanupfd);
	if (z->zz_hdl != NULL)
		libzfs_fini(z->zz_hdl);
	free(z);
}

int
zr_zfs_hold(struct zr_zfs *z, const char *snapshot, char *err, size_t errlen)
{
	nvlist_t *errlist, *holds;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL)
		return (zz_err(err, errlen, "hold", EINVAL));
	holds = NULL;
	errlist = NULL;
	/*
	 * lzc_hold (lib/libzfs_core/libzfs_core.c): the keys are
	 * snapshot names and each value is the tag, a string. The
	 * cleanup descriptor is what makes the hold die with us.
	 */
	rc = nvlist_alloc(&holds, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_string(holds, snapshot, z->zz_tag);
	if (rc != 0) {
		nvlist_free(holds);
		return (zz_err(err, errlen, "hold", rc));
	}
	rc = lzc_hold(holds, z->zz_cleanupfd, &errlist);
	nvlist_free(holds);
	nvlist_free(errlist);
	if (rc != 0)
		return (zz_err(err, errlen, snapshot, rc));
	return (0);
}

int
zr_zfs_release(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen)
{
	nvlist_t *errlist, *holds, *tags;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL)
		return (zz_err(err, errlen, "release", EINVAL));
	holds = NULL;
	tags = NULL;
	errlist = NULL;
	/*
	 * lzc_release (lib/libzfs_core/libzfs_core.c) is shaped one
	 * level deeper than the hold: the keys are snapshot names and
	 * each value is an nvlist whose keys are the tags to remove.
	 */
	rc = nvlist_alloc(&tags, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_boolean(tags, z->zz_tag);
	if (rc == 0)
		rc = nvlist_alloc(&holds, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_nvlist(holds, snapshot, tags);
	if (rc != 0) {
		nvlist_free(holds);
		nvlist_free(tags);
		return (zz_err(err, errlen, "release", rc));
	}
	rc = lzc_release(holds, &errlist);
	nvlist_free(holds);
	nvlist_free(tags);
	nvlist_free(errlist);
	if (rc != 0)
		return (zz_err(err, errlen, snapshot, rc));
	return (0);
}

int
zr_zfs_clone(struct zr_zfs *z, const char *snapshot, const char *clone,
    const char *mountpoint, const char *manifest, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	nvlist_t *props;
	uint64_t ro;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL || clone == NULL ||
	    mountpoint == NULL || manifest == NULL)
		return (zz_err(err, errlen, "clone", EINVAL));
	/*
	 * readonly is an index property: module/zcommon/zfs_prop.c
	 * registers it with zprop_register_index, so the value the
	 * kernel wants is the uint64 index of "on" and not the word.
	 * lzc_clone hands this nvlist to ZFS_IOC_CLONE untouched, and
	 * zfs_set_prop_nvlist (module/zfs/zfs_ioctl.c), which
	 * zfs_ioc_clone applies the props with, returns EINVAL for a
	 * string whose property is not PROP_TYPE_STRING. zfs(8) never
	 * meets that because zfs_clone (lib/libzfs/libzfs_dataset.c)
	 * runs the list through zfs_valid_proplist first, whose
	 * zprop_parse_value (lib/libzfs/libzfs_util.c) converts every
	 * PROP_TYPE_INDEX string to its index and re-adds it as a
	 * uint64; off the libzfs path that conversion is ours to do.
	 * mountpoint is zprop_register_string in the same table, so it
	 * stays a string.
	 */
	ro = 0;
	if (zfs_prop_string_to_index(ZFS_PROP_READONLY, "on", &ro) != 0)
		return (zz_err(err, errlen, "clone", EINVAL));
	props = NULL;
	rc = nvlist_alloc(&props, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_uint64(props,
		    zfs_prop_to_name(ZFS_PROP_READONLY), ro);
	if (rc == 0)
		rc = nvlist_add_string(props,
		    zfs_prop_to_name(ZFS_PROP_MOUNTPOINT), mountpoint);
	/*
	 * The two markers. A user property is a name with a colon in
	 * it (zfs_prop_user, module/zcommon/zfs_prop.c) and its value
	 * must be a string: zfs_set_prop_nvlist (module/zfs/
	 * zfs_ioctl.c), which zfs_ioc_clone applies these with,
	 * returns EINVAL for a user property of any other type. It
	 * takes them on the create path exactly as on the set path,
	 * so the clone carries the marker from birth and never exists
	 * for an instant as an unmarked dataset.
	 */
	if (rc == 0)
		rc = nvlist_add_string(props, ZR_PROP_STATE, "created");
	if (rc == 0)
		rc = nvlist_add_string(props, ZR_PROP_MANIFEST, manifest);
	if (rc != 0) {
		nvlist_free(props);
		return (zz_err(err, errlen, "clone", rc));
	}
	/*
	 * lzc_clone (lib/libzfs_core/libzfs_core.c) creates it with
	 * the properties already set, so the clone is read-only from
	 * the first instant it exists and is never mounted anywhere
	 * but the private mountpoint.
	 */
	rc = lzc_clone(clone, snapshot, props);
	nvlist_free(props);
	if (rc != 0)
		return (zz_err(err, errlen, clone, rc));
	zhp = zfs_open(z->zz_hdl, clone, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, clone));
	/* zfs_mount is lib/libzfs/libzfs_mount.c. */
	if (zfs_mount(zhp, NULL, 0) != 0) {
		zfs_close(zhp);
		return (zz_hdl_err(z, err, errlen, mountpoint));
	}
	zfs_close(zhp);
	return (0);
}

int
zr_zfs_set_readonly(struct zr_zfs *z, const char *dataset, int on, char *err,
    size_t errlen)
{
	zfs_handle_t *zhp;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL)
		return (zz_err(err, errlen, "readonly", EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/* zfs_prop_set is lib/libzfs/libzfs_dataset.c. */
	rc = zfs_prop_set(zhp, zfs_prop_to_name(ZFS_PROP_READONLY),
	    on ? "on" : "off");
	zfs_close(zhp);
	if (rc != 0)
		return (zz_hdl_err(z, err, errlen, dataset));
	return (0);
}

int
zr_zfs_destroy(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL)
		return (zz_err(err, errlen, "destroy", EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * zfs_unmount (lib/libzfs/libzfs_mount.c) looks the dataset up
	 * in the mount table first and does nothing when it is not
	 * there, so this is safe on a clone that never mounted.
	 */
	rc = zfs_unmount(zhp, NULL, 0);
	zfs_close(zhp);
	if (rc != 0)
		return (zz_hdl_err(z, err, errlen, dataset));
	rc = lzc_destroy(dataset);
	if (rc != 0)
		return (zz_err(err, errlen, dataset, rc));
	return (0);
}

int
zr_zfs_exists(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL)
		return (zz_err(err, errlen, "exists", EINVAL));
	/*
	 * zfs_dataset_exists (lib/libzfs/libzfs_dataset.c) validates
	 * the name for the types asked for and then tries to make a
	 * handle: it reports, and never fails.
	 */
	if (zfs_dataset_exists(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM |
	    ZFS_TYPE_SNAPSHOT | ZFS_TYPE_VOLUME))
		return (1);
	return (0);
}

int
zr_zfs_get(struct zr_zfs *z, const char *dataset, const char *prop, char *buf,
    size_t buflen, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	zfs_prop_t p;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL || prop == NULL || buf == NULL ||
	    buflen == 0)
		return (zz_err(err, errlen, "property", EINVAL));
	buf[0] = '\0';
	/* zfs_name_to_prop is include/sys/fs/zfs.h, module/zcommon. */
	p = zfs_name_to_prop(prop);
	if (p == ZPROP_INVAL)
		return (zz_err(err, errlen, prop, EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_SNAPSHOT);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * zfs_prop_get (lib/libzfs/libzfs_dataset.c) as the string
	 * zfs(8) would print: no source wanted, no source text, and
	 * not literal, so a size comes back in the pretty form and
	 * mountpoint, mounted, casesensitivity, normalization and
	 * acltype come back as themselves.
	 */
	rc = zfs_prop_get(zhp, p, buf, buflen, NULL, NULL, 0, B_FALSE);
	zfs_close(zhp);
	if (rc != 0) {
		/*
		 * A property with neither a value nor a default fails
		 * here rather than returning something: origin on a
		 * dataset that is not a clone is registered with a
		 * NULL default (module/zcommon/zfs_prop.c), and
		 * zfs_prop_get's ZFS_PROP_ORIGIN arm returns -1 for
		 * it. zfs(8) prints "-" for exactly that case --
		 * get_callback in cmd/zfs/zfs_main.c substitutes it
		 * when zfs_prop_get fails and the property does apply
		 * to the type -- and so does this.
		 */
		if (!zfs_prop_valid_for_type(p, ZFS_TYPE_DATASET, B_FALSE))
			return (zz_hdl_err(z, err, errlen, prop));
		(void) snprintf(buf, buflen, "-");
	}
	return (0);
}

int
zr_zfs_get_int(struct zr_zfs *z, const char *dataset, const char *prop,
    uint64_t *out, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	zfs_prop_t p;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL || prop == NULL || out == NULL)
		return (zz_err(err, errlen, "property", EINVAL));
	*out = 0;
	p = zfs_name_to_prop(prop);
	if (p == ZPROP_INVAL)
		return (zz_err(err, errlen, prop, EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_SNAPSHOT);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * zfs_prop_get_int (lib/libzfs/libzfs_dataset.c) reads the
	 * number out of the handle's own stats and cannot fail; it
	 * returns 0 for a property that has no value there. The
	 * caller asks only for numeric properties of a dataset that
	 * opened, and createtxg is always in the stats.
	 */
	*out = zfs_prop_get_int(zhp, p);
	zfs_close(zhp);
	return (0);
}

int
zr_zfs_set_user(struct zr_zfs *z, const char *dataset, const char *prop,
    const char *value, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL || prop == NULL || value == NULL)
		return (zz_err(err, errlen, "property", EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * zfs_prop_set (lib/libzfs/libzfs_dataset.c) is the same call
	 * a native property goes through; zfs_valid_proplist takes
	 * the user-property arm for a name with a colon in it and
	 * passes the string down untouched.
	 */
	rc = zfs_prop_set(zhp, prop, value);
	zfs_close(zhp);
	if (rc != 0)
		return (zz_hdl_err(z, err, errlen, prop));
	return (0);
}

int
zr_zfs_get_user(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	nvlist_t *props, *val;
	const char *s;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL || prop == NULL || buf == NULL ||
	    buflen == 0)
		return (zz_err(err, errlen, "property", EINVAL));
	buf[0] = '\0';
	zhp = zfs_open(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * The user properties come back as one nvlist per property,
	 * the value under ZPROP_VALUE and the source under
	 * ZPROP_SOURCE; collect_dataset in cmd/zfs/zfs_main.c reads
	 * them the same way. The list belongs to the handle, so the
	 * string is copied out before the handle closes.
	 */
	props = zfs_get_user_props(zhp);
	rc = 0;
	if (props != NULL && nvlist_lookup_nvlist(props, prop, &val) == 0 &&
	    nvlist_lookup_string(val, ZPROP_VALUE, &s) == 0) {
		if (strlen(s) >= buflen) {
			zfs_close(zhp);
			return (zz_err(err, errlen, prop, ENAMETOOLONG));
		}
		(void) snprintf(buf, buflen, "%s", s);
		rc = 1;
	}
	zfs_close(zhp);
	return (rc);
}

int
zr_zfs_diff(struct zr_zfs *z, const char *fromsnap, const char *to,
    const char *mountpoint, struct zr_diff *out, char *err, size_t errlen)
{
	char ds[ZZ_NAME_MAX];
	zfs_handle_t *zhp;
	FILE *fp;
	size_t n;
	int fd, rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (out != NULL) {
		out->zd_entries = NULL;
		out->zd_n = 0;
	}
	if (z == NULL || fromsnap == NULL || to == NULL ||
	    mountpoint == NULL || out == NULL)
		return (zz_err(err, errlen, "diff", EINVAL));
	/*
	 * The handle wants to be the filesystem and not either
	 * snapshot: cmd/zfs/zfs_main.c's zfs_do_diff cuts the name at
	 * the '@' and opens what is left as ZFS_TYPE_FILESYSTEM.
	 */
	n = strcspn(to, "@");
	if (n >= sizeof (ds))
		return (zz_err(err, errlen, "diff", ENAMETOOLONG));
	(void) memcpy(ds, to, n);
	ds[n] = '\0';
	fp = tmpfile();
	if (fp == NULL)
		return (zz_err(err, errlen, "diff", errno));
	/*
	 * The descriptor libzfs gets is a duplicate: its differ
	 * thread fdopens what it is handed and fcloses it at the end
	 * (lib/libzfs/libzfs_diff.c), which would take our own out
	 * from under us. On the paths where zfs_show_diffs fails
	 * before that thread runs the duplicate leaks, and it is left
	 * to leak rather than closed twice; the run is over either
	 * way.
	 */
	fd = dup(fileno(fp));
	if (fd < 0) {
		rc = errno;
		(void) fclose(fp);
		return (zz_err(err, errlen, "diff", rc));
	}
	zhp = zfs_open(z->zz_hdl, ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		(void) close(fd);
		(void) fclose(fp);
		return (zz_hdl_err(z, err, errlen, ds));
	}
	/*
	 * ZFS_DIFF_CLASSIFY adds the type column that src/diff.c
	 * reads and ZFS_DIFF_PARSEABLE makes the separators tabs; the
	 * timestamp flag is the one we leave off.
	 */
	rc = zfs_show_diffs(zhp, fd, fromsnap, to,
	    ZFS_DIFF_CLASSIFY | ZFS_DIFF_PARSEABLE);
	zfs_close(zhp);
	if (rc != 0) {
		(void) fclose(fp);
		return (zz_hdl_err(z, err, errlen, "diff"));
	}
	if (fseek(fp, 0L, SEEK_SET) != 0) {
		rc = errno;
		(void) fclose(fp);
		return (zz_err(err, errlen, "diff", rc));
	}
	rc = zr_diff_parse(fp, mountpoint, out, err, errlen);
	(void) fclose(fp);
	return (rc);
}

#else	/* ZR_FREEBSD */

#define	ZZ_UNBUILT	"not built with ZR_FREEBSD"

static int
zz_unbuilt(char *err, size_t errlen)
{
	if (err != NULL && errlen > 0)
		(void) snprintf(err, errlen, "%s", ZZ_UNBUILT);
	return (-1);
}

int
zr_zfs_open(struct zr_zfs **out, const char *holdtag, char *err, size_t errlen)
{
	(void) holdtag;
	if (out != NULL)
		*out = NULL;
	return (zz_unbuilt(err, errlen));
}

void
zr_zfs_close(struct zr_zfs *z)
{
	(void) z;
}

int
zr_zfs_hold(struct zr_zfs *z, const char *snapshot, char *err, size_t errlen)
{
	(void) z;
	(void) snapshot;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_release(struct zr_zfs *z, const char *snapshot, char *err,
    size_t errlen)
{
	(void) z;
	(void) snapshot;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_clone(struct zr_zfs *z, const char *snapshot, const char *clone,
    const char *mountpoint, const char *manifest, char *err, size_t errlen)
{
	(void) z;
	(void) snapshot;
	(void) clone;
	(void) mountpoint;
	(void) manifest;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_set_readonly(struct zr_zfs *z, const char *dataset, int on, char *err,
    size_t errlen)
{
	(void) z;
	(void) dataset;
	(void) on;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_destroy(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_exists(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_get(struct zr_zfs *z, const char *dataset, const char *prop, char *buf,
    size_t buflen, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	(void) prop;
	if (buf != NULL && buflen > 0)
		buf[0] = '\0';
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_get_int(struct zr_zfs *z, const char *dataset, const char *prop,
    uint64_t *out, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	(void) prop;
	if (out != NULL)
		*out = 0;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_set_user(struct zr_zfs *z, const char *dataset, const char *prop,
    const char *value, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	(void) prop;
	(void) value;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_get_user(struct zr_zfs *z, const char *dataset, const char *prop,
    char *buf, size_t buflen, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	(void) prop;
	if (buf != NULL && buflen > 0)
		buf[0] = '\0';
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_diff(struct zr_zfs *z, const char *fromsnap, const char *to,
    const char *mountpoint, struct zr_diff *out, char *err, size_t errlen)
{
	(void) z;
	(void) fromsnap;
	(void) to;
	(void) mountpoint;
	if (out != NULL) {
		out->zd_entries = NULL;
		out->zd_n = 0;
	}
	return (zz_unbuilt(err, errlen));
}

#endif	/* ZR_FREEBSD */
