#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Writes the C program that prints the plugin ABI manifest.

The plugin ABI is the shape of the headers a plugin compiles against, not the
text of them. This reads those headers, takes every struct and union
definition (members in order, a nested union by its name path), every
enumerator, every integer DFTU_* #define and every function pointer spelling,
and writes one C file. Compiled against include/ and the generated include
dir, that program prints one fact per line, sorted by kind and name:

    struct <name> size <n> align <a>
    field <struct>.<path> offset <o> size <s>
    enum <NAME> <value>
    const <NAME> <value>      (an integer, or a string define with its quotes)
    fnptr <name> <type text>
    typedef <name> <underlying type text>

Sizes, alignments, offsets and constant values come from sizeof, _Alignof,
offsetof and the compiler, never from this parser: the parser supplies only
the shapes to ask about, the function pointer spellings and the typedef
underlying types, which no runtime operator gives. A function pointer spelling
keeps the return type, the pointers and every const, and drops the declarator
and parameter names, so renaming a parameter or a callback changes no line.
cmake/plugin_abi_manifest.txt is the recorded output and ctest
`plugin_abi_manifest` compares against it.

    scripts/plugin_abi_manifest.py --out build/build-tests/plugin_abi_manifest.c

Values print as signed when the constant's own C type is signed, so a change
shows as a changed value, not as a change of spelling.
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The plugin ABI header set: the one cmake/modules/Versions.cmake hashed before
# the manifest replaced that hash, with dataframe/c_api*.h as a glob so a new
# c_api header is covered without editing this list.
ABI_HEADERS = [
    "dftracer/utils/common/c_api.h",
    "dftracer/utils/common/export.h",
    "dftracer/utils/dataframe/agg_op_codes.h",
    "dftracer/utils/duql/c_api.h",
    "dftracer/utils/plugins/abi.h",
]
ABI_HEADER_GLOBS = [
    ("dftracer/utils/dataframe", "c_api*.h"),
    ("dftracer/utils/plugins/abi", "*.h"),
]

ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
NUM_RE = re.compile(r"(?:0[xX][0-9A-Fa-f]+|0[0-7]*|[0-9]+)(?:[uUlL]*)")
PUNCT2 = ("<<", ">>", "&&", "||", "->", "<=", ">=", "==", "!=", "##")

# Cast targets that make a #define an integer constant.
INT_TYPES = frozenset(
    """
    _Bool bool char signed unsigned short int long
    size_t ssize_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t wchar_t
    int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t
    int_least8_t int_least16_t int_least32_t int_least64_t
    uint_least8_t uint_least16_t uint_least32_t uint_least64_t
    int_fast8_t int_fast16_t int_fast32_t int_fast64_t
    uint_fast8_t uint_fast16_t uint_fast32_t uint_fast64_t
    """.split()
)

# stdint.h and limits.h names an integer constant may be built from.
INT_MACROS = frozenset(
    """
    INT8_MIN INT8_MAX INT16_MIN INT16_MAX INT32_MIN INT32_MAX
    INT64_MIN INT64_MAX UINT8_MAX UINT16_MAX UINT32_MAX UINT64_MAX
    INT_LEAST8_MAX INT_LEAST16_MAX INT_LEAST32_MAX INT_LEAST64_MAX
    UINT_LEAST8_MAX UINT_LEAST16_MAX UINT_LEAST32_MAX UINT_LEAST64_MAX
    INT_FAST8_MAX INT_FAST16_MAX INT_FAST32_MAX INT_FAST64_MAX
    UINT_FAST8_MAX UINT_FAST16_MAX UINT_FAST32_MAX UINT_FAST64_MAX
    INTMAX_MIN INTMAX_MAX UINTMAX_MAX INTPTR_MIN INTPTR_MAX UINTPTR_MAX
    PTRDIFF_MIN PTRDIFF_MAX SIZE_MAX
    CHAR_BIT SCHAR_MIN SCHAR_MAX UCHAR_MAX SHRT_MIN SHRT_MAX USHRT_MAX
    INT_MIN INT_MAX UINT_MAX LONG_MIN LONG_MAX ULONG_MAX
    LLONG_MIN LLONG_MAX ULLONG_MAX
    """.split()
)

