#!/usr/bin/env python3
"""Generate a large three-tree rebase scenario as plain directories.

usage: tools/gen-big-tree.py [--names N] [--dirs D] [--pools P]
           [--change PCT] [--size-mean BYTES] [--seed S] OUTDIR

Writes OUTDIR/base, OUTDIR/from and OUTDIR/onto: a base tree of N
names over D directories, of which P pools carry two to four names
apiece (hard links), and two sides that each changed PCT of the base
objects.  The trees are ordinary directories, so this feeds
zfs_rebase --posix anywhere, and on FreeBSD it feeds three datasets
once they have been copied in.

Everything is a function of --seed and the other options, so the same
command line makes byte-identical trees on any host: sizes and the
choice of what changes come from one seeded Random drawn in a fixed
order, and a file's bytes come from a fixed master block of random
bytes plus a marker derived from its content id.  Equal content ids
mean equal bytes and different ones differ, in the first chunk except
where a tail edit was asked for.

What each side does to the base, all of it disjoint from the other
side's except the overlapping edits:

    modify     PCT of the objects, one-sided and therefore clean
    overlap    a fixed fraction of that count, changed by both sides
               to different bytes: these are the conflicts
    add        a fixed fraction, a name the base never had
    remove     a fixed fraction, a single-name object unlinked
    link       a fixed fraction of the pools gains a name, and
               another fixed fraction loses one

A modification rewrites the object's bytes at its original size.
That is deliberate: the content oracle compares sizes before it reads
anything, so an edit that changed the size would be decided without a
single read and would measure nothing.  Of the modifications, a small
fraction differ from the base only in their last chunk, and those are
given a size of several chunks; the rest differ from the first chunk.
The two together exercise the streaming comparison's early exit from
both ends.

Sizes are lognormal with the arithmetic mean --size-mean, so most
files are far smaller than the mean and a few are much larger.  The
default of 200000 names at a mean of 2048 bytes is a few hundred
megabytes of data per tree, plus whatever the filesystem spends on
200000 inodes -- three trees, so reckon on a couple of gigabytes.
"""
import argparse
import hashlib
import math
import os
import random
import sys
import time

# The oracle reads in chunks of this size (ZO_CHUNK in src/yellow.c).
# A file that must differ only in its last chunk needs more than one.
CHUNK = 128 * 1024

# The master block a file's bytes are cut from, and the largest file
# that may be cut from it.  Sizes are clamped to MAXSIZE.
MASTER = 32 * 1024 * 1024
MAXSIZE = 4 * 1024 * 1024

# The shape of the size distribution: lognormal, this sigma, and a mu
# chosen so that the arithmetic mean is --size-mean.
SIGMA = 1.5

# The fixed fractions, all of them of the per-side modification count
# except the two link fractions, which are of the pool count.
OVERLAP_FRAC = 0.05
ADD_FRAC = 0.10
RM_FRAC = 0.10
TAIL_FRAC = 0.01
LINK_ADD_FRAC = 0.05
LINK_RM_FRAC = 0.05

# Directory levels below the root.
DEPTH = 3

# Content id spaces, kept apart so that no two of them can collide.
CID_FROM_MOD = 1 << 40
CID_ONTO_MOD = 2 << 40
CID_FROM_ADD = 3 << 40
CID_ONTO_ADD = 4 << 40


def h64(cid):
    """A 64-bit value from a content id, for the offset into master."""
    d = hashlib.blake2b(cid.to_bytes(8, "little"), digest_size=8).digest()
    return int.from_bytes(d, "little")


def marker(cid):
    """The 16 bytes that make one content id's bytes its own."""
    return hashlib.blake2b(cid.to_bytes(8, "little"), digest_size=16).digest()


def build_dirs(ndirs):
    """A balanced tree of ndirs directory paths, the root first as ''."""
    dirs = [""]
    if ndirs <= 1:
        return dirs
    fan = 2
    while sum(fan ** i for i in range(1, DEPTH + 1)) < ndirs - 1:
        fan += 1
    frontier = [""]
    while len(dirs) < ndirs and frontier:
        nxt = []
        for parent in frontier:
            for j in range(fan):
                if len(dirs) >= ndirs:
                    break
                leaf = "d%02d" % j
                path = leaf if parent == "" else parent + "/" + leaf
                dirs.append(path)
                nxt.append(path)
            if len(dirs) >= ndirs:
                break
        frontier = nxt
    return dirs


def draw_size(rng, mean):
    """One lognormal size in bytes, at least one and at most MAXSIZE.

    The mu below is the one that puts the arithmetic mean of the
    distribution at mean, which is what --size-mean promises; the
    median sits far below it, so most files are small.
    """
    mu = math.log(mean) - SIGMA * SIGMA / 2.0
    v = int(rng.lognormvariate(mu, SIGMA))
    if v < 1:
        v = 1
    if v > MAXSIZE:
        v = MAXSIZE
    return v


