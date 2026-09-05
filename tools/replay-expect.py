#!/usr/bin/env python3
"""The replay harness's expectation: how many pools of each side of a
fixture the prune rule marks unchanged.

usage: tools/replay-expect.py [--check] [FIXTURE ...]

One line per fixture on stdout, the count for from and the count for
onto:

    tests/fixtures/probe.zrt 5 6

tests/box/run-replay.sh asserts their sum against the tool's own
"zfs_rebase: N pools unchanged" line, so this file is what turns that
line from a number into a claim. Regenerate it with make replay-expect;
make check fails when the committed tests/box/replay-expect.txt is
stale.

What is counted
---------------

The harness builds the fixture's base as a dataset, snapshots it,
clones the snapshot twice, and makes each clone into the fixture's
from and onto with --edit-fixture, which touches only what the two
trees disagree about (tests/fixtures/FORMAT.md, "Editing a built
tree"). So the base walk sees the base tree and the side walk sees
the side tree, over one object-number space, which is what makes the
rule in src/yellow.c mean anything: a side pool is unchanged when its
object number, generation number, ctime to the nanosecond, link
count, type and name set are all base's.

Per side, this file counts the side pools that come through the edit
that way:

  - the object is the one base had -- the pool keeps an object of
    base's rather than making one -- and its name set is exactly the
    base pool's, so no name of it was linked or unlinked;
  - nothing was written or set on it: the same bytes (token) for a
    file, the same target for a symlink, and the same attributes
    (mode, uid, gid, flags, xattrs, ACL, each absent one resolved the
    way the builder and the editor resolve it);
  - and, for a directory, no entry of it was created, removed,
    recreated or relinked. A directory keeps its inode when its
    children change but not its ctime, which the kernel moves for
    every name that appears in it or leaves it. A child rewritten in
    place, or given new attributes, does not touch it.

The root of the tree counts too, under the same directory rule: no
fixture describes it, so nothing is ever set on it, and it is
unchanged exactly when no top-level name appeared or left.

A pool of the side that base never had can never count, since its
object is new.

How it is computed
------------------

The decisions are src/fixture.c's, mirrored here from the spec alone:
which object each pool of the side keeps (the greedy vote in
fx_ed_claim, a pool keeping only an object of its own type and a
symlink only one already pointing where it must), what that leaves to
do (fx_ed_needs), and which names are unlinked and made again
(fx_ed_decide, fx_ed_remove, fx_ed_make). The one thing that is not
read out of a decision is the directory rule above, which is the
kernel's and not the editor's: --edit-fixture reports such a
directory as untouched, and the fixture-edit worklog says in as many
words that the harness has to subtract it.

Attributes are compared symbolically -- "the builder's default for
this type" is one value, whatever it works out to -- so the count is
the same on the Mac and on the box. The one platform rule spelled out
is the group of a new object, which is the group of the directory it
is made in on both FreeBSD and macOS (FX_GID_INHERITS in
src/fixture.c).

--check
-------

--check is the proof that this model is the tool's behaviour, as far
as the Mac can see it. For every fixture this host can build, it
builds base with the real tool, lstats every name, edits that tree
into the side with the real tool, lstats every name again, and counts
the pools that came through with their inode, generation number,
ctime, link count, type and name set intact -- the rule itself, over
a real filesystem. That count must equal this file's. The freebsd/
fixtures are skipped where they cannot be built. Three shapes the
suite has not got (INLINE below) are written out and checked the same
way, so that no branch of the model rests on nothing.

What --check cannot show is what only ZFS has: object numbers that
mean the same thing across a snapshot and its clone, a generation
number that is not always zero, and the tick-resolution ctime that
run-replay.sh's sleep exists for.
"""

import os
import stat
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(REPO, "zfs_rebase")

FX_FILE, FX_LINK, FX_DIR, FX_SYMLINK = 0, 1, 2, 3
TYPES = {"file": FX_FILE, "link": FX_LINK, "dir": FX_DIR,
         "symlink": FX_SYMLINK}

# chflags(1) spells several flags more than one way and strtofflags(3),
# which the fixture reader uses, reads the spellings as one bit. A name
# this table does not know is compared as it is written, which can only
# make the model claim a difference where there is none; --check would
# say so.
FLAG_SYNONYM = {
    "uchange": "uchg", "uimmutable": "uchg",
    "uappend": "uappnd",
    "uunlink": "uunlnk",
    "schange": "schg", "simmutable": "schg",
    "sappend": "sappnd",
    "sunlink": "sunlnk",
    "archived": "arch",
    "uhidden": "hidden",
}


