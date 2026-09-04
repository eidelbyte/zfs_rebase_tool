/*
 * zfsops: the ZFS operations the real run needs, wrapped so that the
 * driver speaks one vocabulary and never execs zfs(8). Hold, clone,
 * mount, property get and set, existence, release and destroy, over
 * libzfs_core and libzfs. No snapshot: the three the run works from
 * are the user's, and the holds it takes on them outlive the
 * process, because a rebase does.
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

/*
 * The libzfs handle, and one descriptor on /dev/zfs kept only for
 * the temporary holds a --verify takes: the kernel gives such a hold
 * back when that descriptor closes, and closing it is the one thing
 * a dying process always does. It is opened on the first temporary
 * hold and never otherwise.
 */
struct zr_zfs {
	libzfs_handle_t	*zz_hdl;
	int		zz_cleanupfd;
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

/*
 * The two index values zfsops.h publishes for the driver, held
 * against the property tables libzfs_init has just built. run.c
 * compares casesensitivity and normalization with numbers and has no
 * ZFS header to read them from, so this is where those numbers
 * answer to ZFS itself: zfs_prop_string_to_index is the registered table --
 * zprop_string_to_index over the index table zfs_prop_init filled in
 * (module/zcommon/zfs_prop.c), which libzfs_init calls
 * (lib/libzfs/libzfs_util.c) -- so this is not a second copy of the
 * mapping but the mapping. mounted is not in the table: it is a
 * boolean and "not 0" is the whole of it.
 */
static int
zz_index_check(char *err, size_t errlen)
{
	static const struct {
		zfs_prop_t	zi_prop;
		const char	*zi_word;
		uint64_t	zi_want;
	} tab[] = {
		{ ZFS_PROP_CASE, "sensitive", ZR_CASE_SENSITIVE },
		{ ZFS_PROP_NORMALIZE, "none", ZR_NORMALIZE_NONE }
	};
	uint64_t idx;
	size_t i;

	for (i = 0; i < sizeof (tab) / sizeof (tab[0]); i++) {
		idx = (uint64_t)-1;
		if (zfs_prop_string_to_index(tab[i].zi_prop, tab[i].zi_word,
		    &idx) == 0 && idx == tab[i].zi_want)
			continue;
		if (err != NULL && errlen > 0) {
			(void) snprintf(err, errlen, "this ZFS does not put "
			    "%s=%s at index %llu", zfs_prop_to_name(
			    tab[i].zi_prop), tab[i].zi_word,
			    (unsigned long long)tab[i].zi_want);
		}
		return (-1);
	}
	return (0);
}

int
zr_zfs_open(struct zr_zfs **out, char *err, size_t errlen)
{
	struct zr_zfs *z;
	int e;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (out == NULL)
		return (zz_err(err, errlen, "zfs open", EINVAL));
	*out = NULL;
	z = malloc(sizeof (struct zr_zfs));
	if (z == NULL)
		return (zz_err(err, errlen, "zfs open", ENOMEM));
	(void) memset(z, 0, sizeof (struct zr_zfs));
	z->zz_cleanupfd = -1;
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
	if (zz_index_check(err, errlen) != 0) {
		libzfs_fini(z->zz_hdl);
		free(z);
		return (-1);
	}
	*out = z;
	return (0);
}

void
zr_zfs_close(struct zr_zfs *z)
{
	if (z == NULL)
		return;
	/*
	 * The cleanup descriptor goes with the handle, and with it
	 * every temporary hold filed against it. The persistent holds
	 * a rebase takes are filed against nothing and stay.
	 */
	if (z->zz_cleanupfd >= 0)
		(void) close(z->zz_cleanupfd);
	if (z->zz_hdl != NULL)
		libzfs_fini(z->zz_hdl);
	free(z);
}

/*
 * One hold, filed against cleanupfd. lzc_hold (lib/libzfs_core/
 * libzfs_core.c): the keys are snapshot names and each value is the
 * tag, a string. A cleanup descriptor of -1 is left out of the
 * ioctl's arguments altogether, and the kernel then files an
 * ordinary user hold that nothing but a release takes away; a real
 * descriptor makes the hold the kernel's to give back when that
 * descriptor closes.
 */
static int
zz_hold(struct zr_zfs *z, const char *snapshot, const char *tag, int cleanupfd,
    char *err, size_t errlen)
{
	nvlist_t *errlist, *holds;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL || tag == NULL || tag[0] == '\0')
		return (zz_err(err, errlen, "hold", EINVAL));
	holds = NULL;
	errlist = NULL;
	rc = nvlist_alloc(&holds, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_string(holds, snapshot, tag);
	if (rc != 0) {
		nvlist_free(holds);
		return (zz_err(err, errlen, "hold", rc));
	}
	rc = lzc_hold(holds, cleanupfd, &errlist);
	nvlist_free(holds);
	nvlist_free(errlist);
	if (rc != 0)
		return (zz_err(err, errlen, snapshot, rc));
	return (0);
}

int
zr_zfs_hold(struct zr_zfs *z, const char *snapshot, const char *tag, char *err,
    size_t errlen)
{
	return (zz_hold(z, snapshot, tag, -1, err, errlen));
}

int
zr_zfs_hold_tmp(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen)
{
	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL)
		return (zz_err(err, errlen, "hold", EINVAL));
	if (z->zz_cleanupfd < 0) {
		/*
		 * An open of /dev/zfs of our own. zfs_ioc_hold takes
		 * the descriptor with zfs_onexit_fd_hold (module/zfs/
		 * zfs_ioctl.c), finds the onexit state of that minor
		 * and files the hold against it, so the kernel drops
		 * the hold when the descriptor closes -- which the
		 * death of the process does, whatever kills it.
		 * libzfs opens it exactly this way for zfs send's own
		 * temporary holds (lib/libzfs/libzfs_sendrecv.c) and
		 * for the diff (lib/libzfs/libzfs_diff.c).
		 */
		z->zz_cleanupfd = open(ZFS_DEV, O_RDWR | O_CLOEXEC);
		if (z->zz_cleanupfd < 0)
			return (zz_err(err, errlen, ZFS_DEV, errno));
	}
	return (zz_hold(z, snapshot, tag, z->zz_cleanupfd, err, errlen));
}