class Plan(object):
    """The whole scenario, decided before a single byte is written."""

    def __init__(self, args):
        rng = random.Random(args.seed)
        self.args = args
        nnames = args.names
        ndirs = args.dirs
        npools = args.pools

        self.dirs = build_dirs(ndirs)
        ndirs = len(self.dirs)
        self.path = [self.namepath(i, "f%07d" % i) for i in range(nnames)]

        # The pools: npools of them, two to four names each, drawn
        # from anywhere in the tree so that a pool spans directories.
        sizes = [rng.randint(2, 4) for _ in range(npools)]
        need = sum(sizes)
        if need > nnames:
            raise SystemExit("gen-big-tree: %d pools want %d of %d names"
                             % (npools, need, nnames))
        members = rng.sample(range(nnames), need)
        of_pool = {}
        pools = []
        at = 0
        for k in sizes:
            group = sorted(members[at:at + k])
            at += k
            for nm in group:
                of_pool[nm] = len(pools)
            pools.append(group)

        # Objects, in name order: a pool's first name opens it, the
        # rest join it, and every other name is an object of its own.
        self.objs = []
        opened = {}
        for i in range(nnames):
            p = of_pool.get(i)
            if p is None:
                self.objs.append([i])
            elif p not in opened:
                opened[p] = len(self.objs)
                self.objs.append(list(pools[p]))
        nobj = len(self.objs)
        self.size = [draw_size(rng, args.size_mean) for _ in range(nobj)]

        # The change sets, all disjoint, drawn in one sample so that
        # no object is asked to do two things at once.
        nmod = int(round(nobj * args.change / 100.0))
        nover = int(round(nmod * OVERLAP_FRAC))
        nadd = int(round(nmod * ADD_FRAC))
        nrm = int(round(nmod * RM_FRAC))
        nlink_add = int(round(npools * LINK_ADD_FRAC))
        nlink_rm = int(round(npools * LINK_RM_FRAC))
        want = 2 * nmod + nover + 2 * nrm + 2 * nlink_add
        if want > nobj:
            raise SystemExit("gen-big-tree: the changes want %d of %d "
                             "objects; lower --change" % (want, nobj))
        picked = rng.sample(range(nobj), want)
        cut = [0]

        def take(n):
            """The next n of the one disjoint sample, in order."""
            got = picked[cut[0]:cut[0] + n]
            cut[0] += n
            return got

        self.mod = {}
        self.rm = {}
        self.linkadd = {}
        self.linkrm = {}
        self.add = {}
        for side, cidbase in (("from", CID_FROM_MOD), ("onto", CID_ONTO_MOD)):
            self.mod[side] = {j: cidbase + j for j in take(nmod)}
        self.overlap = take(nover)
        for j in self.overlap:
            self.mod["from"][j] = CID_FROM_MOD + j
            self.mod["onto"][j] = CID_ONTO_MOD + j
        # Removals unlink a single-name object, so that a removal is
        # one name gone and not a pool torn up.
        for side in ("from", "onto"):
            self.rm[side] = set(j for j in take(nrm)
                                if len(self.objs[j]) == 1)
        for side, letter in (("from", "f"), ("onto", "o")):
            got = take(nlink_add)
            self.linkadd[side] = dict(
                (j, self.namepath(k * 11 + 5, "l%s%06d" % (letter, k)))
                for k, j in enumerate(got))
        # Link removals have to come from the pools, since a
        # single-name object has no name to spare: they are drawn
        # from the multi-name objects the sample above left alone.
        taken = set(picked)
        spare = [j for j in range(nobj)
                 if len(self.objs[j]) > 1 and j not in taken]
        nlink_rm = min(nlink_rm, len(spare) // 2)
        drawn = rng.sample(spare, 2 * nlink_rm)
        self.linkrm["from"] = set(drawn[:nlink_rm])
        self.linkrm["onto"] = set(drawn[nlink_rm:])

        # A fraction of each side's modifications differ from the base
        # only in the last chunk, which needs a file of several
        # chunks: those objects are resized here, in the base too.
        self.tail = {}
        for side in ("from", "onto"):
            keys = sorted(self.mod[side])
            ntail = int(round(len(keys) * TAIL_FRAC))
            step = max(1, len(keys) // ntail) if ntail else 1
            chosen = keys[::step][:ntail]
            self.tail[side] = set(chosen)
            for j in chosen:
                self.size[j] = 2 * CHUNK + rng.randrange(2 * CHUNK)

        # The names each side invents, which the base never had.
        for side, letter, cidbase in (("from", "f", CID_FROM_ADD),
                                      ("onto", "o", CID_ONTO_ADD)):
            made = []
            for k in range(nadd):
                leaf = "a%s%06d" % (letter, k)
                made.append((self.namepath(k * 7 + 3, leaf),
                             draw_size(rng, args.size_mean), cidbase + k))
            self.add[side] = made

        self.nnames = nnames
        self.ndirs = ndirs
        self.npools = npools
        self.nobj = nobj
        self.nmod = nmod
        self.nover = nover
        self.nadd = nadd

    def namepath(self, i, leaf):
        d = self.dirs[i % len(self.dirs)]
        return leaf if d == "" else d + "/" + leaf

    def names_of(self, side, j):
        """The names object j has on side, base order, minus any drop."""
        names = [self.path[i] for i in self.objs[j]]
        if side != "base":
            if j in self.linkrm[side] and len(names) > 1:
                names = names[:-1]
            extra = self.linkadd[side].get(j)
            if extra is not None:
                names = names + [extra]
        return names

    def content_of(self, side, j):
        """(content id, base content id or None for a tail edit)."""
        if side == "base":
            return (j, None)
        cid = self.mod[side].get(j)
        if cid is None:
            return (j, None)
        return (cid, j if j in self.tail[side] else None)


class Writer(object):
    """Bytes for a content id, cut from one master block of noise."""

    def __init__(self, seed):
        self.master = random.Random(seed ^ 0x5eed).randbytes(MASTER)

    def blob(self, cid, size):
        off = h64(cid) % (MASTER - MAXSIZE)
        b = bytearray(self.master[off:off + size])
        m = marker(cid)
        n = min(len(m), size)
        b[0:n] = m[0:n]
        return b

    def tail_blob(self, base_cid, cid, size):
        b = self.blob(base_cid, size)
        m = marker(cid)
        n = min(len(m), size)
        b[size - n:size] = m[0:n]
        return b


def write_file(root, rel, data):
    path = os.path.join(root, rel)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        at = 0
        view = memoryview(data)
        while at < len(data):
            at += os.write(fd, view[at:])
    finally:
        os.close(fd)


def materialize(outdir, side, plan, writer):
    """One tree written whole. Returns the bytes of file data it wrote."""
    root = os.path.join(outdir, side)
    os.mkdir(root)
    for d in plan.dirs:
        if d != "":
            os.mkdir(os.path.join(root, d))
    total = 0
    dropped = plan.rm[side] if side != "base" else frozenset()
    for j in range(plan.nobj):
        if j in dropped:
            continue
        names = plan.names_of(side, j)
        if not names:
            continue
        size = plan.size[j]
        cid, tail_of = plan.content_of(side, j)
        if tail_of is None:
            data = writer.blob(cid, size)
        else:
            data = writer.tail_blob(tail_of, cid, size)
        write_file(root, names[0], data)
        total += size
        for extra in names[1:]:
            os.link(os.path.join(root, names[0]), os.path.join(root, extra))
    if side != "base":
        for rel, size, cid in plan.add[side]:
            write_file(root, rel, writer.blob(cid, size))
            total += size
    return total


def main():
    ap = argparse.ArgumentParser(
        description="generate a large three-tree scenario as directories",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--names", type=int, default=200000,
                    help="names in the base tree (default 200000)")
    ap.add_argument("--dirs", type=int, default=None,
                    help="directories (default names/100)")
    ap.add_argument("--pools", type=int, default=None,
                    help="hard link pools of 2 to 4 names "
                         "(default names/100)")
    ap.add_argument("--change", type=float, default=2.0,
                    help="percent of objects each side modifies "
                         "(default 2)")
    ap.add_argument("--size-mean", type=int, default=2048,
                    help="mean file size in bytes (default 2048)")
    ap.add_argument("--seed", type=int, default=1,
                    help="the seed the whole scenario comes from "
                         "(default 1)")
    ap.add_argument("outdir", metavar="OUTDIR")
    args = ap.parse_args()
    if args.names < 1:
        raise SystemExit("gen-big-tree: --names wants at least one name")
    if args.dirs is None:
        args.dirs = max(1, args.names // 100)
    if args.pools is None:
        args.pools = max(1, args.names // 100)
    if args.dirs < 1 or args.pools < 0:
        raise SystemExit("gen-big-tree: --dirs and --pools want a count")
    if args.size_mean < 1:
        raise SystemExit("gen-big-tree: --size-mean wants bytes")

    started = time.time()
    os.mkdir(args.outdir)
    plan = Plan(args)
    writer = Writer(args.seed)
    wrote = 0
    for side in ("base", "from", "onto"):
        wrote += materialize(args.outdir, side, plan, writer)
    elapsed = time.time() - started

    nrm = len(plan.rm["from"]), len(plan.rm["onto"])
    nla = len(plan.linkadd["from"]), len(plan.linkadd["onto"])
    nlr = len(plan.linkrm["from"]), len(plan.linkrm["onto"])
    ntail = len(plan.tail["from"]), len(plan.tail["onto"])
    print("gen-big-tree: seed %d names %d dirs %d objects %d pools %d "
          "mean %d bytes %d | per side: mod %d tail %d/%d add %d rm %d/%d "
          "link+ %d/%d link- %d/%d | overlap %d | %.1fs"
          % (args.seed, plan.nnames, plan.ndirs, plan.nobj, plan.npools,
             args.size_mean, wrote, plan.nmod, ntail[0], ntail[1],
             plan.nadd, nrm[0], nrm[1], nla[0], nla[1], nlr[0], nlr[1],
             plan.nover, elapsed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
