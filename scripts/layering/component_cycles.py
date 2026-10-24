import os
import re
import sys

INCLUDE = re.compile(r"^\s*#\s*include\s*<(dftracer/utils/[^>]+)>", re.M)
SOURCE_EXTS = {".cpp", ".cc", ".cxx", ".c"}
HEADER_EXTS = {".h", ".hpp", ".hh"}
UTILS = os.path.join("dftracer", "utils")


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.abspath(
        os.path.join(os.path.dirname(__file__), "..", "..")
    )
    files = {}
    for tree in ("include", "src"):
        base = os.path.join(root, tree, UTILS)
        for dirpath, _, names in os.walk(base):
            for name in names:
                stem, ext = os.path.splitext(name)
                if ext in SOURCE_EXTS or ext in HEADER_EXTS:
                    full = os.path.join(dirpath, name)
                    key = os.path.relpath(os.path.join(dirpath, stem), base)
                    files[os.path.relpath(full, root)] = (key, ext in SOURCE_EXTS)

    graph = {key: set() for key, _ in files.values()}
    for rel, (key, is_source) in files.items():
        with open(os.path.join(root, rel), errors="replace") as handle:
            text = handle.read()
        for target in INCLUDE.findall(text):
            for tree in ("include", "src"):
                found = files.get(os.path.join(tree, target))
                if found:
                    break
            if not found:
                continue
            if found[0] == key and is_source:
                continue
            graph[key].add(found[0])

    sys.setrecursionlimit(100000)
    index = {}
    low = {}
    stack = []
    on_stack = set()
    groups = []

    def visit(node):
        index[node] = low[node] = len(index)
        stack.append(node)
        on_stack.add(node)
        for nxt in sorted(graph[node]):
            if nxt not in index:
                visit(nxt)
                low[node] = min(low[node], low[nxt])
            elif nxt in on_stack:
                low[node] = min(low[node], index[nxt])
        if low[node] == index[node]:
            group = []
            while True:
                member = stack.pop()
                on_stack.discard(member)
                group.append(member)
                if member == node:
                    break
            groups.append(sorted(group))

    for node in sorted(graph):
        if node not in index:
            visit(node)

    cycles = [g for g in groups if len(g) > 1 or g[0] in graph[g[0]]]
    cycles.sort(key=lambda g: (-len(g), g))
    print("components %d" % len(graph))
    print("cycles %d" % len(cycles))
    print("components_in_cycles %d" % sum(len(g) for g in cycles))
    for number, group in enumerate(cycles, 1):
        print("cycle %02d size %d" % (number, len(group)))
        for member in group:
            print("cycle %02d member %s" % (number, member))


main()
