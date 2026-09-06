/*
 * probe-mount: the mount and property questions section 5 of the
 * sprint-5 documents design puts to the box, in one transcript. Four
 * questions, each one deciding code:
 *
 *   (a) does zfs_mount_at mount a dataset whose mountpoint property
 *       is none at a path of the caller's choosing? The source says
 *       it checks only zfs_is_mountable_internal, which is the zoned
 *       property, and that the none and legacy refusal lives in
 *       zfs_mount alone (lib/libzfs/libzfs_mount.c).
 *   (b) with that dataset mounted there, does a readonly change
 *       succeed, attempt no remount at the stale mountpoint, and
 *       reach the live mount? statfs(2) on the path is the answer:
 *       readonly_changed_cb (module/os/freebsd/zfs/zfs_vfsops.c)
 *       sets VFS_RDONLY, which is MNT_RDONLY, on the mount itself.
 *   (c) with sharenfs off, does canmount=noauto leave an explicit
 *       zfs_mount working, and does canmount=on make libzfs mount
 *       the dataset itself?
 *   (d) the same with sharenfs on, which is the case where
 *       changelist_postfix (lib/libzfs/libzfs_changelist.c) is the
 *       reason to expect libzfs to mount it.
 *
 * DATASET must be a scratch dataset: the probe changes its
 * mountpoint, canmount, readonly and sharenfs properties, mounts and
 * unmounts it repeatedly, and restores all four at the end. DIR is
 * the private mount point; the probe creates it with mode 0700 if it
 * is missing and removes it when it is done. Every line has the shape
 *
 *     <step> <what>: rc <n>[; libzfs: <action>: <description>][; <state>]
 *
 * where <state> is where the dataset is now, because libzfs mounts
 * and unmounts of its own accord on a property change are half of
 * what is being asked. Nothing stops the transcript: a failed step is
 * printed and the restore still runs, and the exit status is 1 when a
 * property could not be put back. Not part of the tool and not built
 * by default.
 *
 *   make probe-mount
 *   sudo ./build/probe-mount POOL/scratch /var/db/zfs_rebase/probe
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libzfs.h>

/* The four properties the probe changes, and where each one lives. */
#define	PM_MOUNTPOINT	0
#define	PM_CANMOUNT	1
#define	PM_READONLY	2
#define	PM_SHARENFS	3
#define	PM_NPROPS	4

/* Room for a mount path, a property value, or libzfs's own words. */
#define	PM_LEN		1024

/* A line's trailing state clause is a path plus a few words. */
#define	PM_STATELEN	(PM_LEN + 64)

static const zfs_prop_t pm_prop[PM_NPROPS] = {
	ZFS_PROP_MOUNTPOINT,
	ZFS_PROP_CANMOUNT,
	ZFS_PROP_READONLY,
	ZFS_PROP_SHARENFS
};

static libzfs_handle_t *pm_hdl;
static const char *pm_ds;
static const char *pm_dir;
static char pm_orig[PM_NPROPS][PM_LEN];

/*
 * libzfs's own words for the last failure: the action it was
 * attempting and the description of what went wrong, the two strings
 * zfs(8) prints together.
 */
static void
pm_words(char *buf, size_t len)
{
	const char *act = libzfs_error_action(pm_hdl);
	const char *desc = libzfs_error_description(pm_hdl);

	(void) snprintf(buf, len, "%s: %s", act == NULL ? "" : act,
	    desc == NULL ? "" : desc);
}

/*
 * One property, as the string zfs(8) would print. A failure leaves a
 * question mark, because the transcript wants a value in every
 * column and never an abort.
 */
static void
pm_get(int which, char *buf, size_t len)
{
	zfs_handle_t *zhp;

	(void) snprintf(buf, len, "?");
	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL)
		return;
	/* zfs_prop_get is lib/libzfs/libzfs_dataset.c, as zr_zfs_get. */
	if (zfs_prop_get(zhp, pm_prop[which], buf, len, NULL, NULL, 0,
	    B_FALSE) != 0)
		(void) snprintf(buf, len, "?");
	zfs_close(zhp);
}

/*
 * Where the dataset is mounted, from the mount table rather than from
 * its properties: 1 and the path, 0 for not mounted, -1 and libzfs's
 * words when the handle would not open. This is zr_zfs_mounted_at's
 * call, zfs_is_mounted.
 */
static int
pm_mounted(char *buf, size_t len, char *err, size_t errlen)
{
	zfs_handle_t *zhp;
	char *where = NULL;
	int rc = 0;

	buf[0] = '\0';
	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, errlen);
		return (-1);
	}
	if (zfs_is_mounted(zhp, &where)) {
		(void) snprintf(buf, len, "%s", where == NULL ? "?" : where);
		rc = 1;
	}
	free(where);
	zfs_close(zhp);
	return (rc);
}