INT_OPS = frozenset(("(", ")", "+", "-", "*", "/", "%", "&", "|", "^", "~", "<<", ">>"))

SORT_ORDER = {"const": 0, "enum": 1, "field": 2, "fnptr": 3, "struct": 4, "typedef": 5}

# A parameter declarator loses its name, never one of these.
C_KEYWORDS = frozenset(
    "void char short int long float double signed unsigned _Bool const volatile "
    "restrict struct union enum".split()
)


class ManifestError(Exception):
    """A header this script cannot read as plugin ABI."""


def abi_headers(root):
    include = os.path.join(root, "include")
    names = set(ABI_HEADERS)
    for folder, pattern in ABI_HEADER_GLOBS:
        directory = os.path.join(include, *folder.split("/"))
        if not os.path.isdir(directory):
            raise ManifestError("no such header folder: %s" % directory)
        for name in os.listdir(directory):
            if name.endswith(".h") and _glob_match(name, pattern):
                names.add(folder + "/" + name)
    return sorted(names)


def _glob_match(name, pattern):
    return re.match("^" + re.escape(pattern).replace("\\*", "[^/]*") + "$", name) is not None


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    out = []
    i = 0
    end = len(text)
    while i < end:
        if text.startswith("/*", i):
            close = text.find("*/", i + 2)
            i = end if close < 0 else close + 2
            out.append(" ")
        elif text.startswith("//", i):
            newline = text.find("\n", i)
            i = end if newline < 0 else newline
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < end and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def tokenize(text):
    toks = []
    i = 0
    end = len(text)
    while i < end:
        ch = text[i]
        if ch in " \t\r\n\f\v":
            i += 1
        elif ch in "\"'":
            j = i + 1
            while j < end and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            toks.append(("str", text[i : j + 1], i))
            i = j + 1
        elif ch.isalpha() or ch == "_":
            match = ID_RE.match(text, i)
            toks.append(("id", match.group(0), i))
            i = match.end()
        elif ch.isdigit():
            match = NUM_RE.match(text, i)
            toks.append(("num", match.group(0), i))
            i = match.end()
        elif text[i : i + 2] in PUNCT2:
            toks.append(("punct", text[i : i + 2], i))
            i += 2
        else:
            toks.append(("punct", ch, i))
            i += 1
    return toks


def span_of(tok):
    """The (start, end) offsets of a token in the text it came from."""
    return (tok[2], tok[2] + len(tok[1]))


def tidy(raw):
    """Whitespace-normalized C text, with no space inside a parameter list."""
    text = " ".join(raw.split())
    text = re.sub(r"\(\s+", "(", text)
    text = re.sub(r"\s+\)", ")", text)
    text = re.sub(r"\s+,", ",", text)
    text = re.sub(r",\s*", ", ", text)
    text = re.sub(r"\s+\]", "]", text)
    text = re.sub(r"\[\s+", "[", text)
    return text.strip()


def stitch(text, head, tail, cuts):
    """text[head:tail] with the cuts removed, tidied."""
    pieces = []
    cursor = head
    for start, end in sorted(cuts):
        if start < cursor or end > tail:
            continue
        pieces.append(text[cursor:start])
        cursor = end
    pieces.append(text[cursor:tail])
    return tidy("".join(pieces))


def declaration_text(text, toks, cuts=()):
    """The spelling of a declaration, whitespace-normalized, cuts removed."""
    if not toks:
        return ""
    return stitch(text, toks[0][2], toks[-1][2] + len(toks[-1][1]), cuts)