int
zr_zfs_release(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen)
{
	nvlist_t *errlist, *holds, *tags;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL || tag == NULL || tag[0] == '\0')
		return (zz_err(err, errlen, "release", EINVAL));
	holds = NULL;
	tags = NULL;
	errlist = NULL;
	/*
	 * lzc_release (lib/libzfs_core/libzfs_core.c) is shaped one
	 * level deeper than the hold: the keys are snapshot names and
	 * each value is an nvlist whose keys are the tags to remove.
	 * A tag that is not there, and a snapshot that is not there,
	 * go on the errlist without failing the call
	 * (dsl_dataset_user_release_check, module/zfs/dsl_userhold.c),
	 * so releasing twice is not an error and --abort can be run
	 * again over a half-aborted run.
	 */
	rc = nvlist_alloc(&tags, NV_UNIQUE_NAME, 0);
	if (rc == 0)
		rc = nvlist_add_boolean(tags, tag);
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
    const char *mountpoint, const struct zr_rebase_record *rec, char *err,
    size_t errlen)
{
	char bg[24], fg[24], og[24];
	zfs_handle_t *zhp;
	nvlist_t *props;
	uint64_t ro;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || snapshot == NULL || clone == NULL ||
	    mountpoint == NULL || rec == NULL || rec->base == NULL ||
	    rec->from == NULL || rec->onto == NULL || rec->made == NULL ||
	    rec->mode == NULL || rec->form == NULL || rec->tag == NULL ||
	    rec->verify == NULL || rec->manifest == NULL)
		return (zz_err(err, errlen, "clone", EINVAL));
	/* a guid is a uint64; the record spells every value out */
	(void) snprintf(bg, sizeof (bg), "%llu",
	    (unsigned long long)rec->base_guid);
	(void) snprintf(fg, sizeof (fg), "%llu",
	    (unsigned long long)rec->from_guid);
	(void) snprintf(og, sizeof (og), "%llu",
	    (unsigned long long)rec->onto_guid);
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
	 * The record. A user property is a name with a colon in it
	 * (zfs_prop_user, module/zcommon/zfs_prop.c) and its value
	 * must be a string: zfs_set_prop_nvlist (module/zfs/
	 * zfs_ioctl.c), which zfs_ioc_clone applies these with,
	 * returns EINVAL for a user property of any other type. It
	 * takes them on the create path exactly as on the set path,
	 * so the clone carries the whole record from birth. zfs_ioc_
	 * clone makes the head and then applies these with
	 * ZPROP_SRC_LOCAL, and destroys the head again if that fails,
	 * so a create that half-worked leaves nothing behind and the
	 * source of every one of them is the dataset itself, which is
	 * what zr_zfs_get_user demands of a record. There is no state
	 * among them: the state is written at the gates the run
	 * passes.
	 */
	if (rc == 0) {
		const struct {
			const char	*name;
			const char	*val;
		} rp[] = {
			{ ZR_PROP_BASE, rec->base },
			{ ZR_PROP_BASE_GUID, bg },
			{ ZR_PROP_FROM, rec->from },
			{ ZR_PROP_FROM_GUID, fg },
			{ ZR_PROP_ONTO, rec->onto },
			{ ZR_PROP_ONTO_GUID, og },
			{ ZR_PROP_MADE, rec->made },
			{ ZR_PROP_MODE, rec->mode },
			{ ZR_PROP_FORM, rec->form },
			{ ZR_PROP_TAG, rec->tag },
			{ ZR_PROP_VERIFY, rec->verify },
			{ ZR_PROP_MANIFEST, rec->manifest }
		};
		size_t i, n = sizeof (rp) / sizeof (rp[0]);

		for (i = 0; rc == 0 && i < n; i++)
			rc = nvlist_add_string(props, rp[i].name, rp[i].val);
	}
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
zr_zfs_mount(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || dataset == NULL)
		return (zz_err(err, errlen, "mount", EINVAL));
	zhp = zfs_open(z->zz_hdl, dataset, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, dataset));
	/*
	 * zfs_mount (lib/libzfs/libzfs_mount.c) reads the dataset's
	 * own mountpoint property and mounts it there; a mountpoint of
	 * none or legacy, or canmount=off, is nothing to do and not a
	 * failure. The caller asks only for a dataset whose mounted
	 * property said no.
	 */
	rc = zfs_mount(zhp, NULL, 0);
	zfs_close(zhp);
	if (rc != 0)
		return (zz_hdl_err(z, err, errlen, dataset));
	return (0);
}