def vis_decode(s):
    """A backslash and three octal digits, every other byte itself."""
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\":
            out.append(chr(int(s[i + 1:i + 4], 8)))
            i += 4
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def parent_of(path):
    """The directory one path of a fixture lives in; "/" has none."""
    if path == "/":
        return None
    cut = path.rfind("/")
    return "/" if cut == 0 else path[:cut]


class Entry(object):
    """One line of one tree."""

    def __init__(self, path, etype):
        self.path = path
        self.etype = etype
        self.token = None       # a file's bytes, opaque
        self.target = None      # a symlink's target string
        self.mode = None
        self.uid = None
        self.gid = None
        self.flags = None
        self.xattrs = []        # (name, value), in the line's order
        self.acl = None
        self.pool = -1          # the entry owning this pool


class Tree(object):
    """One of the three trees: entries in the fixture's own order."""

    def __init__(self):
        self.ents = []
        self.byname = {}        # path -> entry index
        self.members = {}       # owner -> [entry index], in order

    def add(self, e):
        i = len(self.ents)
        if e.pool < 0:
            e.pool = i
        self.byname[e.path] = i
        self.ents.append(e)
        self.members.setdefault(e.pool, []).append(i)

    def owners(self):
        return sorted(self.members)

    def names(self, owner):
        return [self.ents[i].path for i in self.members[owner]]


def parse_attrs(e, fields):
    for f in fields:
        if f.startswith("mode="):
            e.mode = int(f[5:], 8)
        elif f.startswith("uid="):
            e.uid = int(f[4:])
        elif f.startswith("gid="):
            e.gid = int(f[4:])
        elif f.startswith("flags="):
            names = [FLAG_SYNONYM.get(n, n) for n in f[6:].split(",")]
            e.flags = frozenset(names)
        elif f.startswith("xattr="):
            name, value = f[6:].split(":", 1)
            e.xattrs.append((name, vis_decode(value)))
        elif f.startswith("acl="):
            e.acl = vis_decode(f[4:])
        else:
            raise ValueError("%s: not an attribute" % f)


class Fixture(object):
    """A fixture's three trees and its platform line."""

    def __init__(self, path):
        self.path = path
        self.platform = None
        self.trees = {"base": Tree(), "from": Tree(), "onto": Tree()}
        self._load(path)

    def _load(self, path):
        with open(path, "rb") as fp:
            text = fp.read().decode("latin-1")
        cur = None
        for raw in text.split("\n"):
            line = raw.strip(" \t")
            if line == "" or line.startswith("#"):
                continue
            f = line.split()
            if f[0] == "expect":
                return
            if f[0] == "platform":
                self.platform = f[1]
                continue
            if f[0] == "tree":
                cur = self.trees[f[1]]
                continue
            self._entry(cur, f)

    def _entry(self, t, f):
        path = vis_decode(f[0])
        etype = TYPES[f[1]]
        e = Entry(path, etype)
        if etype == FX_DIR:
            rest = f[2:]
        else:
            rest = f[3:]
            if etype == FX_FILE:
                e.token = f[2]          # a token has no structure
            elif etype == FX_SYMLINK:
                e.target = vis_decode(f[2])
            else:
                # a link: another name for an earlier file of this tree
                e.pool = t.byname[vis_decode(f[2])]
        parse_attrs(e, rest)
        t.add(e)


# ---- the attributes one pool ends up with (fx_effective) -------------
#
# Every name of the pool folded in, the later name winning attribute by
# attribute, and every absent attribute resolved: the builder's default
# for the type, this process's own owner, and the group of the
# directory the object is made in. The defaults are symbols rather than
# numbers -- the builder and the editor read them from the same
# fx_defaults, so what they work out to on this host cannot change an
# answer.

DEF_MODE = ("default mode",)
DEF_UID = ("default uid",)
ROOT_GID = ("the tree root's group",)


