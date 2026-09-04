PORTNAME=	zfs_rebase
DISTVERSIONPREFIX=	v
DISTVERSION=	0.1.0
CATEGORIES=	sysutils

MAINTAINER=	mail@eidelbyte.net
COMMENT=	Rebase one ZFS filesystem onto another
WWW=		https://github.com/eidelbyte/zfs_rebase_tool

LICENSE=	BSD3CLAUSE
LICENSE_FILE=	${WRKSRC}/LICENSE

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

# No TEST_TARGET. make check-freebsd in ${WRKSRC} is the gate, but its
# FreeBSD fixtures want root and a /tmp that can hold NFSv4 ACLs and
# system-namespace extended attributes, which is a box and not a
# build jail.

.include <bsd.port.pre.mk>

.if !exists(${SRC_BASE}/sys/contrib/openzfs/include/libzfs.h)
IGNORE=	needs the FreeBSD source tree in ${SRC_BASE}: install src.txz there, or set SRC_BASE to a checkout
.endif

# The upstream install target honors DESTDIR and PREFIX and puts the
# binary in sbin and the page in share/man/man8, which is where this
# tree keeps them; pkg-plist names the page uncompressed, the way the
# tree has listed man pages since it stopped compressing them.
do-install:
	@cd ${WRKSRC} && ${MAKE_CMD} ${MAKE_ARGS} install \
		DESTDIR=${STAGEDIR} PREFIX=${PREFIX}

.include <bsd.port.post.mk>
