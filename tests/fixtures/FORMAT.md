# The fixture format (.zrt)

A fixture is one text file describing three trees -- base, from and
onto -- and the manifest a rebase of them must emit. The same file is
built two ways: as plain directories on any filesystem that has hard
links, which is how the core is tested where there is no ZFS, and
later as real datasets on FreeBSD. One file, one scenario, both
builders.

## Bytes and lines

A fixture is ASCII: no byte outside tab, space and 0x21-0x7e appears
in it, and no line is longer than the reader's patience. That is the
format's rule and not the parser's guard. What the loader refuses
outright is a NUL byte anywhere in the file and a file over 64 MiB;
what it range-checks is a file's token and an extended attribute's
name. Every other byte it passes through as itself, so a fixture
written with CRLF line endings, or with a raw 0x80 in a path, loads
and describes a tree nobody meant. Write it ASCII.

Names, symlink targets, extended attribute values and ACL text are
vis-encoded, by the one rule of v4-manifest.md section 3: a byte from
0x21 to 0x7e stands for itself, except backslash and hash; every
other byte, space and tab included, is a backslash and exactly three
octal digits.

    /a\040b       the name "/a b"
    /caf\303\251  the UTF-8 bytes of cafe with an acute e
    /\134         a backslash
    /\043         a hash

Outside the expect block, a blank line is ignored and so is a line
whose first non-blank character is a hash. A name can never begin
with a literal hash, because the encoding escapes it, so nothing is
ambiguous. Leading blanks on any line are ignored, which lets a
fixture indent its entries under their tree for the eye. Fields are
separated by runs of spaces and tabs.

## The platform line

    platform freebsd

says the fixture is that platform's alone. It may appear anywhere
before the first tree line, and only once. Two attributes below need
it, because nothing else can build them: acl=, since a text ACL is
FreeBSD's own, and an extended attribute of the system namespace,
which the Mac has no namespaces for. A fixture without the line
builds anywhere, and a fixture off its platform is refused where it
would be built: --edit-fixture says so in words, and --build-fixture
prints the errno of the refusal, "Operation not supported", since it
calls the form of the builder that takes no message.

Such a fixture lives in tests/fixtures/freebsd/ rather than in the
flat directory, which every host builds whole; tests/run-fixtures.sh
walks both and skips what this host cannot build, counting the skips
apart from the passes.

## Trees

Three lines start the three trees:

    tree base
    tree from
    tree onto

Each must appear exactly once and in that order. Everything between
one of them and the next belongs to that tree.

Inside a tree there is one entry per line:

    PATH TYPE [ARG] [mode=OCTAL] [uid=N] [gid=N] [flags=NAMES]
        [xattr=NAME:VALUE]... [acl=TEXT]

(one line in the file; wrapped here for the page)

PATH is tree-relative, written with a leading slash, vis-encoded, and
carries no trailing slash. It has no empty component and no "." or
".." component, and it holds no NUL byte, since no path can. It is a
path inside the tree and never a path on the running system: wherever
a builder puts the tree, that prefix belongs to the builder and
appears in no fixture, no manifest and no resolution. The root "/" is
implicit: it is never listed, and every other entry's parent must be
an earlier "dir" entry of the same tree. A tree lists no name twice.
The three trees are independent and may mention any names they like.

TYPE is one of five words, with the argument it takes:

    file TOKEN      a regular file whose bytes are TOKEN followed by
                    a newline. The token is opaque: nothing reads it
                    except to compare it with another token. Equal
                    tokens mean equal bytes, different tokens mean
                    different bytes, in this tree and across all
                    three.
    link TARGET     another name for the file at TARGET, which must
                    be an earlier "file" line of the same tree, and
                    nothing else: a link never points at another
                    link, a directory, a symlink or a socket.
    dir             a directory.
    symlink TARGET  a symbolic link whose target string is TARGET,
                    vis-encoded like a name. The target is a string,
                    not a reference: nothing has to exist at it. It
                    is not empty and, like a path, holds no NUL.
    sock            a unix-domain socket, made with bind(2), which
                    is the only way one is made. It takes no
                    argument and has no content of its own: what it
                    is, is its type and its attributes. bind takes
                    no mode either, so an absent mode= on a sock
                    line means what the kernel gives one, 0777 under
                    the umask, and not a file's 0644. A socket
                    address is about a hundred bytes, so the
                    directory the tree is built in plus the entry's
                    own path must fit in that or the build fails
                    with ENAMETOOLONG: keep a sock entry shallow.