class Eff(object):
    def __init__(self):
        self.mode = None
        self.uid = None
        self.gid = None
        self.flags = frozenset()
        self.xattrs = {}
        self.acl = None

    def attrs_same(self, other, etype):
        """fx_ed_attr_same: everything but the file flags, and not a
        symlink's mode, which nothing here honours."""
        if etype != FX_SYMLINK and self.mode != other.mode:
            return False
        if self.uid != other.uid or self.gid != other.gid:
            return False
        if self.xattrs != other.xattrs:
            return False
        return self.acl == other.acl


def effective(tree, owner, cache):
    """The pool's attributes, with its group resolved down the tree."""
    if owner in cache:
        return cache[owner]
    ef = Eff()
    has_gid = False
    for i in tree.members[owner]:
        e = tree.ents[i]
        if e.mode is not None:
            ef.mode = e.mode
        if e.uid is not None:
            ef.uid = e.uid
        if e.gid is not None:
            ef.gid = e.gid
            has_gid = True
        if e.flags is not None:
            ef.flags = e.flags
        if e.acl is not None:
            ef.acl = e.acl
        for name, value in e.xattrs:
            ef.xattrs[name] = value
    if ef.mode is None:
        ef.mode = (DEF_MODE, tree.ents[owner].etype == FX_DIR)
    if tree.ents[owner].etype == FX_SYMLINK:
        ef.mode = 0
    if ef.uid is None:
        ef.uid = DEF_UID
    if not has_gid:
        ef.gid = inherited_gid(tree, owner, cache)
    cache[owner] = ef
    return ef


def inherited_gid(tree, owner, cache):
    """A new object takes the group of the directory it is made in, and
    a pool is made at the first name the fixture gives it. The builder
    gives a directory its attributes before it fills it, so the
    fixture's own gid= for that directory is the answer; the root is no
    fixture's, so the walk's group is."""
    up = parent_of(tree.ents[owner].path)
    if up is None or up == "/":
        return ROOT_GID
    return effective(tree, tree.ents[tree.byname[up]].pool, cache).gid


# ---- the edit, from the spec alone -----------------------------------

FE_FRESH, FE_LINKS, FE_WRITE, FE_ATTR, FE_FLAGS = 1, 2, 4, 8, 16


def side_plan(fx, side):
    """What --edit-fixture would do to a directory holding base, as
    far as the count needs it: which base object each side pool keeps,
    what is left to do to it, and which names are unlinked and made.
    """
    base = fx.trees["base"]
    tree = fx.trees[side]
    beff = {}
    seff = {}

    # ed_nm and ed_dp: the object each name of the side is on today.
    dp = []
    for e in tree.ents:
        j = base.byname.get(e.path)
        dp.append(None if j is None else base.ents[j].pool)

    def fits(owner, bp):
        """fx_ed_fits: an object of the pool's own type, and, a symlink
        having no content that can be written, one already pointing
        where it must. A pool is owned by a file, a dir or a symlink
        line, never by a link line, so the two types compare directly.
        """
        so = tree.ents[owner]
        bo = base.ents[bp]
        if so.etype != bo.etype:
            return False
        if so.etype != FX_SYMLINK:
            return True
        return so.target == bo.target

    # fx_ed_claim: a name already on an object its pool could keep is a
    # vote for it; the pool with the most votes keeps it, ties to the
    # pool the fixture lists first.
    votes = []
    index = {}
    for i, e in enumerate(tree.ents):
        owner = e.pool
        if dp[i] is None or not fits(owner, dp[i]):
            continue
        key = (owner, dp[i])
        if key not in index:
            index[key] = len(votes)
            votes.append([owner, dp[i], 0])
        votes[index[key]][2] += 1
    claim = {}
    taken = set()
    while True:
        best = None
        for k, (owner, bp, n) in enumerate(votes):
            if n == 0 or bp in taken or owner in claim:
                continue
            if best is None or n > votes[best][2]:
                best = k
        if best is None:
            break
        claim[votes[best][0]] = votes[best][1]
        taken.add(votes[best][1])

    def sameset(owner, bp):
        """fx_ed_sameset: the names on that object are exactly the
        names the fixture gives this pool."""
        if bp is None:
            return False
        bnames = base.names(bp)
        if len(bnames) != len(tree.members[owner]):
            return False
        for p in bnames:
            j = tree.byname.get(p)
            if j is None or tree.ents[j].pool != owner:
                return False
        return True

    # fx_ed_needs, pool by pool.
    need = {}
    for owner in tree.owners():
        bp = claim.get(owner)
        n = 0
        if bp is None:
            n |= FE_FRESH
        else:
            if not sameset(owner, bp):
                n |= FE_LINKS
            if tree.ents[owner].etype == FX_FILE and \
                    tree.ents[owner].token != base.ents[bp].token:
                n |= FE_WRITE
        cur = fresh_eff(tree, owner, seff) if bp is None else \
            effective(base, bp, beff)
        ef = effective(tree, owner, seff)
        if not ef.attrs_same(cur, tree.ents[owner].etype):
            n |= FE_ATTR
        if ef.flags != cur.flags:
            n |= FE_FLAGS
        need[owner] = n

    # fx_ed_decide: the names that are unlinked, and with them the
    # names that have to be made or made again.
    gone = set()
    for i, e in enumerate(tree.ents):
        if dp[i] is not None and dp[i] != claim.get(e.pool):
            gone.add(e.path)
    for p in base.byname:
        if p not in tree.byname:
            gone.add(p)
    made = set(e.path for i, e in enumerate(tree.ents)
               if dp[i] is None or e.path in gone)
    return claim, need, gone, made


