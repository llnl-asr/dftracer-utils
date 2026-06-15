#!/usr/bin/env python3
"""Filter Valgrind XML report(s) to records attributable to our own code.

Usage: ours_only.py [--match dftracer] [--topk 4] <xml-file>...
"""

import argparse
import glob
import sys
import xml.etree.ElementTree as ET
from collections import Counter, OrderedDict


def frame_str(fr):
    fn = fr.findtext("fn") or "?"
    file = fr.findtext("file")
    line = fr.findtext("line")
    obj = (fr.findtext("obj") or "").split("/")[-1]
    if file:
        return f"{fn} ({file}:{line})"
    return f"{fn} [{obj}]"


def _scan_error(err, match, topk):
    """Return (kind, frames, idx, bytes_txt) if attributable to match, else None."""
    stack = err.find("stack")
    if stack is None:
        return None
    frames = stack.findall("frame")
    idx = None
    for i, fr in enumerate(frames[:topk]):
        fn = fr.findtext("fn") or ""
        file = fr.findtext("file") or ""
        dir_ = fr.findtext("dir") or ""
        if match in fn or match in file or match in dir_:
            idx = i
            break
    if idx is None:
        return None
    kind = err.findtext("kind") or "?"
    bytes_txt = ""
    xwhat = err.find("xwhat")
    if xwhat is not None and xwhat.findtext("leakedbytes"):
        bytes_txt = f" {xwhat.findtext('leakedbytes')}B"
    return (kind, frames, idx, bytes_txt)


def collect(xml_path, match, topk):
    """Return (ours, total) for one XML."""
    total = 0
    ours = []
    try:
        for _ev, elem in ET.iterparse(xml_path, events=("end",)):
            if elem.tag != "error":
                continue
            total += 1
            hit = _scan_error(elem, match, topk)
            if hit is not None:
                ours.append(hit)
            else:
                elem.clear()  # free memory; keep matched errors' subtrees
    except ET.ParseError:
        # Truncated report (process killed mid-write); keep what we parsed.
        pass
    except OSError as e:
        print(f"ours_only: cannot read {xml_path}: {e}", file=sys.stderr)
        return None, 0
    return ours, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--match", default="dftracer")
    ap.add_argument("--topk", type=int, default=8)
    ap.add_argument("xml", nargs="+", help="Valgrind XML file(s); globs allowed")
    args = ap.parse_args()

    files = []
    for pat in args.xml:
        files.extend(sorted(glob.glob(pat)) or [pat])

    all_ours = []
    grand_total = 0
    per_file = []
    for f in files:
        ours, total = collect(f, args.match, args.topk)
        if ours is None:
            continue
        grand_total += total
        if ours:
            per_file.append((f.split("/")[-1], len(ours)))
        all_ours.extend(ours)

    print(
        f"ours_only: {len(all_ours)} records attributable to '{args.match}' "
        f"across {len(files)} report(s) ({grand_total} total errors)"
    )
    for fname, n in per_file:
        print(f"  {n:5d}  {fname}")
    if not all_ours:
        return 0

    grouped = Counter((k, frame_str(frames[i])) for (k, frames, i, _b) in all_ours)
    print("--- grouped by kind + our top frame ---")
    for (kind, top), n in grouped.most_common(50):
        print(f"  [{kind}] x{n}  {top}")

    print("--- example stacks ---")
    seen = OrderedDict()
    for kind, frames, i, b in all_ours:
        key = (kind, frame_str(frames[i]))
        if key in seen:
            continue
        seen[key] = True
        print(f"[{kind}]{b}")
        for fr in frames[:12]:
            print(f"    {frame_str(fr)}")
        print()
        if len(seen) >= 15:
            break
    return 1


sys.exit(main())
