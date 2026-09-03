# The fixture format (.zrt)

A fixture is one text file describing three trees -- base, from and
onto -- and the manifest a rebase of them must emit. The same file is
built two ways: as plain directories on any filesystem that has hard
links, which is how the core is tested where there is no ZFS, and
later as real datasets on FreeBSD. One file, one scenario, both
builders.

## Bytes and lines

A fixture is ASCII. No byte outside tab, space and 0x21-0x7e appears
in it, and no line is longer than the reader's patience.

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
builds anywhere, and --build-fixture on a platform the line does not
name fails, naming the line.

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

PATH is absolute, vis-encoded, and carries no trailing slash. It has
no empty component and no "." or ".." component, and it holds no NUL
byte, since no path can. The root "/" is implicit: it is never
listed, and every other entry's parent must be an earlier "dir" entry
of the same tree. A tree lists no name twice. The three trees are
independent and may mention any names they like.

TYPE is one of four words, with the argument it takes:

    file TOKEN      a regular file whose bytes are TOKEN followed by
                    a newline. The token is opaque: nothing reads it
                    except to compare it with another token. Equal
                    tokens mean equal bytes, different tokens mean
                    different bytes, in this tree and across all
                    three.
    link TARGET     another name for the file at TARGET, which must
                    be an earlier "file" line of the same tree. A
                    link never points at a link, a directory or a
                    symlink.
    dir             a directory.
    symlink TARGET  a symbolic link whose target string is TARGET,
                    vis-encoded like a name. The target is a string,
                    not a reference: nothing has to exist at it. It
                    is not empty and, like a path, holds no NUL.

A token is a run of ASCII 0x21-0x7e with no whitespace in it. It is
not vis-decoded, having no structure to protect.

## The attributes

The optional attributes come last, in the order shown, each at most
once but xattr=. What is absent is the builder's default: 0755 for a
directory, 0644 for a file, both under the umask, the building
process's own owner and group, no file flags, no extended attributes
and no ACL.

    mode=OCTAL      one to four octal digits.
    uid=N           a decimal number.
    gid=N           a decimal number.
    flags=NAMES     the BSD file flags by their chflags(1) names,
                    comma separated: uchg, nodump, uappnd and the
                    rest. FreeBSD and the Mac both have them and
                    name them the same way; a platform that has
                    neither cannot read this attribute at all, and
                    says so.
    xattr=NAME:VAL  one extended attribute, and the only attribute a
                    line may repeat. NAME is the name the walk
                    itself reports, "user.NAME" or "system.NAME";
                    on FreeBSD that is the namespace and the bare
                    name put back together, and everywhere else it
                    is the literal name, so a built tree walks back
                    to the name the fixture wrote. A line lists its
                    attributes in bytewise name order and each name
                    once, because that is the order the walk sorts
                    them into and this format reads like the walk.
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

An ACL is compared as its text. Two fixtures that mean one ACL but
spell it differently are two contents to this format and one to the
filesystem, so write the same ACL the same way; and write one the
mode alone could not express, since a walk keeps an NFSv4 ACL only
where it says more than the mode does.

## The expect block

    expect

starts the expect block. Every line after it, to the end of the file,
is the manifest the tool must emit for these three trees, verbatim:
its header lines, its tree section, its conflict section, its blank
lines and its comments, none of them ignored and none of them
rewritten. The block is optional; a fixture without one is still a
valid description of three trees. Nothing may follow it, because
nothing can: the block ends at the end of the file.

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
    #rebase-manifest 4
    ...

and one line carrying all three of the newer attributes, from a
fixture whose platform line lets it:

    /rc file c mode=0640 flags=uchg,nodump xattr=system.audit:on
        xattr=user.origin:v1 acl=user:1001:rwxp--aARWcCos:------:allow

(again one line in the file, wrapped here for the page). Its two
extended attributes are in bytewise name order, "system.audit"
before "user.origin"; its ACL text needs no escape, holding no
space, and a real one lists every entry the file is to have, as
tests/fixtures/freebsd/acl-nfsv4.zrt does; and its flags go on after
the file has its mode, its attributes and its ACL.

## Reading a fixture in C

src/fixture.h loads one:

    zr_fixture_load()      parse; on any violation, -1 and a message
                           naming the line number
    zr_fixture_platform()  the platform line's platform, or NULL
    zr_fixture_build()     write one tree under an existing empty
                           directory, with mkdir(2), link(2),
                           symlink(2) and the platform's own calls
                           for the attributes POSIX never
                           standardised
    zr_fixture_build_err() the same, with a message; a fixture off
                           its platform is refused here in words
    zr_fixture_to_tree()   fill a sealed struct zr_tree from the
                           spec alone, touching no filesystem
    zr_fixture_expect()    the expect block, or NULL

The two builders agree by construction. zr_fixture_to_tree gives each
pool a synthetic inode number, an nlink equal to its name count, and
one content handle standing for everything the content oracle
compares: the type, the bytes -- a file's token, a symlink's target
string -- and the whole attribute set the pool ends up with, every
name of it folded in and every absent attribute resolved to what the
builder would have left behind. Two pools carry the same handle
exactly when all of that agrees, in one tree and across all three,
and different handles otherwise. So two files with one token and one
differing extended attribute are two contents here, as they are to
the oracle, and a fixture can say "only the attribute changed" and
have the manifest say write.