def fresh_eff(tree, owner, cache):
    """fx_ed_newattr: what an object made here starts out as."""
    ef = Eff()
    ef.mode = (DEF_MODE, tree.ents[owner].etype == FX_DIR)
    if tree.ents[owner].etype == FX_SYMLINK:
        ef.mode = 0
    ef.uid = DEF_UID
    ef.gid = inherited_gid(tree, owner, cache)
    return ef


def prune_count(fx, side):
    """How many pools of that side the rule marks unchanged."""
    tree = fx.trees[side]
    claim, need, gone, made = side_plan(fx, side)
    # Every name that appears in a directory or leaves it moves that
    # directory's ctime, whatever the editor did or did not do to the
    # directory itself.
    stirred = set()
    for p in gone | made:
        up = parent_of(p)
        if up is not None:
            stirred.add(up)
    n = 0
    for owner in tree.owners():
        if claim.get(owner) is None or need[owner] != 0:
            continue
        if tree.ents[owner].etype == FX_DIR and \
                tree.ents[owner].path in stirred:
            continue
        n += 1
    if "/" not in stirred:
        n += 1          # the root, which no fixture describes
    return n


# ---- the fixtures ----------------------------------------------------

def fixture_paths():
    """Every fixture, the flat directory before the freebsd one, each
    in bytewise order, which is the order of the expectation file."""
    out = []
    for sub in ("tests/fixtures", "tests/fixtures/freebsd"):
        d = os.path.join(REPO, sub)
        for name in sorted(os.listdir(d)):
            if name.endswith(".zrt"):
                out.append(sub + "/" + name)
    return out


def emit(paths, out):
    for rel in paths:
        fx = Fixture(os.path.join(REPO, rel))
        out.write("%s %d %d\n" % (rel, prune_count(fx, "from"),
                                  prune_count(fx, "onto")))


# ---- --check: the model against the tool on this host ----------------

def scan(root):
    """Every name under root, the root as "/": what the walk keeps of
    it, and what zo_unmoved reads."""
    names = {}
    stack = [(root, b"/")]
    while stack:
        path, rel = stack.pop()
        st = os.lstat(path)
        if stat.S_ISDIR(st.st_mode):
            kind = FX_DIR
            nlink = 1       # a directory's link count is its subdirs
            for ent in os.listdir(path):
                stack.append((path + b"/" + ent,
                              (b"" if rel == b"/" else rel) + b"/" + ent))
        elif stat.S_ISLNK(st.st_mode):
            kind = FX_SYMLINK
            nlink = st.st_nlink
        elif stat.S_ISREG(st.st_mode):
            kind = FX_FILE
            nlink = st.st_nlink
        else:
            raise ValueError("%s: not a file of any type the walk knows"
                             % path)
        names[rel] = (st.st_ino, getattr(st, "st_gen", 0), st.st_ctime_ns,
                      nlink, kind)
    return names


def pools_of(names):
    """Names grouped by object number, which is what a pool is."""
    out = {}
    for nm, rec in names.items():
        out.setdefault(rec[0], []).append(nm)
    return out