A token is a run of ASCII 0x21-0x7e with no whitespace in it. It is
not vis-decoded, having no structure to protect.

Those five are the whole grammar. The engine knows three more types
-- fifo, character device and block device -- and the apply makes
them, but a fixture has no word for one, so those cells are covered
elsewhere and not here.

## The attributes

The optional attributes come last, in the order shown, each at most
once but xattr=. What is absent is the builder's default: 0755 for a
directory, 0644 for a file and 0777 for a socket, all of them under
the umask, the building process's own owner and group, no file
flags, no extended attributes and no ACL. A line holds at most 24
fields, which is PATH, TYPE, ARG and the five attribute kinds with
sixteen xattr= among them; a longer line is refused as "more than 24
fields".

    mode=OCTAL      one to four octal digits.
    uid=N           a decimal number under 2^32.
    gid=N           the same.
    flags=NAMES     the BSD file flags by their chflags(1) names,
                    comma separated. The names are handed to
                    strtofflags(3), so the vocabulary is the
                    building host's own and the two hosts differ:
                    nodump, hidden, schg, sappnd, sunlnk, uchg,
                    uappnd, opaque and arch are spelled the same on
                    FreeBSD and on the Mac, while uarch, rdonly,
                    offline, sparse, system and uunlnk are
                    FreeBSD's alone and compressed and restricted
                    are the Mac's. A name the host does not know is
                    refused at load, naming it, whether or not the
                    fixture would ever be built there -- so a
                    fixture with no platform line uses only names
                    both hosts have. A "no" name parses and is
                    dropped: this attribute says which flags are
                    on, and nothing is on that is not named.
                    ZFS holds nodump, hidden, uarch, rdonly,
                    offline, sparse, system and the system three
                    schg, sappnd and sunlnk (root's, and off again
                    only while securelevel is 0 or less); it
                    answers uchg and uappnd with EOPNOTSUPP and the
                    build fails. A fixture that runs everywhere
                    uses nodump and hidden. The archive bit
                    (uarch) is not a flag the tool sees at all:
                    ZFS sets it on every new object and every
                    write, and the walk masks it out, so an entry
                    without flags= is flag-less on ZFS too. A
                    platform with no flags at all cannot read this
                    attribute, and says so.
    xattr=NAME:VAL  one extended attribute, and the only attribute a
                    line may repeat, up to sixteen times. NAME is
                    the name the walk itself reports, "user.NAME" or
                    "system.NAME";
                    on FreeBSD that is the namespace and the bare
                    name put back together, and everywhere else it
                    is the literal name, so a built tree walks back
                    to the name the fixture wrote. It is not
                    vis-decoded, every byte of it is 0x21 to 0x7e,
                    it begins with "user." or "system." and has at
                    least one byte after that, and no more than 255.
                    A line lists its
                    attributes in bytewise name order and each name
                    once, because that is the order the walk sorts
                    them into and this format reads like the walk.
                    The split is at the first colon, so a name holds
                    none and a value may hold as many as it likes.
                    VAL is the value's bytes, vis-encoded, so a
                    value may hold any byte, including none:
                    "user.a:" is the empty value, which is a value
                    and not the absence of one. The system
                    namespace needs the platform line.
    acl=TEXT        the ACL, as the text acl_from_text(3) takes: the
                    NFSv4 form, with numeric ids, its entries
                    separated by commas. Vis-encoded, so a newline
                    or a space in it is safe; commas need no escape.
                    Needs the platform line.

Four notes the builders force.

On a symlink, mode= is ignored: no filesystem this tool targets
honours a symlink's permission bits, while uid= and gid= are set with
lchown(2), the flags with lchflags(2) and the extended attributes
with the l-forms of their own calls, and all of those do apply.