/* The state clause every acting line ends with. */
static void
pm_state(char *buf, size_t len)
{
	char at[PM_LEN];
	char err[PM_LEN];
	int rc;

	rc = pm_mounted(at, sizeof (at), err, sizeof (err));
	if (rc < 0)
		(void) snprintf(buf, len, "mounted unknown; libzfs: %s", err);
	else if (rc == 0)
		(void) snprintf(buf, len, "not mounted");
	else
		(void) snprintf(buf, len, "mounted at %s", at);
}

/*
 * zfs_prop_set (lib/libzfs/libzfs_dataset.c), as zr_zfs_set_readonly
 * makes it, with the state afterwards on the same line: a property
 * change runs changelist_prefix and changelist_postfix, which unmount
 * and mount on libzfs's own initiative, and that is the answer the
 * probe is here for.
 */
static int
pm_set(const char *step, int which, const char *val)
{
	zfs_handle_t *zhp;
	const char *name = zfs_prop_to_name(pm_prop[which]);
	char state[PM_STATELEN];
	char err[PM_LEN];
	int rc;

	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, sizeof (err));
		(void) printf("%s set %s=%s: rc -1; libzfs: %s\n", step, name,
		    val, err);
		return (-1);
	}
	rc = zfs_prop_set(zhp, name, val);
	zfs_close(zhp);
	pm_state(state, sizeof (state));
	if (rc == 0) {
		(void) printf("%s set %s=%s: rc 0; %s\n", step, name, val,
		    state);
	} else {
		pm_words(err, sizeof (err));
		(void) printf("%s set %s=%s: rc %d; libzfs: %s; %s\n", step,
		    name, val, rc, err, state);
	}
	return (rc);
}

/*
 * The same, but only when the property is not already what it should
 * be. A redundant set is not free: a mountpoint set still runs the
 * changelist, and changelist_postfix mounts a dataset whose canmount
 * is on, which would be a side effect the transcript did not ask for.
 */
static int
pm_set_if(const char *step, int which, const char *val)
{
	const char *name = zfs_prop_to_name(pm_prop[which]);
	char now[PM_LEN];

	pm_get(which, now, sizeof (now));
	if (strcmp(now, val) == 0) {
		(void) printf("%s set %s=%s: unchanged\n", step, name, val);
		return (0);
	}
	return (pm_set(step, which, val));
}

/*
 * zfs_unmount with no mountpoint and no flags, the way zr_zfs_unmount
 * calls it: libzfs finds the dataset in the mount table and unmounts
 * it from wherever it is, and one that is not there is nothing to do.
 */
static void
pm_unmount(const char *step)
{
	zfs_handle_t *zhp;
	char at[PM_LEN];
	char err[PM_LEN];
	int rc;

	rc = pm_mounted(at, sizeof (at), err, sizeof (err));
	if (rc < 0) {
		(void) printf("%s unmount: rc -1; libzfs: %s\n", step, err);
		return;
	}
	if (rc == 0) {
		(void) printf("%s unmount: rc 0; was not mounted\n", step);
		return;
	}
	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, sizeof (err));
		(void) printf("%s unmount: rc -1; libzfs: %s\n", step, err);
		return;
	}
	rc = zfs_unmount(zhp, NULL, 0);
	zfs_close(zhp);
	if (rc == 0) {
		(void) printf("%s unmount: rc 0; was mounted at %s\n", step,
		    at);
	} else {
		pm_words(err, sizeof (err));
		(void) printf("%s unmount: rc %d; libzfs: %s; still at %s\n",
		    step, rc, err, at);
	}
}

/*
 * zfs_mount, the plain one zr_zfs_mount makes: it reads the dataset's
 * own mountpoint property and mounts it there. zfs_is_mountable turns
 * it into a silent success for a mountpoint of none or legacy and for
 * canmount=off; canmount=noauto is not on that list, which is the
 * whole of question (c).
 */
static void
pm_mount(const char *step)
{
	zfs_handle_t *zhp;
	char state[PM_STATELEN];
	char err[PM_LEN];
	int rc;

	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, sizeof (err));
		(void) printf("%s zfs_mount: rc -1; libzfs: %s\n", step, err);
		return;
	}
	rc = zfs_mount(zhp, NULL, 0);
	zfs_close(zhp);
	pm_state(state, sizeof (state));
	if (rc == 0) {
		(void) printf("%s zfs_mount: rc 0; %s\n", step, state);
	} else {
		pm_words(err, sizeof (err));
		(void) printf("%s zfs_mount: rc %d; libzfs: %s; %s\n", step,
		    rc, err, state);
	}
}

