#!/usr/bin/env python3
"""Generate the history fixtures and validate them against the checkers.

usage: ZR_THEORY=/path/to/zfs-rebase-theory tools/gen-history.py REPO

Each case is base/from/onto in the notes' notation. The fixture is
written, built as directories, run through zfs_rebase --posix, and the
manifest is replayed on the onto tree in Python; the replayed result
(or the conflict classes) must equal what v4-yellowcheck.py says.
"""
import importlib.util, os, re, subprocess, sys, tempfile

THEORY = os.environ.get("ZR_THEORY",
    "/Users/miri/freebsd-development/zfs-rebase-theory")
def load(n):
    s = importlib.util.spec_from_file_location(n, os.path.join(THEORY, n + ".py"))
    m = importlib.util.module_from_spec(s); s.loader.exec_module(m); return m
G = load("v4-greencheck"); Y = load("v4-yellowcheck")

POOL = re.compile(r"\{([^{}]*)\}([a-z]?)")

def parse_tree(s):
    """'({A B}x,{C}y)' or '({AB}x)' -> tuple of (frozenset names, content)"""
    out = []
    for names, c in POOL.findall(s):
        ns = names.split() if " " in names else list(names.strip())
        out.append((frozenset(ns), c or "c"))
    return tuple(out)

# ---- the cases: (group, slug, base, from, onto, modes) -----------------
CASES = []
def case(group, slug, b, f, o, modes=("strict",)):
    CASES.append((group, slug, b, f, o, modes))

