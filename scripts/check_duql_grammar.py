#!/usr/bin/env python3
"""Build scripts/duql.lark with Lark's LALR(1) generator and check samples.

Run: uv run --no-project --with lark python scripts/check_duql_grammar.py
Exits non-zero on any grammar conflict, any valid sample that does not
parse, or any invalid sample that parses.
"""

import argparse
import logging
import re
import sys
from collections import defaultdict
from pathlib import Path

from lark import Lark, logger
from lark.exceptions import GrammarError, LarkError

GRAMMAR = Path(__file__).with_name("duql.lark")

DEFAULT_SAMPLES = Path(__file__).resolve().parents[1] / "tests" / "duql" / "grammar_samples.duql"


def _read_samples(path: Path) -> tuple[list[str], list[str]]:
    """Split on `---` lines; a `### invalid` line starts the must-fail part."""
    valid: list[str] = []
    invalid: list[str] = []
    target = valid
    chunk: list[str] = []
    for line in [*path.read_text().splitlines(), "---"]:
        if line in ("---", "### invalid"):
            if chunk:
                target.append("\n".join(chunk))
            chunk = []
            if line == "### invalid":
                target = invalid
        else:
            chunk.append(line)
    return valid, invalid


class _Conflicts(logging.Handler):
    def __init__(self) -> None:
        super().__init__(logging.WARNING)
        self.lines: list[str] = []

    def emit(self, record: logging.LogRecord) -> None:
        self.lines.append(record.getMessage())


def _group_reduce_reduce(msg: str) -> str:
    found = re.findall(
        r"Reduce/Reduce collision in Terminal\('([^']+)'\) between the following "
        r"rules: \n((?:\t- .*\n)+)",
        msg,
    )
    if not found:
        return msg
    by_rules: dict[str, list[str]] = defaultdict(list)
    for terminal, rules in found:
        by_rules[rules].append(terminal)
    return "\n".join(
        f"Reduce/Reduce between\n{rules}  on lookahead {', '.join(sorted(ts))}"
        for rules, ts in by_rules.items()
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--samples", type=Path, default=DEFAULT_SAMPLES)
    args = ap.parse_args()
    conflicts = _Conflicts()
    logger.handlers = [conflicts]
    logger.setLevel(logging.WARNING)
    errors: list[str] = []
    try:
        parser = Lark(GRAMMAR.read_text(), parser="lalr", lexer="contextual", debug=True)
    except GrammarError as e:
        errors.append(_group_reduce_reduce(str(e)))
    pairs = zip(conflicts.lines[::2], conflicts.lines[1::2])
    shift_reduce = sorted(
        {f"{head.split(':')[0]}: shift vs reduce {rule.strip(' *')}" for head, rule in pairs}
    )
    errors[:0] = shift_reduce
    if errors:
        print("grammar conflicts:\n" + "\n".join(errors))
        return 1
    valid, invalid = _read_samples(args.samples)
    failed = 0
    for text in valid:
        try:
            parser.parse(text)
        except LarkError as e:
            failed += 1
            print(f"FAIL (should parse) {text!r}\n  {e}")
    for text in invalid:
        try:
            parser.parse(text)
        except LarkError:
            continue
        failed += 1
        print(f"FAIL (should not parse) {text!r}")
    total = len(valid) + len(invalid)
    print(f"{total - failed}/{total} samples ok ({len(valid)} valid, {len(invalid)} invalid)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