def split_preprocessor(text):
    """The #define bodies of a header, and its text with the directives gone."""
    text = text.replace("\\\r\n", "").replace("\\\n", "")
    defines = {}
    code = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped.startswith("#"):
            code.append(line)
            continue
        body = stripped[1:].lstrip()
        if not body.startswith("define"):
            continue
        rest = body[len("define") :]
        if not rest[:1].isspace():
            continue
        rest = rest.lstrip()
        match = ID_RE.match(rest)
        if match is None:
            continue
        tail = rest[match.end() :]
        if tail.startswith("("):
            continue
        defines.setdefault(match.group(0), tail.strip())
    return defines, "\n".join(code)


def match_pair(toks, open_index, opener, closer):
    depth = 0
    for index in range(open_index, len(toks)):
        kind, text = toks[index][:2]
        if kind != "punct":
            continue
        if text == opener:
            depth += 1
        elif text == closer:
            depth -= 1
            if depth == 0:
                return index
    raise ManifestError("unbalanced %s at offset %d" % (opener, toks[open_index][2]))


def match_brace(toks, open_index):
    return match_pair(toks, open_index, "{", "}")


def split_top(toks, separator):
    parts = []
    current = []
    depth = 0
    for tok in toks:
        kind, text = tok[:2]
        if kind == "punct":
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
            elif text == separator and depth == 0:
                parts.append(current)
                current = []
                continue
        current.append(tok)
    if current:
        parts.append(current)
    return parts


def statement_end(toks, start):
    depth = 0
    for index in range(start, len(toks)):
        kind, text = toks[index][:2]
        if kind != "punct":
            continue
        if text in ("(", "[", "{"):
            depth += 1
        elif text in (")", "]", "}"):
            depth -= 1
        elif text == ";" and depth == 0:
            return index
    return len(toks)


def declarator_names(toks):
    """The names a definition declares after its closing brace, up to the ;."""
    names = []
    depth = 0
    candidate = None
    for kind, text, _ in toks:
        if kind == "punct":
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
            elif depth == 0 and text in (",", ";"):
                if candidate is not None:
                    names.append(candidate)
                    candidate = None
                if text == ";":
                    return names
        elif kind == "id" and depth == 0:
            candidate = text
        elif kind == "id" and depth > 0:
            candidate = None
    if candidate is not None:
        names.append(candidate)
    return names


def last_top_id(toks):
    """The index of the last identifier at depth 0, or None."""
    depth = 0
    last = None
    for index, tok in enumerate(toks):
        kind, text = tok[:2]
        if kind == "punct":
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
        elif kind == "id" and depth == 0:
            last = index
    return last


class Unit:
    """One struct or union definition."""

    def __init__(self, kind, tag):
        self.kind = kind
        self.tag = tag
        self.names = []
        self.fields = []

    @property
    def name(self):
        return self.names[0] if self.names else self.tag

    @property
    def c_type(self):
        if self.names:
            return self.names[0]
        if self.tag:
            return "%s %s" % (self.kind, self.tag)
        return None


def enum_names(toks):
    names = []
    depth = 0
    expecting = True
    for kind, text, _ in toks:
        if kind == "punct":
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
            elif text == "," and depth == 0:
                expecting = True
        elif kind == "id" and depth == 0 and expecting:
            names.append(text)
            expecting = False
    return names


def fnptr_group(toks, index):
    """(the (* of a declarator, its name index or None) at toks[index], or None."""
    if index + 2 >= len(toks):
        return None
    if toks[index][:2] != ("punct", "(") or toks[index + 1][:2] != ("punct", "*"):
        return None
    if toks[index + 2][:2] == ("punct", ")"):
        return (index, None)
    if (
        index + 3 < len(toks)
        and toks[index + 2][0] == "id"
        and toks[index + 3][:2] == ("punct", ")")
    ):
        return (index, index + 2)
    return None


def fnptr_name(toks):
    """The name of a (*name) function pointer in toks, or None."""
    for index in range(len(toks)):
        group = fnptr_group(toks, index)
        if group is not None and group[1] is not None:
            return toks[group[1]][1]
    return None


