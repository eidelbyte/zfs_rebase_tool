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

Names and symlink targets are vis-encoded, by the one rule of
v4-manifest.md section 3: a byte from 0x21 to 0x7e stands for itself,
except backslash and hash; every other byte, space and tab included,
is a backslash and exactly three octal digits.

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

## Trees

Three lines start the three trees:

    tree base
    tree from
    tree onto

Each must appear exactly once and in that order. Everything between
one of them and the next belongs to that tree.

Inside a tree there is one entry per line:

    PATH TYPE [ARG] [mode=OCTAL] [uid=N] [gid=N]

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

The optional attributes come last, in the order shown, each at most
once. mode= takes one to four octal digits, uid= and gid= a decimal
number. What is absent is the builder's default: 0755 for a
directory, 0644 for a file, both under the umask, and the building
process's own owner and group.

Two notes the builders force. On a symlink, mode= is ignored: no
filesystem this tool targets honours a symlink's permission bits,
while uid= and gid= are set with lchown(2) and do apply. On a link
line the attributes act on the file the names share, so where two
names of one pool both carry attributes the later line wins.

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

## Reading a fixture in C

src/fixture.h loads one:

    zr_fixture_load()     parse; on any violation, -1 and a message
                          naming the line number
    zr_fixture_build()    write one tree under an existing empty
                          directory, with mkdir(2), link(2) and
                          symlink(2)
    zr_fixture_to_tree()  fill a sealed struct zr_tree from the spec
                          alone, touching no filesystem
    zr_fixture_expect()   the expect block, or NULL

The two builders agree by construction. zr_fixture_to_tree gives each
pool a synthetic inode number, an nlink equal to its name count, and
a content handle taken from the token: equal tokens get equal
handles across all three trees, a directory gets one fixed handle,
and a symlink gets its target's handle with a high bit set, so that a
symlink and a file are never confused for equal content.