# sprint-2 catalog and design principles (v4-review-against-history.md s2)
case("s2", "recycle", "({AB}x)", "({C}y)", "({AB}x)")
case("s2", "phantom", "({ABCXY}x)", "({ABC}y,{X}x,{Y}x)", "({CXY}y,{A}x,{B}x)")
case("s2", "sever-vs-nothing", "({ABC})", "({BC},{A})", "({ABC})")
case("s2", "sever-and-join", "({ABC},{DE})", "({BC},{A},{DE})", "({BC},{ADE})")
case("s2", "novel-same-content", "()", "({AB}x)", "({CD}x)")
case("s2", "both-sever", "({ABC})", "({BC},{A})", "({BC},{A})")
case("s2", "breakup-identical", "({AB}x)", "({B}x,{A}x)", "({AB}x)")
case("s2", "same-path-new-bytes", "({AB}x)", "({B}x,{A}y)", "({AB}x)")
case("s2", "divergent-membership", "({P}p,{AB}a,{CD}c)", "({ABP}a,{CD}c)", "({AB}a,{CDP}c)")
case("s2", "divergent-equal", "({P}a,{AB}a,{CD}a)", "({ABP}a,{CD}a)", "({AB}a,{CDP}a)", ("strict", "permissive"))
case("s2", "delete-vs-edit", "({AB}x)", "()", "({AB}y)")
case("s2", "delete-vs-relink", "({AB})", "({B})", "({ABC})")
case("s2", "two-conflicts", "({AB}x)", "({AB}y)", "({ABC}z)")
case("s2", "novel-overlap-diff", "()", "({AB}x)", "({AC}y)")
case("s2", "novel-overlap-same", "()", "({AB}x)", "({AC}x)")
case("s2", "implied-change", "({AB}x)", "({AB}y)", "({AB}z)")
case("s2", "degenerate-anchor", "({A}x)", "({AB}x)", "({AC}x)")
case("s2", "split-fragment", "({ABCD}x)", "({AB}x,{CD}x)", "({ABCD}y)")
case("s2", "fragment-overlap", "({ABCD})", "({AD},{BC})", "({AB},{CD})")
case("s2", "zero-overlap", "({ABCD})", "({ACD},{B})", "({ABD},{C})")
case("s2", "fragment-vs-anchor", "({ABCD})", "({AB},{CD})", "({ABC},{D})")
case("s2", "gone-vs-standalone", "({AB})", "({B})", "({B},{A})")
case("s2", "sever-vs-edit", "({ABC}x)", "({BC}x,{A}x)", "({ABC}y)")
case("s2", "severed-content", "({AB}x)", "({B}y,{A}x)", "({AB}y)")
case("s2", "one-file-two-names", "({AB}x)", "({AB}x)", "({AB}y)")
# sprint-2 tracker traps
case("s2", "trap-create-vs-link", "({B}y)", "({A}y,{B}y)", "({AB}y)")
case("s2", "trap-edit-vs-repoint", "({A}m,{Q}q)", "({A}n,{Q}q)", "({AQ}q)")
case("s2", "trap-rename-src-vs-pool", "({A}m)", "({R}m)", "({AS}m)")
case("s2", "trap-rename-dest", "({A}m,{Q}q)", "({R}m,{Q}q)", "({A}m,{QR}q)")
case("s2", "trap-edit-plus-grow", "({AB}x)", "({AB}y)", "({ABC}x)")
case("s2", "trap-two-new-pools", "()", "({A}x)", "({B}y)")
case("s2", "trap-disjoint-fragments", "({ABCD}x)", "({AB}x,{C}x,{D}x)", "({CD}x,{A}x,{B}x)")
case("s2", "trap-joiner-alive", "({AB}x)", "({AB}x)", "({C}x)")
case("s2", "trap-dead-vs-edit", "({AB}x)", "({AB}y)", "()")
case("s2", "trap-shrink-survivor", "({AB}x)", "({AB}x)", "({A}x)")
case("s2", "trap-delete-vs-sever", "({AB}x)", "({B}x)", "({B}x,{A}x)")
case("s2", "trap-contested-in-grown", "({AB}x)", "({AB}x,{P}y)", "({ABP}x)")
# the previous theory's classes, open problems and vectors (th)
case("th", "name-both-rehome", "({A}a,{B}b,{C}c)", "({AB}b,{C}c)", "({AC}c,{B}b)")
case("th", "merge-vs-split", "({AB}x,{C}x)", "({ABC}x)", "({A}x,{B}x,{C}x)")
case("th", "growth-into-divided", "({AB}x)", "({A}x,{B}x)", "({ABE}x)")
case("th", "move-link-into-divided", "({PQ}x,{S}x)", "({P}x,{Q}x,{S}x)", "({PQS}x)")
case("th", "both-edited", "({A}x)", "({A}y)", "({A}z)")
case("th", "parallel-creation", "()", "({A}y)", "({A}z)")
case("th", "identical-merge-plus-edit", "({A}a,{B}b)", "({AB}a)", "({AB}c)")
case("th", "op1-edit-vs-split", "({AB}x)", "({A}x,{B}x)", "({AB}y)")
case("th", "op5-rename-rename", "({A}x)", "({X}x)", "({Y}x)")
case("th", "op6-succession", "({AB}x)", "({C}x,{B}x)", "({Q}x,{D}x)")
case("th", "v1-unilateral-split", "({AB}v)", "({A}v,{B}v)", "({AB}v)")
case("th", "v2a-two-splits", "({ABC}v)", "({A}v,{BC}v)", "({AB}v,{C}v)")
case("th", "v5-rename-delete", "({A}v)", "()", "({B}v)")
case("th", "v6-rename-vs-edit", "({A}v)", "({X}v)", "({A}w)")
case("th", "v7a-editor-backup", "({A}v)", "({X}v,{A}v)", "({A}v)")
case("th", "v7b-editor-backup-edit", "({A}v)", "({X}v,{A}v)", "({A}w)")
case("th", "v8-double-rename-pair", "({AB}v)", "({XB}v)", "({AY}v)")
case("th", "v9b-same-rename", "({A}v)", "({X}v)", "({X}v)")
case("th", "v9c-linked-double-rename", "({AB}v)", "({CB}v)", "({QD}v)")
case("th", "v9d-same-on-both", "({AB}v)", "({QD}v)", "({QD}v)")
case("th", "v10-identical-merge", "({A}a,{B}b)", "({AB}a)", "({AB}a)")
case("th", "v12-sibling-adds", "()", "({A}a)", "({B}b)")
case("th", "v14a-split-vs-rename", "({AB}v)", "({A}v,{B}v)", "({ZB}v)")
case("th", "v17-swapfile", "({A}v)", "({A}v)", "({A}w)")
case("th", "v18-both-delete", "({A}v)", "()", "()")
case("th", "v19-recycle-name", "({AW}v,{Z}z)", "({AW}v,{Z}y)", "({ZW}v)")
case("th", "v20-fidelity", "({AB}v,{C}c)", "({AB}v,{C}c)", "({XB}w,{D}d)")