def param_name_index(toks):
    """The index of a parameter's name, or None when the declarator has none."""
    depth = 0
    count = 0
    last = None
    for index, tok in enumerate(toks):
        kind, text = tok[:2]
        if kind == "punct":
            if text in ("(", "[", "{"):
                depth += 1
            elif text in (")", "]", "}"):
                depth -= 1
            continue
        if depth != 0 or kind != "id":
            continue
        count += 1
        last = index
    if last is None or count < 2:
        return None
    if toks[last][1] in C_KEYWORDS:
        return None
    if last > 0 and toks[last - 1][1] in ("struct", "union", "enum"):
        return None
    return last


def param_cuts(toks):
    """The name offsets inside one parameter declarator."""
    if not toks:
        return []
    if len(toks) == 1 and toks[0][1] in ("void", "..."):
        return []
    if any(fnptr_group(toks, index) is not None for index in range(len(toks))):
        return name_cuts(toks)
    index = param_name_index(toks)
    return [] if index is None else [span_of(toks[index])]


def name_cuts(toks):
    """The offsets of every declarator name and parameter name in toks."""
    cuts = []
    index = 0
    while index < len(toks):
        group = fnptr_group(toks, index)
        if group is None:
            index += 1
            continue
        open_index, name_index = group
        if name_index is None:
            index = open_index + 3
        else:
            cuts.append(span_of(toks[name_index]))
            index = open_index + 4
        if index < len(toks) and toks[index][:2] == ("punct", "("):
            close = match_pair(toks, index, "(", ")")
            for param in split_top(toks[index + 1 : close], ","):
                cuts.extend(param_cuts(param))
            index = close + 1
    return cuts


def fnptr_text(text, toks):
    """A function pointer's spelling without declarator or parameter names."""
    return declaration_text(text, toks, name_cuts(toks))


def parse_members(unit, toks, text, prefix):
    for chunk in split_top(toks, ";"):
        if not chunk:
            continue
        if chunk[0][0] == "id" and chunk[0][1] in ("struct", "union"):
            index = 1
            if index < len(chunk) and chunk[index][0] == "id":
                index += 1
            if index < len(chunk) and chunk[index][:2] == ("punct", "{"):
                close = match_brace(chunk, index)
                names = declarator_names(chunk[close + 1 :])
                sub = prefix + (names[0] + "." if names else "")
                parse_members(unit, chunk[index + 1 : close], text, sub)
                continue
        add_declaration(unit, chunk, text, prefix)


def add_declaration(unit, chunk, text, prefix):
    for position, part in enumerate(split_top(chunk, ",")):
        if not part:
            continue
        name = fnptr_name(part)
        is_fnptr = name is not None
        at = 0 if is_fnptr else None
        depth = 0
        for index, tok in enumerate(part):
            kind, token = tok[:2]
            if kind == "punct":
                if token in ("(", "[", "{"):
                    depth += 1
                elif token in (")", "]", "}"):
                    depth -= 1
            elif kind == "id" and depth == 0 and not is_fnptr:
                name = token
                at = index
        if name is None or at is None:
            continue
        if not is_fnptr:
            unit.fields.append((prefix + name, None))
        elif position == 0:
            unit.fields.append((prefix + name, fnptr_text(text, part)))
        else:
            raise ManifestError("%s: function pointer past the first declarator" % unit.name)


def parse_aggregate(toks, start, text):
    kind = toks[start][1]
    index = start + 1
    tag = None
    if index < len(toks) and toks[index][0] == "id":
        tag = toks[index][1]
        index += 1
    if index >= len(toks) or toks[index][:2] != ("punct", "{"):
        return None, start + 1
    close = match_brace(toks, index)
    body = toks[index + 1 : close]
    if kind == "enum":
        return enum_names(body), close + 1
    unit = Unit(kind, tag)
    parse_members(unit, body, text, "")
    return unit, close + 1