/* What the guid walk is looking for, and where the answer goes. */
struct zz_guid {
	uint64_t	zg_want;
	char		*zg_buf;
	size_t		zg_buflen;
	int		zg_toolong;
};

/*
 * One snapshot. The guid is a number in the handle's own stats
 * (dsl_dataset_fast_stat fills dds_guid, and zfs_prop_get_int's
 * ZFS_PROP_GUID arm reads it), so a simple handle is enough and no
 * property list is asked for. Returning non-zero stops the walk,
 * which is what "the first match wins" means.
 */
static int
zz_guid_snap(zfs_handle_t *zhp, void *arg)
{
	struct zz_guid *g = arg;
	const char *name;
	int found = 0;

	if (zfs_prop_get_int(zhp, ZFS_PROP_GUID) == g->zg_want) {
		name = zfs_get_name(zhp);
		if (strlen(name) >= g->zg_buflen)
			g->zg_toolong = 1;
		else
			(void) snprintf(g->zg_buf, g->zg_buflen, "%s", name);
		found = 1;
	}
	zfs_close(zhp);
	return (found);
}

/*
 * One filesystem: its own snapshots first, then the filesystems
 * under it. The iterator hands the callback a handle the callback
 * owns, so each one is closed here, including on the way out of a
 * walk that found what it wanted.
 */
static int
zz_guid_fs(zfs_handle_t *zhp, void *arg)
{
	int rc;

	rc = zfs_iter_snapshots(zhp, B_TRUE, zz_guid_snap, arg, 0, 0);
	if (rc == 0)
		rc = zfs_iter_filesystems(zhp, zz_guid_fs, arg);
	zfs_close(zhp);
	return (rc);
}