/*
 * zfs_mount home, but only when the dataset is not already mounted:
 * zfs_mount_at has no already-mounted check of its own, and a second
 * mount at the same path would stack rather than fail.
 */
static void
pm_mount_home(const char *step)
{
	char at[PM_LEN];
	char err[PM_LEN];

	if (pm_mounted(at, sizeof (at), err, sizeof (err)) > 0) {
		(void) printf("%s zfs_mount: skipped; already mounted at %s\n",
		    step, at);
		return;
	}
	pm_mount(step);
}

/*
 * zfs_mount_at with no options and no flags, the way zr_zfs_mount_at
 * calls it: the mountpoint is given rather than read from the
 * property, the options are the defaults zfs_add_options builds from
 * the dataset's own properties, and the directory must be empty.
 */
static void
pm_mount_at(const char *step)
{
	zfs_handle_t *zhp;
	char state[PM_STATELEN];
	char err[PM_LEN];
	int rc, saved;

	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, sizeof (err));
		(void) printf("%s zfs_mount_at %s: rc -1; libzfs: %s\n", step,
		    pm_dir, err);
		return;
	}
	errno = 0;
	rc = zfs_mount_at(zhp, NULL, 0, pm_dir);
	saved = errno;
	zfs_close(zhp);
	pm_state(state, sizeof (state));
	if (rc == 0) {
		(void) printf("%s zfs_mount_at %s: rc 0; errno %d (%s); %s\n",
		    step, pm_dir, saved, strerror(saved), state);
	} else {
		pm_words(err, sizeof (err));
		(void) printf("%s zfs_mount_at %s: rc %d; errno %d (%s); "
		    "libzfs: %s; %s\n", step, pm_dir, rc, saved,
		    strerror(saved), err, state);
	}
}

/*
 * The two facts about the mount that libzfs does not report: which
 * dataset the kernel says is mounted at the path, and whether that
 * mount is read-only. MNT_RDONLY is what readonly_changed_cb sets, so
 * this line is where a readonly change that reached the kernel
 * becomes visible, and f_mntfromname is where a mount that quietly
 * went away does.
 */
static void
pm_statfs(const char *step, const char *path)
{
	struct statfs sfs;

	if (statfs(path, &sfs) != 0) {
		(void) printf("%s statfs %s: rc -1; %s\n", step, path,
		    strerror(errno));
		return;
	}
	(void) printf("%s statfs %s: rc 0; f_mntfromname %s (%s); "
	    "MNT_RDONLY %s\n", step, path, sfs.f_mntfromname,
	    strcmp(sfs.f_mntfromname, pm_ds) == 0 ? "matches" : "differs",
	    (sfs.f_flags & MNT_RDONLY) != 0 ? "set" : "clear");
}

/*
 * mkdir -p with mode 0700, so that the private mount point may be
 * named under a directory that does not exist yet. 1 when this call
 * created it, 0 when it was already there, -1 with errno set.
 */
static int
pm_mkdirp(const char *path)
{
	char buf[PM_LEN];
	struct stat st;
	size_t i;

	if (stat(path, &st) == 0)
		return (0);
	if (strlen(path) >= sizeof (buf)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	(void) snprintf(buf, sizeof (buf), "%s", path);
	for (i = 1; buf[i] != '\0'; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0700) != 0 && errno != EEXIST)
			return (-1);
		buf[i] = '/';
	}
	if (mkdir(path, 0700) != 0 && errno != EEXIST)
		return (-1);
	return (1);
}