On a link line the attributes act on the file the names share, so
where two names of one pool both carry attributes the later line
wins, attribute by attribute: an extended attribute set through one
name and another set through the other leave the file with both.

The file flags go on last, after everything else and after a
directory's children exist. An immutable file takes no further
attribute and an immutable directory takes no child, so a builder
that set the flags any earlier could not finish the tree. This is
also what a harness must undo before it removes a built tree: clear
the flags first (chflags -R nouchg,nouappnd,noschg,nosappnd), or
rm(1) cannot.

An ACL is compared as its text by the content handle
zr_fixture_to_tree builds, so two fixtures that mean one ACL but
spell it differently are two contents to that handle and one to the
filesystem. (The editor is the other way round: it holds the text
against the acl_t the walk read, so it sees them as one.) Write the
same ACL the same way; and write one the mode alone could not
express, since a walk keeps an NFSv4 ACL only where it says more
than the mode does.

## The expect block

    expect

starts the expect block. The word stands alone on its line and comes
after all three tree lines. Every line after it, to the end of the
file, is the manifest the tool must emit for these three trees,
verbatim: its header lines, its tree section, its conflict section,
its blank lines and its comments, none of them ignored and none of
them rewritten. The block is optional; a fixture without one is
still a valid description of three trees. Nothing may follow it,
because nothing can: the block ends at the end of the file.

The block is also the fixture's exit status. tests/run-fixtures.sh
reads the #conflicts line out of it -- a block without one is a
failure, not a skip -- and wants exit 0 where it says 0 and exit 1
where it says anything else.

A fixture whose name ends in "-permissive" is run with -p, and its
block is the permissive-merge decision, #mode and all;
tools/regen-expect.sh follows the same rule.

The block is the whole document zfs_rebase --posix writes for these
trees, run from the directory the fixture was built in, so its
header is the posix form's: #result, #made, #tag, #take and
#written are the "-" that says the run had none of them, #form is
posix, and the three names are base, from and onto with the guid 0,
there being no snapshot to have one (v4-manifest.md, section 6).
tools/regen-expect.sh writes it and rewrites it whole whenever the
tool's output moves. It needs a built ./zfs_rebase; it skips a
fixture with a platform line unless this host is that platform and
the caller is root, as tests/run-fixtures.sh does; and it finds the
block by an "expect" at column 0, so a block written with the
leading blanks the parser tolerates is silently passed over.
--build-fixture drops the block beside the three trees as a file
named expect, which is what a harness compares against.

What the harnesses compare is the block from the #mode line on,
which is where the decision starts: above it the header is the run,
and a real run on the box names its own snapshots, its own result
and its own tag there. So the header of a block is a record of what
--posix wrote and not a thing any run must match.

## An example

    # two files, one of them hardlinked, one edit on from

    tree base
        /a file x
        /h1 file h
        /h2 link /h1

    tree from
        /a file x2
        /h1 file h
        /h2 link /h1

    tree onto
        /a file x
        /h1 file h
        /h2 link /h1
        /log file l mode=0600 uid=0

    expect
    #rebase-manifest 5
    #result -
    #form posix
    #base base 0
    #from from 0
    #onto onto 0
    #made -
    #tag -
    #take -
    #written -
    #mode strict
    ...

and one line carrying all three of the newer attributes, from a
fixture whose platform line lets it:

    /rc file c mode=0640 flags=hidden,nodump xattr=system.audit:on
        xattr=user.origin:v1 acl=user:1001:rwxp--aARWcCos:------:allow

(again one line in the file, wrapped here for the page). Its two
extended attributes are in bytewise name order, "system.audit"
before "user.origin"; its ACL text needs no escape, holding no
space, and a real one lists every entry the file is to have, as
tests/fixtures/freebsd/acl-nfsv4.zrt does; and its flags go on after
the file has its mode, its attributes and its ACL.

## Editing a built tree

    zfs_rebase --edit-fixture FIXTURE TREE DIR