def unmoved(before, after):
    """zr_oracle_prune over two scans of one tree: the pools of the
    second that the first holds under the same object number,
    generation, ctime, link count, type and names."""
    bpools = pools_of(before)
    n = 0
    for ino, snames in pools_of(after).items():
        first = snames[0]
        if first not in before:
            continue        # a name base never had
        brec = before[first]
        srec = after[first]
        if brec != srec:
            continue
        bnames = bpools[brec[0]]
        if len(bnames) != len(snames):
            continue
        if any(nm not in before or before[nm][0] != brec[0]
               for nm in snames):
            continue
        n += 1
    return n


def run(args):
    p = subprocess.run(args, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError("%s: exit %d: %s" % (" ".join(args),
                           p.returncode, p.stderr.decode("latin-1").strip()))


def scrub(d):
    """The file flags come off before a built tree can be removed."""
    subprocess.run(["chflags", "-R", "nouchg,nouappnd,noschg,nosappnd", d],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["rm", "-rf", d], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)


def check_side(path, side):
    """Build base with the tool, edit it into the side with the tool,
    and count what came through unmoved."""
    work = tempfile.mkdtemp(prefix="zr-replay.")
    try:
        run([BIN, "--build-fixture", path, work])
        tree = os.path.join(work, "base").encode("latin-1")
        before = scan(tree)
        run([BIN, "--edit-fixture", path, side, tree.decode("latin-1")])
        return unmoved(before, scan(tree))
    finally:
        scrub(work)


# Three shapes the fixture suite does not have, and the model's answer
# for them rests on a branch nothing else reaches: a directory below
# the root that keeps everything of its own while a child comes or
# goes, a pool whose file flags are the only thing that differs, and a
# symlink pointing somewhere else, which cannot be written in place and
# so is made again. --check builds and edits these too, so that the
# three branches are held against the filesystem like the rest.
INLINE = [
    ("a directory whose children change", """
tree base
	/d dir
	/d/a file x
	/d/b file y
	/keep file k
tree from
	/d dir
	/d/a file x
	/keep file k
tree onto
	/d dir
	/d/a file x
	/d/b file y
	/d/c file z
	/keep file k
"""),
    ("the file flags alone", """
tree base
	/f file x
	/g file y
tree from
	/f file x flags=hidden
	/g file y
tree onto
	/f file x
	/g file y
"""),
    ("a symlink pointing somewhere else", """
tree base
	/s symlink /a
	/k file x
tree from
	/s symlink /b
	/k file x
tree onto
	/s symlink /a
	/k file x
"""),
]


def check_inline():
    """The same comparison over the shapes above, written out as
    fixtures of their own."""
    bad = 0
    d = tempfile.mkdtemp(prefix="zr-replay-inline.")
    try:
        for i, (what, text) in enumerate(INLINE):
            path = os.path.join(d, "inline%d.zrt" % i)
            with open(path, "w") as fp:
                fp.write(text)
            fx = Fixture(path)
            for side in ("from", "onto"):
                want = prune_count(fx, side)
                got = check_side(path, side)
                if want != got:
                    sys.stderr.write("%s (%s): the model says %d, the "
                                     "tool leaves %d\n" %
                                     (what, side, want, got))
                    bad += 1
    finally:
        scrub(d)
    return bad


def check(paths):
    if not os.access(BIN, os.X_OK):
        sys.stderr.write("replay-expect: no %s: make first\n" % BIN)
        return 2
    bad = skipped = done = 0
    bad += check_inline()
    for rel in paths:
        path = os.path.join(REPO, rel)
        fx = Fixture(path)
        if fx.platform is not None and sys.platform != "freebsd":
            skipped += 1
            continue
        for side in ("from", "onto"):
            want = prune_count(fx, side)
            got = check_side(path, side)
            if want != got:
                sys.stderr.write("%s %s: the model says %d, the tool "
                                 "leaves %d\n" % (rel, side, want, got))
                bad += 1
        done += 1
    sys.stdout.write("replay-expect --check: %d fixtures and %d shapes "
                     "of its own, %d sides, %d skipped off their "
                     "platform, %d disagree\n" %
                     (done, len(INLINE), 2 * (done + len(INLINE)),
                      skipped, bad))
    return 1 if bad else 0


def main(argv):
    args = argv[1:]
    checking = False
    if args and args[0] == "--check":
        checking = True
        args = args[1:]
    paths = args if args else fixture_paths()
    paths = [p[len(REPO) + 1:] if p.startswith(REPO + "/") else p
             for p in paths]
    if checking:
        return check(paths)
    emit(paths, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