int
main(int argc, char **argv)
{
	zfs_handle_t *zhp;
	char cur[PM_NPROPS][PM_LEN];
	char state[PM_STATELEN];
	char diff[PM_LEN];
	char err[PM_LEN];
	int i, made, unrestored = 0;

	if (argc != 3) {
		(void) fprintf(stderr, "usage: probe-mount DATASET DIR\n");
		return (2);
	}
	pm_ds = argv[1];
	pm_dir = argv[2];
	pm_hdl = libzfs_init();
	if (pm_hdl == NULL) {
		(void) fprintf(stderr, "libzfs_init: %s\n", strerror(errno));
		return (2);
	}
	libzfs_print_on_error(pm_hdl, B_FALSE);
	zhp = zfs_open(pm_hdl, pm_ds, ZFS_TYPE_FILESYSTEM);
	if (zhp == NULL) {
		pm_words(err, sizeof (err));
		(void) fprintf(stderr, "%s: %s\n", pm_ds, err);
		libzfs_fini(pm_hdl);
		return (2);
	}
	zfs_close(zhp);

	/* 0: what the dataset was, and the directory to mount it on. */
	for (i = 0; i < PM_NPROPS; i++)
		pm_get(i, pm_orig[i], sizeof (pm_orig[i]));
	(void) printf("0a properties: mountpoint=%s canmount=%s readonly=%s "
	    "sharenfs=%s\n", pm_orig[PM_MOUNTPOINT], pm_orig[PM_CANMOUNT],
	    pm_orig[PM_READONLY], pm_orig[PM_SHARENFS]);
	made = pm_mkdirp(pm_dir);
	if (made < 0) {
		(void) printf("0b mkdir %s: rc -1; %s\n", pm_dir,
		    strerror(errno));
	} else {
		(void) printf("0b mkdir %s: rc 0; %s\n", pm_dir,
		    made == 1 ? "created" : "already there");
	}
	if (strcmp(pm_orig[PM_MOUNTPOINT], "none") == 0 ||
	    strcmp(pm_orig[PM_MOUNTPOINT], "legacy") == 0) {
		(void) printf("0c note: the original mountpoint is %s, so no "
		    "home mount can be shown in steps 3, 4 and 5\n",
		    pm_orig[PM_MOUNTPOINT]);
	} else if (strcmp(pm_orig[PM_CANMOUNT], "off") == 0) {
		(void) printf("0c note: the original canmount is off, so no "
		    "home mount can be shown in steps 3, 4 and 5\n");
	} else {
		(void) printf("0c note: none\n");
	}

	/* 1: (a) the private mount of a mountpoint=none dataset. */
	pm_unmount("1a");
	(void) pm_set_if("1b", PM_MOUNTPOINT, "none");
	pm_mount_at("1c");
	pm_statfs("1d", pm_dir);

	/* 2: (b) the readonly flip while it is mounted there. */
	(void) pm_set("2a", PM_READONLY, "on");
	pm_statfs("2b", pm_dir);
	(void) pm_set("2c", PM_READONLY, "off");
	pm_statfs("2d", pm_dir);
	(void) pm_set_if("2e", PM_READONLY, pm_orig[PM_READONLY]);

	/* 3: (c) canmount with sharenfs off. */
	pm_unmount("3a");
	(void) pm_set_if("3b", PM_SHARENFS, "off");
	(void) pm_set("3c", PM_CANMOUNT, "noauto");
	(void) pm_set_if("3d", PM_MOUNTPOINT, pm_orig[PM_MOUNTPOINT]);
	pm_mount("3e");
	pm_unmount("3f");
	(void) pm_set("3g", PM_CANMOUNT, "on");
	pm_unmount("3h");

	/* 4: (d) the same with sharenfs on. */
	(void) pm_set("4a", PM_SHARENFS, "on");
	pm_unmount("4b");
	(void) pm_set("4c", PM_CANMOUNT, "noauto");
	pm_mount("4d");
	pm_unmount("4e");
	(void) pm_set("4f", PM_CANMOUNT, "on");
	pm_unmount("4g");
	(void) pm_set_if("4h", PM_SHARENFS, pm_orig[PM_SHARENFS]);

	/*
	 * 5: put it back. Every set is made while the dataset is
	 * unmounted, and the mountpoint last, so that a changelist that
	 * mounts on libzfs's initiative can only mount it at home.
	 */
	pm_unmount("5a");
	(void) pm_set_if("5b", PM_CANMOUNT, pm_orig[PM_CANMOUNT]);
	(void) pm_set_if("5c", PM_READONLY, pm_orig[PM_READONLY]);
	(void) pm_set_if("5d", PM_MOUNTPOINT, pm_orig[PM_MOUNTPOINT]);
	pm_mount_home("5e");
	diff[0] = '\0';
	for (i = 0; i < PM_NPROPS; i++) {
		pm_get(i, cur[i], sizeof (cur[i]));
		if (strcmp(cur[i], pm_orig[i]) == 0)
			continue;
		unrestored++;
		if (diff[0] != '\0')
			(void) strlcat(diff, " ", sizeof (diff));
		(void) strlcat(diff, zfs_prop_to_name(pm_prop[i]),
		    sizeof (diff));
	}
	pm_state(state, sizeof (state));
	(void) printf("5f final: mountpoint=%s canmount=%s readonly=%s "
	    "sharenfs=%s; %s\n", cur[PM_MOUNTPOINT], cur[PM_CANMOUNT],
	    cur[PM_READONLY], cur[PM_SHARENFS], state);
	if (rmdir(pm_dir) == 0) {
		(void) printf("5g rmdir %s: rc 0\n", pm_dir);
	} else {
		(void) printf("5g rmdir %s: rc -1; %s\n", pm_dir,
		    strerror(errno));
	}
	if (unrestored == 0)
		(void) printf("5h restored: yes\n");
	else
		(void) printf("5h restored: no; still changed: %s\n", diff);

	libzfs_fini(pm_hdl);
	return (unrestored == 0 ? 0 : 1);
}