int
zr_zfs_find_guid(struct zr_zfs *z, const char *pool, uint64_t guid, char *buf,
    size_t buflen, char *err, size_t errlen)
{
	struct zz_guid g;
	zfs_handle_t *zhp;
	int rc;

	if (err != NULL && errlen > 0)
		err[0] = '\0';
	if (z == NULL || pool == NULL || buf == NULL || buflen == 0)
		return (zz_err(err, errlen, "guid", EINVAL));
	buf[0] = '\0';
	(void) memset(&g, 0, sizeof (g));
	g.zg_want = guid;
	g.zg_buf = buf;
	g.zg_buflen = buflen;
	zhp = zfs_open(z->zz_hdl, pool, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return (zz_hdl_err(z, err, errlen, pool));
	rc = zfs_iter_snapshots(zhp, B_TRUE, zz_guid_snap, &g, 0, 0);
	if (rc == 0)
		rc = zfs_iter_filesystems(zhp, zz_guid_fs, &g);
	zfs_close(zhp);
	/*
	 * The iterators return what the callback returned, and their
	 * own failures as a negative number (lib/libzfs/libzfs_iter.c).
	 */
	if (rc < 0)
		return (zz_hdl_err(z, err, errlen, pool));
	if (rc == 0)
		return (0);
	if (g.zg_toolong != 0)
		return (zz_err(err, errlen, "guid", ENAMETOOLONG));
	return (1);
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
	const char *s, *src;
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
	 * ZPROP_SOURCE; get_callback in cmd/zfs/zfs_main.c reads them
	 * the same way. The kernel always puts a source there
	 * (dsl_prop_get_all_impl, module/zfs/dsl_prop.c), and it is
	 * the name of the dataset the value was set on: this
	 * dataset's own name for a local value, an ancestor's name
	 * for an inherited one, and the string "$recvd"
	 * (ZPROP_SOURCE_VAL_RECVD) for a received one. Only the first
	 * is ours. A user property inherits down the naming tree, so
	 * without this test a zfs_rebase:tag set on a pool would make
	 * every dataset under it look like a result; a received value
	 * is not ours either, since it names another machine's
	 * snapshots and another machine's manifest path.
	 *
	 * The list belongs to the handle, so the string is copied out
	 * before the handle closes.
	 */
	props = zfs_get_user_props(zhp);
	rc = 0;
	if (props != NULL && nvlist_lookup_nvlist(props, prop, &val) == 0 &&
	    nvlist_lookup_string(val, ZPROP_VALUE, &s) == 0 &&
	    nvlist_lookup_string(val, ZPROP_SOURCE, &src) == 0 &&
	    strcmp(src, dataset) == 0) {
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
zr_zfs_open(struct zr_zfs **out, char *err, size_t errlen)
{
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
zr_zfs_hold(struct zr_zfs *z, const char *snapshot, const char *tag, char *err,
    size_t errlen)
{
	(void) z;
	(void) snapshot;
	(void) tag;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_hold_tmp(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen)
{
	(void) z;
	(void) snapshot;
	(void) tag;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_find_guid(struct zr_zfs *z, const char *pool, uint64_t guid, char *buf,
    size_t buflen, char *err, size_t errlen)
{
	(void) z;
	(void) pool;
	(void) guid;
	if (buf != NULL && buflen > 0)
		buf[0] = '\0';
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_mount(struct zr_zfs *z, const char *dataset, char *err, size_t errlen)
{
	(void) z;
	(void) dataset;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_release(struct zr_zfs *z, const char *snapshot, const char *tag,
    char *err, size_t errlen)
{
	(void) z;
	(void) snapshot;
	(void) tag;
	return (zz_unbuilt(err, errlen));
}

int
zr_zfs_clone(struct zr_zfs *z, const char *snapshot, const char *clone,
    const char *mountpoint, const struct zr_rebase_record *rec, char *err,
    size_t errlen)
{
	(void) z;
	(void) snapshot;
	(void) clone;
	(void) mountpoint;
	(void) rec;
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

#endif	/* ZR_FREEBSD */
