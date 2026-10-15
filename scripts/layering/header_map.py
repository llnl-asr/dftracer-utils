import os
import re
import sys

INCLUDE = re.compile(r"^\s*#\s*include\s*<(dftracer/utils/[^>]+)>", re.M)
SOURCE_EXTS = {".cpp", ".cc", ".cxx", ".c"}
HEADER_EXTS = {".h", ".hpp", ".hh"}
UTILS = os.path.join("dftracer", "utils")
CLASSES = [
    "public",
    "public-transitive",
    "to-src",
    "private-but-other-tests",
    "dead",
    "src-crossmodule",
    "keep-abi",
]
ABI_FILES = {"plugins/abi.h", "plugins/arrow_abi.h", "dataframe/abi.h"}


def area_of(rel):
    parts = rel.split(os.sep)
    if parts[0] == "tests":
        return parts[1] if len(parts) > 2 else "tests"
    inner = parts[3:]
    return inner[0] if len(inner) > 1 else "."


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..")
    )
    files = []
    for tree in (os.path.join("include", UTILS), os.path.join("src", UTILS), "tests"):
        for dirpath, _, names in os.walk(os.path.join(root, tree)):
            for name in names:
                if os.path.splitext(name)[1] in SOURCE_EXTS | HEADER_EXTS:
                    files.append(os.path.relpath(os.path.join(dirpath, name), root))
    known = set(files)

    includers = {}
    for rel in files:
        with open(os.path.join(root, rel), errors="replace") as handle:
            text = handle.read()
        for target in INCLUDE.findall(text):
            for tree in ("include", "src"):
                path = os.path.join(tree, target)
                if path in known:
                    includers.setdefault(path, set()).add(rel)
                    break

    def is_header(rel):
        return os.path.splitext(rel)[1] in HEADER_EXTS

    headers = sorted(
        r for r in files if r.split(os.sep)[0] in ("include", "src") and is_header(r)
    )
    info = {}
    for rel in headers:
        own = area_of(rel)
        users = set()
        tests = set()
        for user in includers.get(rel, ()):
            if user.split(os.sep)[0] == "tests":
                tests.add(area_of(user))
            else:
                users.add(area_of(user))
        info[rel] = (own, users - {own}, tests)

    klass = {}
    for rel in headers:
        if not rel.startswith("include" + os.sep):
            if info[rel][1]:
                klass[rel] = "src-crossmodule"
            continue
        inner = os.path.relpath(rel, os.path.join("include", UTILS)).replace(os.sep, "/")
        if inner.startswith("plugins/abi/") or inner in ABI_FILES:
            klass[rel] = "keep-abi"
        elif info[rel][1]:
            klass[rel] = "public"

    changed = True
    while changed:
        changed = False
        for rel in headers:
            if rel in klass or not rel.startswith("include" + os.sep):
                continue
            for user in includers.get(rel, ()):
                if (
                    user.startswith("include" + os.sep)
                    and is_header(user)
                    and klass.get(user) in ("public", "keep-abi", "public-transitive")
                ):
                    klass[rel] = "public-transitive"
                    changed = True
                    break

    for rel in headers:
        if rel in klass or not rel.startswith("include" + os.sep):
            continue
        own, _, tests = info[rel]
        if not includers.get(rel):
            klass[rel] = "dead"
        elif tests - {own}:
            klass[rel] = "private-but-other-tests"
        else:
            klass[rel] = "to-src"

    for name in CLASSES:
        print("class %s %d" % (name, sum(1 for v in klass.values() if v == name)))
    order = {name: i for i, name in enumerate(CLASSES)}
    for rel in sorted(klass, key=lambda r: (order[klass[r]], r)):
        _, users, tests = info[rel]
        print(
            "header %s %s users=%s tests=%s"
            % (klass[rel], rel.replace(os.sep, "/"), ",".join(sorted(users)), ",".join(sorted(tests)))
        )


main()
