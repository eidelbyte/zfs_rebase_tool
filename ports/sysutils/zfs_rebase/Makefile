PORTNAME=	zfs_rebase
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.0
CATEGORIES=	sysutils

MAINTAINER=	mail@eidelbyte.net
COMMENT=	Rebase one ZFS filesystem onto another
WWW=		https://github.com/eidelbyte/zfs_rebase_tool

# Two licenses, both applying: the tool is BSD3CLAUSE, and the copy of
# FreeBSD's contrib/libdiff that the picker's merge builds against is
# ISC. That copy is src/plugins/picker/libdiff -- Neels Hofmeyr's diff
# library from the Game of Trees project, carried whole and never
# edited (see its UPSTREAM), together with the two OpenBSD compat
# allocators under the same terms. It is compiled into the binary, so
# it is a license of the package and not only of the source.
#
# Both lines stay whichever way the PICKER option below is set: the
# distfile is one tarball and carries libdiff either way, and a
# package built without the picker is a package built from that same
# source. What changes with the option off is only that nothing of
# libdiff is compiled in.
LICENSE=	BSD3CLAUSE ISC
LICENSE_COMB=	multi
LICENSE_FILE_BSD3CLAUSE=	${WRKSRC}/LICENSE
LICENSE_FILE_ISC=	${WRKSRC}/src/plugins/picker/libdiff/LICENCE

USE_GITHUB=	yes
GH_ACCOUNT=	eidelbyte
GH_PROJECT=	zfs_rebase_tool

# DISTVERSION is a placeholder: no release is tagged yet, and distinfo
# says the same. The first tag sets both, the second by make makesum.
#
# No USES=gmake: the upstream Makefile is plain POSIX make -- $(VAR)
# references, explicit rules, no GNU-only functions -- and builds with
# FreeBSD's own make. No LIB_DEPENDS either: libzfs, libzfs_core and
# libnvpair are in base, and the tool links against nothing else.
#
# The build does need the FreeBSD source tree rather than the
# installed headers alone: libzfs.h assumes libspl's Solaris types and
# sys/avl.h, sys/fs/zfs.h and sys/mnttab.h, none of which land in
# /usr/include, so the port compiles against sys/contrib/openzfs the
# way FreeBSD's own zfs(8) is built. No package provides a source
# tree, so this is a check and a note rather than a BUILD_DEPENDS.
# SRC_BASE is what ports call it; ZFS_SRC is what the upstream
# Makefile calls it.
ALL_TARGET=	freebsd
MAKE_ARGS=	ZFS_SRC=${SRC_BASE}

# The built-in picker is an option, on by default (tracker issue
# port-picker-option, plan section 8 item 14).
#
# On: the tool carries the picker, links ncursesw from base -- the
# freebsd target passes CURSES_LIBS=-lncursesw, which is what base's
# own programs link -- and the standalone zfs_rebase-picker is
# installed beside it. USES=ncurses:base says so to the framework and
# adds no dependency, since the library is in base.
#
# Off: PICKER=no goes through MAKE_ARGS to the build and to the
# install, and the tool is built with the upstream stub in the
# picker's place: no model, no screens, no merge, no libdiff, no
# curses library on any link line and no standalone binary. -i CMD is
# the same flag it always was, and -i with no command says this build
# has no picker and exits 2, which leaves the conflicts gate standing
# like any other non-zero exit from the child.
#
# **To confirm on the box** with tests/box/run-port.sh, which builds
# the port both ways: some framework versions want the port to say
# nothing at all about a base library, and portlint or the build will
# say which.
OPTIONS_DEFINE=	PICKER
OPTIONS_DEFAULT=	PICKER
PICKER_DESC=	Built-in ncurses conflict picker for -i

PICKER_USES=	ncurses:base
PICKER_MAKE_ARGS_OFF=	PICKER=no
PICKER_PLIST_FILES=	sbin/zfs_rebase-picker

# No TEST_TARGET. make check-freebsd in ${WRKSRC} is the gate, but its
# FreeBSD fixtures want root and a /tmp that can hold NFSv4 ACLs and
# system-namespace extended attributes, which is a box and not a
# build jail.

.include <bsd.port.pre.mk>

.if !exists(${SRC_BASE}/sys/contrib/openzfs/include/libzfs.h)
IGNORE=	needs the FreeBSD source tree in ${SRC_BASE}: install src.txz there, or set SRC_BASE to a checkout
.endif

# The upstream install target honors DESTDIR and PREFIX and puts the
# tool in sbin, the standalone picker beside it where it was built,
# and the page in share/man/man8, which is where this tree keeps
# them. The framework compresses the page at stage, so
# pkg-plist names it with its .gz (check-plist on the box,
# 2026-09-08: "Orphaned: share/man/man8/zfs_rebase.8.gz").
#
# zfs_rebase-picker is the PICKER option's own file, so it is in
# PICKER_PLIST_FILES above and not in pkg-plist: with the option off
# the upstream install target installs the tool and the page and
# nothing else, by the same PICKER=no it was built with.
#
# It goes in sbin beside the tool and not in libexec:
# the tool never runs it -- --interactive forks and calls the picker's
# entry in its own child -- so it is not a private helper of the
# package but a program an administrator runs by hand, on a resolution
# and the manifest beside it, and it needs root to read the trees for
# the same reason the tool does. libexec is for what a package runs
# and a person does not.
do-install:
	@cd ${WRKSRC} && ${MAKE_CMD} ${MAKE_ARGS} install \
		DESTDIR=${STAGEDIR} PREFIX=${PREFIX}

.include <bsd.port.post.mk>