# rows of the yellow, permissive and green notes, parsed from the files
def note_rows(path, group, modes=("strict",)):
    n = 0
    for line in open(path):
        if not line.startswith("    ({") and not line.startswith("    ()"):
            continue
        toks = line.split()
        if len(toks) < 4 or not all(t.startswith("(") for t in toks[:3]):
            continue
        n += 1
        case(group, "row%02d" % n, toks[0], toks[1], toks[2], modes)
note_rows(os.path.join(THEORY, "v4-yellow-content.md"), "yw")
note_rows(os.path.join(THEORY, "v4-permissive-merge.md"), "pm", ("strict", "permissive"))
note_rows(os.path.join(THEORY, "v4-green-pooling.md"), "gr")

# ---- fixture text ---------------------------------------------------------
def tree_lines(tree):
    out = []
    for names, c in sorted(tree, key=lambda p: min(p[0])):
        first = min(names)
        out.append("/%s file %s" % (first, c))
        for n in sorted(names - {first}):
            out.append("/%s link /%s" % (n, first))
    return out

def fixture_text(b, f, o, verdict, source, expect):
    lines = ["# %s" % source, "# verdict: %s" % verdict, "tree base"] + tree_lines(b)
    lines += ["tree from"] + tree_lines(f) + ["tree onto"] + tree_lines(o)
    lines += ["expect"] + expect.rstrip("\n").split("\n")
    return "\n".join(lines) + "\n"

# ---- replay of a manifest on the onto tree ---------------------------------
def replay(manifest, from_tree, onto_tree):
    """Return (pools as set of (frozenset, content), classes as set)."""
    fcontent = {n: c for names, c in from_tree for n in names}
    obj = {}; content = {}; nextid = 0
    for names, c in onto_tree:
        for n in names:
            obj[n] = nextid
        content[nextid] = c; nextid += 1
    classes = set(); intree = True; conflicts = 0
    for line in manifest.split("\n"):
        if line.startswith("#conflicts"):
            conflicts = int(line.split()[1])
        if line.startswith("conflict "):
            for cls in line.split()[2].split(","):
                classes.add(cls)
        s = line.strip()
        if not intree or not s or s.startswith("#") or s == "/" or s == "..":
            if s == ".." and line.startswith("    ..") and line == "    ..":
                intree = False
            continue
        toks = s.split()
        name, act = toks[0], (toks[1] if len(toks) > 1 else None)
        arg = toks[2].lstrip("/") if len(toks) > 2 else None
        if act == "rm":
            del obj[name]
        elif act == "cp":
            obj[name] = nextid; content[nextid] = fcontent[arg]; nextid += 1
        elif act == "dup":
            obj[name] = nextid; content[nextid] = content[obj[arg]]; nextid += 1
        elif act == "write":
            content[obj[name]] = fcontent[arg]
        elif act == "ln":
            obj[name] = obj[arg]
        elif act == "conflict":
            pass
    pools = {}
    for n, i in obj.items():
        pools.setdefault(i, set()).add(n)
    return ({(frozenset(v), content[i]) for i, v in pools.items()}, classes, conflicts)

