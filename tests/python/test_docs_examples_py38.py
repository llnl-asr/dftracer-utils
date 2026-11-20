# SPDX-License-Identifier: MIT
"""Every Python example in the docs and in examples/ parses with the Python 3.8
grammar and avoids the 3.9+ runtime features that the grammar does not catch."""

import ast
import re
import textwrap
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BLOCK = re.compile(r"^(\s*)\.\. code-block:: python\s*$")
GENERIC_BUILTINS = {"list", "dict", "tuple", "set", "frozenset", "type"}
NEW_METHODS = {"removeprefix", "removesuffix", "is_relative_to", "with_stem", "bit_count"}


def _rst_blocks(path):
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        m = BLOCK.match(lines[i])
        i += 1
        if not m:
            continue
        indent = len(m.group(1))
        while i < len(lines) and lines[i].lstrip().startswith(":"):
            i += 1
        start = i
        body = []
        while i < len(lines):
            line = lines[i]
            if line.strip() and len(line) - len(line.lstrip()) <= indent:
                break
            body.append(line)
            i += 1
        yield start + 1, textwrap.dedent("\n".join(body))


def _examples():
    for path in sorted((ROOT / "docs" / "source").rglob("*.rst")):
        for line, code in _rst_blocks(path):
            yield f"{path.relative_to(ROOT)}:{line}", code
    for path in sorted((ROOT / "examples").rglob("*.py")):
        yield str(path.relative_to(ROOT)), path.read_text(encoding="utf-8")


EXAMPLES = list(_examples())


def _new_features(tree):
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Subscript)
            and isinstance(node.value, ast.Name)
            and node.value.id in GENERIC_BUILTINS
        ):
            yield node.lineno, f"{node.value.id}[...] needs 3.9"
        elif (
            isinstance(node, ast.BinOp)
            and isinstance(node.op, ast.BitOr)
            and any(
                isinstance(side, ast.Constant) and side.value is None
                for side in (node.left, node.right)
            )
        ):
            yield node.lineno, "X | None needs 3.10"
        elif isinstance(node, ast.Attribute) and node.attr in NEW_METHODS:
            yield node.lineno, f".{node.attr}() needs 3.9"
        elif isinstance(node, ast.keyword) and node.arg == "strict":
            yield node.lineno, "zip(strict=) needs 3.10"


def test_examples_found():
    assert len(EXAMPLES) > 100


@pytest.mark.parametrize(("where", "code"), EXAMPLES, ids=[w for w, _ in EXAMPLES])
def test_example_runs_on_python_38(where, code):
    tree = ast.parse(code, filename=where, feature_version=(3, 8))
    found = [f"{where} +{line}: {msg}" for line, msg in _new_features(tree)]
    assert not found, "\n".join(found)
