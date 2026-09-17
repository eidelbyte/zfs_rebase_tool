/*
 * probe-snapfd: hold a directory open, run a command beside it, and
 * ask the held descriptor afterwards. Written for the box on
 * 2026-09-16, when a --continue paused inside applying2 came back to
 * "open of the from file: Not a directory" on the from snapshot's
 * root it had held open across a --verify run beside it: the walk
 * opens names relative to a kept root descriptor (zr_walk_openat),
 * and ENOTDIR from such an openat is what a lookup through a doomed
 * directory vnode returns, which is what a forced unmount of the
 * snapshot's automount leaves behind.
 *
 *   probe-snapfd DIR NAME CMD...
 *
 * opens DIR, prints its device and inode, runs CMD through the shell,
 * prints the device and inode again through the same descriptor, then
 * opens NAME relative to it and prints the result. A dev script, not
 * part of the tool; built by hand with cc.
 */
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
say(const char *when, int fd)
{
	struct stat st;

	if (fstat(fd, &st) != 0) {
		printf("%s: fstat: %s\n", when, strerror(errno));
		return;
	}
	printf("%s: dev %llu ino %llu mode %o\n", when,
	    (unsigned long long)st.st_dev, (unsigned long long)st.st_ino,
	    (unsigned)st.st_mode);
}

int
main(int argc, char **argv)
{
	char cmd[4096];
	size_t off = 0;
	int fd, in, i, rc;

	if (argc < 4) {
		fprintf(stderr, "usage: probe-snapfd DIR NAME CMD...\n");
		return (2);
	}
	fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		printf("open %s: %s\n", argv[1], strerror(errno));
		return (1);
	}
	say("before", fd);
	cmd[0] = '\0';
	for (i = 3; i < argc; i++) {
		rc = snprintf(cmd + off, sizeof (cmd) - off, "%s%s",
		    i > 3 ? " " : "", argv[i]);
		if (rc < 0 || (size_t)rc >= sizeof (cmd) - off) {
			fprintf(stderr, "command too long\n");
			return (2);
		}
		off += (size_t)rc;
	}
	rc = system(cmd);
	printf("command: exit %d\n", rc == -1 ? -1 : WEXITSTATUS(rc));
	say("after", fd);
	in = openat(fd, argv[2], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (in < 0)
		printf("openat %s through the held root: %s\n", argv[2],
		    strerror(errno));
	else {
		printf("openat %s through the held root: ok\n", argv[2]);
		(void) close(in);
	}
	(void) close(fd);
	return (0);
}