def checker_verdict(b, f, o, strict):
    def yt(t): return tuple((frozenset(n), c) for n, c in t)
    r, k = Y.yellow(yt(b), yt(f), yt(o), strict)
    if k is None:
        return r, None
    if k == "green":
        _, _, kinds = G.rule_a([n for n, c in b], [n for n, c in f], [n for n, c in o], strict)
        names = ["healed-split", "orphaned-add", "contested-home", "unexpressed-sharing"]
        return None, {n for n, v in zip(names, kinds) if v}
    return None, {k}

def normalize_classes(cs):
    """The battery's rule: green classes if any, else changed-both before disagree."""
    green = {c for c in cs if c in ("healed-split", "orphaned-add", "contested-home", "unexpressed-sharing")}
    if green:
        return green
    if "changed-both" in cs:
        return {"changed-both"}
    return {"disagree"} if "disagree" in cs else set()

# ---- main -----------------------------------------------------------------
def main():
    repo = sys.argv[1]
    binp = os.path.join(repo, "zfs_rebase")
    outdir = os.path.join(repo, "tests", "fixtures")
    written = 0; problems = []
    for group, slug, bs, fs, os_, modes in CASES:
        b, f, o = parse_tree(bs), parse_tree(fs), parse_tree(os_)
        for mode in modes:
            strict = mode == "strict"
            fname = "h-%s-%s%s.zrt" % (group, slug, "" if strict else "-permissive")
            with tempfile.TemporaryDirectory() as td:
                tmpfx = os.path.join(td, "f.zrt")
                open(tmpfx, "w").write(fixture_text(b, f, o, "?", "", "#none"))
                # build without the expect block: write a spec, build, run
                open(tmpfx, "w").write("\n".join(["tree base"] + tree_lines(b) + ["tree from"] + tree_lines(f) + ["tree onto"] + tree_lines(o)) + "\n")
                subprocess.run([binp, "--build-fixture", tmpfx, td], check=True)
                args = [binp, "--posix"] + (["-p"] if not strict else []) + [os.path.join(td, "base"), os.path.join(td, "from"), os.path.join(td, "onto")]
                p = subprocess.run(args, capture_output=True, text=True)
                manifest = p.stdout
            exp_result, exp_classes = checker_verdict(b, f, o, strict)
            got_pools, got_classes, nconf = replay(manifest, f, o)
            if exp_classes is not None:
                ok = nconf > 0 and normalize_classes(got_classes) == normalize_classes(exp_classes)
                verdict = "conflict " + ",".join(sorted(exp_classes))
            else:
                exp_pools = {(names, c) for names, c in exp_result}
                ok = nconf == 0 and got_pools == exp_pools
                verdict = "(" + ",".join("{%s}%s" % (" ".join(sorted(n)), c) for n, c in sorted(exp_pools, key=lambda pc: min(pc[0]))) + ")"
            if not ok:
                problems.append((fname, verdict, sorted(got_classes), sorted((sorted(n), c) for n, c in got_pools), manifest))
                continue
            # rewrite the header's dataset lines to stable names
            manifest = re.sub(r"^#base .*$", "#base zrt/base@s", manifest, flags=re.M)
            manifest = re.sub(r"^#from .*$", "#from zrt/from@s", manifest, flags=re.M)
            manifest = re.sub(r"^#onto .*$", "#onto zrt/onto@s", manifest, flags=re.M)
            source = "%s %s: base %s from %s onto %s [%s]" % (group, slug, bs, fs, os_, mode)
            open(os.path.join(outdir, fname), "w").write(fixture_text(b, f, o, verdict, source, manifest))
            written += 1
    print("written:", written, "problems:", len(problems))
    for fname, verdict, cls, pools, m in problems:
        print("PROBLEM", fname, "expected", verdict, "got classes", cls, "pools", pools)
        print(m)
    return 1 if problems else 0

if __name__ == "__main__":
    sys.exit(main())