def parse_typedef(toks, text, structs, enums, fnptrs, typedefs, int_types):
    if not toks:
        return
    if toks[0][0] == "id" and toks[0][1] in ("struct", "union", "enum"):
        unit, index = parse_aggregate(toks, 0, text)
        if isinstance(unit, list):
            enums.extend(unit)
            return
        if unit is not None:
            unit.names = declarator_names(toks[index:])
            structs.append(unit)
            return
    name = fnptr_name(toks)
    if name is not None:
        fnptrs.append((name, fnptr_text(text, toks)))
        return
    index = last_top_id(toks)
    if index is None:
        return
    name = toks[index][1]
    typedefs.append((name, declaration_text(text, toks, [span_of(toks[index])])))
    if index == len(toks) - 1 and index > 0 and toks[index - 1][1] in INT_TYPES:
        int_types.add(name)


def parse_unit(toks, text, structs, enums, fnptrs, typedefs, int_types):
    index = 0
    while index < len(toks):
        kind, name = toks[index][:2]
        if kind != "id":
            index += 1
        elif name in ("struct", "union", "enum"):
            unit, next_index = parse_aggregate(toks, index, text)
            if unit is None:
                index = next_index
            elif isinstance(unit, list):
                enums.extend(unit)
                index = next_index
            else:
                end = statement_end(toks, next_index)
                unit.names = declarator_names(toks[next_index:end])
                structs.append(unit)
                index = end + 1
        elif name == "typedef":
            end = statement_end(toks, index)
            parse_typedef(toks[index + 1 : end], text, structs, enums, fnptrs, typedefs, int_types)
            index = end + 1
        else:
            index += 1


def integer_constants(defines, known_names, known_types):
    found = {}
    changed = True
    while changed:
        changed = False
        for name, body in defines.items():
            if not name.startswith("DFTU_") or name in found:
                continue
            if is_integer_body(body, known_names | set(found), known_types):
                found[name] = body
                changed = True
    return found


def is_integer_body(body, known_names, known_types):
    toks = tokenize(body)
    if not toks:
        return False
    for kind, text, _ in toks:
        if kind == "num":
            continue
        if kind == "id":
            if text in known_names or text in known_types:
                continue
            return False
        if text not in INT_OPS:
            return False
    return True


def string_constants(defines):
    """The DFTU_* #defines that are one string literal, by name."""
    found = {}
    for name, body in defines.items():
        if not name.startswith("DFTU_"):
            continue
        toks = tokenize(body)
        if toks and all(tok[0] == "str" for tok in toks):
            found[name] = declaration_text(body, toks)
    return found


def parse_headers(root, headers):
    include = os.path.join(root, "include")
    defines = {}
    structs = []
    enums = []
    fnptrs = []
    typedefs = []
    int_types = set()
    for header in headers:
        path = os.path.join(include, *header.split("/"))
        if not os.path.isfile(path):
            raise ManifestError("no such header: %s" % path)
        header_defines, code = split_preprocessor(strip_comments(read_text(path)))
        for name, body in header_defines.items():
            defines.setdefault(name, body)
        parse_unit(tokenize(code), code, structs, enums, fnptrs, typedefs, int_types)
    return structs, enums, fnptrs, typedefs, int_types, defines


def c_string(text):
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"' + escaped + '"'


def struct_line(name, c_type):
    fmt = '    printf("struct %s size %%zu align %%zu\\n", sizeof(%s), '
    fmt += "(size_t)_Alignof(%s));"
    return fmt % (name, c_type, c_type)


def field_line(name, path, c_type):
    return (
        '    printf("field %s.%s offset %%zu size %%zu\\n", offsetof(%s, %s), '
        "sizeof(((%s *)0)->%s));" % (name, path, c_type, path, c_type, path)
    )


def enum_line(name):
    return '    printf("enum %s %%lld\\n", (long long)%s);' % (name, name)


def fnptr_line(name, type_text):
    return "    fputs(%s, stdout);" % c_string("fnptr %s %s\n" % (name, type_text))