turns DIR, which already holds one built tree -- typically this
fixture's base -- into the fixture's TREE, which is base, from or
onto, by the smallest set of edits it can. What the two trees agree
on is not touched: the object keeps its inode, its generation number
and its ctime, and only what differs is removed, created, relinked,
written or given new attributes.

That is the point of the mode. The tool calls an object unchanged
when its object number is the same on both snapshots and neither its
gen nor its ctime moved, so a replay that rebuilt each side from
nothing would offer every object as new and prune nothing. A side
edited in place offers real unchanged objects.

One decision is taken per name, over the union of the names DIR
holds and the names TREE lists, the root apart. The tests are made
in this order and the first that answers yes is the decision:

    removed     DIR has the name and TREE has not
    created     TREE has the name, DIR has not, and the pool it
                belongs to is new here
    relinked    the names on one object have to change: a name
                linked onto an object that stays, a name taken off
                one, or a name that leaves the pool it was in
    rewritten   the name and its pooling are right and the object
                is not: a file's bytes written through it, which
                keeps the inode, or a symlink or an object of the
                wrong type removed and made again, which does not
    attrs       only the pool's attributes differ
    untouched   nothing differs, and nothing is done

The six are printed on one line and add up to that union, which is
what makes them countable from the fixture alone. The printed order
is removed, created, rewritten, relinked, attrs, untouched, which
swaps the middle pair of the decision order above.
The pooling test comes before the object itself because it is what
decides whether an object stays the object it was: it is the "same
name set" of the unchanged rule, and it is why the survivor of a
pool broken in two is not untouched -- unlinking its fellow moved
the ctime of the object it is on.

Untouched is a property of a whole pool: if one name of a pool is
untouched then that pool's name set and its object both matched, so
every name of it did. A directory is the one thing untouched does
not make unchanged, since the kernel moves a directory's ctime when
a child is created or removed under it, as it would for any real
edit.

Two things the editor must do that a builder need not. The file
flags come off before anything is edited beneath them -- an
immutable file cannot be unlinked, written or given an attribute,
and an immutable directory can be given no child and lose none --
and they go back on last of all, children before parents, which is
the builder's own order. And the group of a new object is the group
of the directory it is made in on the BSD kernels this tool targets,
not the group of the process, so an absent gid= is resolved that way
here even though the content handle, which compares one fixture with
another and never with a filesystem, resolves it to the process's
own. A fixture that writes a gid= naming exactly the group its
object would have had anyway is therefore a fixture whose handles
say a content changed where the filesystem sees nothing; write no
such line, for the same reason a symlink's mode= is not written.

## Reading a fixture in C

src/fixture.h loads one:

    zr_fixture_load()      parse; on any violation, -1 and a message
                           naming the line number
    zr_fixture_platform()  the platform line's platform, or NULL
    zr_fixture_build()     write one tree under an existing empty
                           directory, with mkdir(2), link(2),
                           symlink(2), bind(2) and the platform's
                           own calls for the attributes POSIX never
                           standardised
    zr_fixture_build_err() the same, with a message; a fixture off
                           its platform is refused here in words
    zr_fixture_edit()      turn a directory that holds one built
                           tree into another of them, by the
                           smallest set of edits, with the six
                           counts above; a fixture off its platform
                           is refused here too
    zr_fixture_to_tree()   fill a sealed struct zr_tree from the
                           spec alone, touching no filesystem
    zr_fixture_expect()    the expect block, or NULL

The two builders agree by construction. zr_fixture_to_tree gives each
pool a synthetic inode number, an nlink equal to its name count, and
one content handle standing for everything the content oracle
compares: the type, the bytes -- a file's token, a symlink's target
string, and nothing at all for a directory or a socket -- and the
whole attribute set the pool ends up with, every
name of it folded in and every absent attribute resolved to what the
builder would have left behind. Two pools carry the same handle
exactly when all of that agrees, in one tree and across all three,
and different handles otherwise. So two files with one token and one
differing extended attribute are two contents here, as they are to
the oracle, and a fixture can say "only the attribute changed" and
have the manifest say write.