def const_line(name):
    return (
        "    emit_const(%s,\n"
        "               _Generic((%s), signed char: 1, short: 1, int: 1, long: 1,\n"
        "                        long long: 1, default: 0),\n"
        "               (long long)(%s), (unsigned long long)(%s));"
        % (c_string(name), name, name, name)
    )


def manifest_source(headers, structs, enums, fnptrs, typedefs, consts, strings):
    entries = []
    seen = set()
    for unit in structs:
        name = unit.name
        if name is None:
            raise ManifestError("a %s definition has no tag and no typedef" % unit.kind)
        if name in seen:
            raise ManifestError("%s is defined twice" % name)
        seen.add(name)
        entries.append(("struct", name, struct_line(name, unit.c_type)))
        for path, type_text in unit.fields:
            key = name + "." + path
            if key in seen:
                raise ManifestError("field %s appears twice" % key)
            seen.add(key)
            entries.append(("field", key, field_line(name, path, unit.c_type)))
            if type_text is not None:
                entries.append(("fnptr", key, fnptr_line(key, type_text)))
    for name in enums:
        entries.append(("enum", name, enum_line(name)))
    for name, type_text in fnptrs:
        entries.append(("fnptr", name, fnptr_line(name, type_text)))
    underlying = {}
    for name, text in typedefs:
        if underlying.get(name, text) != text:
            raise ManifestError("typedef %s has two underlying types" % name)
        underlying[name] = text
    for name in sorted(underlying):
        line = "typedef %s %s\n" % (name, underlying[name])
        entries.append(("typedef", name, "    fputs(%s, stdout);" % c_string(line)))
    for name in consts:
        entries.append(("const", name, const_line(name)))
    for name in strings:
        line = "const %s %s\n" % (name, strings[name])
        entries.append(("const", name, "    fputs(%s, stdout);" % c_string(line)))
    entries.sort(key=lambda entry: (SORT_ORDER[entry[0]], entry[1]))
    lines = [
        "/* Generated from the plugin ABI headers by scripts/plugin_abi_manifest.py.",
        "   Do not edit; run `cmake --build <build dir> --target",
        "   plugin_abi_manifest_update` to record a shape change. */",
        "",
        "#include <stddef.h>",
        "#include <stdio.h>",
        "",
    ]
    lines.extend("#include <%s>" % header for header in headers)
    lines.extend(
        [
            "",
            "static void emit_const(const char *name, int is_signed, long long s,",
            "                       unsigned long long u) {",
            "    if (is_signed) {",
            '        printf("const %s %lld\\n", name, s);',
            "    } else {",
            '        printf("const %s %llu\\n", name, u);',
            "    }",
            "}",
            "",
            "int main(void) {",
        ]
    )
    lines.extend(entry[2] for entry in entries)
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def build(root, headers=None):
    """The manifest source text for the headers under root."""
    if headers is None:
        headers = abi_headers(root)
    headers = sorted(headers)
    structs, enums, fnptrs, typedefs, int_types, defines = parse_headers(root, headers)
    known = set(enums) | INT_MACROS
    consts = integer_constants(defines, known, int_types | INT_TYPES)
    strings = string_constants(defines)
    return manifest_source(headers, structs, enums, fnptrs, typedefs, consts, strings)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=ROOT, help="repository root (default: %(default)s)")
    parser.add_argument("--out", required=True, help="path of the C file to write")
    parser.add_argument(
        "--header",
        action="append",
        help="a header under <root>/include to read, repeatable; default: the ABI set",
    )
    args = parser.parse_args(argv)
    try:
        source = build(args.root, args.header)
    except ManifestError as error:
        sys.stderr.write("plugin_abi_manifest: %s\n" % error)
        return 1
    out_dir = os.path.dirname(os.path.abspath(args.out))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    previous = None
    if os.path.isfile(args.out):
        previous = read_text(args.out)
    if previous != source:
        with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(source)
    return 0


if __name__ == "__main__":
    sys.exit(main())
