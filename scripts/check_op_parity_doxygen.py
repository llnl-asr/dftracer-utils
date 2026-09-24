#!/usr/bin/env python3
"""Checks Series/DataFrame/LazyFrame public methods against the op registry.

Reads C++ method names from the Doxygen XML the docs build emits
(docs/Doxyfile: GENERATE_XML = YES), never a header regex - Doxygen already
tells a declaration from a comment or a disabled #if branch. Not wired into
the default build or ctest: this needs docs/doxygen/xml to already exist,
which `cmake --build --preset tests` does not generate. Run it after
`doxygen docs/Doxyfile` (or from a docs CI job):

    scripts/check_op_parity_doxygen.py docs/doxygen/xml \\
        build/build-tests/lib/libdftracer_utils_dataframe.dylib

Reads the registered ops straight out of that shared library with ctypes
(dftu_op_count/dftu_op_at are a plain C ABI), so no helper binary is needed.

Exits non-zero and lists the drifted names when a public method has no op of
the same leaf name in the registry, no entry in FOLDS naming the op that backs
it under another name, and no entry in the ALLOWLIST of non-ops below.

The Python half needs no Doxygen: `--python` (or check_python(), which
tests/python/test_op_parity.py runs under pytest) introspects the
dftracer.utils Series / DataFrame / LazyFrame wrappers and their native
handles against the registry the extension itself reports (op_list), in both
directions: a Python method with no op behind it (Python-only logic), and a
registered op no Python method reaches. PYTHON_ONLY_DEBT is the measured
list of Python methods whose logic lives outside the engine; it is expected
to shrink, never grow.
"""

from __future__ import annotations

import argparse
import ctypes
import re
import sys
from pathlib import Path

CLASSES = {
    "Series": "classdftracer_1_1utils_1_1dataframe_1_1_series.xml",
    "DataFrame": "structdftracer_1_1utils_1_1dataframe_1_1_data_frame.xml",
    "LazyFrame": "classdftracer_1_1utils_1_1dataframe_1_1_lazy_frame.xml",
}

# Which registry bucket (see dftu_op_kind in abi.h) backs each class.
REGISTRY_BUCKET = {"Series": "series", "DataFrame": "frame", "LazyFrame": "lazy"}

MEMBER_RE = re.compile(r'<memberdef kind="function"[^>]*>.*?</memberdef>', re.S)
PROT_RE = re.compile(r'prot="([a-z]+)"')
NAME_RE = re.compile(r"<name>([^<]*)</name>")

# Methods a registered op backs under a different leaf name: an op code folds
# a family of methods onto one op (compare, logical, reduce), or the registry
# uses the canonical name for a method spelled as its alias. The value is the
# registry leaf, looked up across every bucket - Series::value_counts returns a
# frame, so its op is keyed dftu.frame.value_counts.
FOLDS = {
    "Series": {
        "eq": "compare",
        "ne": "compare",
        "lt": "compare",
        "le": "compare",
        "gt": "compare",
        "ge": "compare",
        "logical_and": "logical",
        "logical_or": "logical",
        "hex64_parse": "parse64",
        "hex64_format": "format64",
        "sum": "reduce",
        "mean": "reduce",
        "median": "reduce",
        "max": "reduce",
        "min": "reduce",
        "value_counts": "value_counts",
    },
    "DataFrame": {"melt": "unpivot", "hash_partition": "partition_id"},
    "LazyFrame": {},
}

# Methods that are not ops and never will be: accessors into a handle, the
# Arrow/IPC boundary, and the lazy terminals that end a plan instead of
# extending it.
ALLOWLIST = {
    "Series": {
        "type",
        "encoding",
        "length",
        "null_count",
        "is_null",
        "data",
        "values",
        "is_flat",
        "offsets",
        "offsets_span",
        "valid",
        "handle",
        "release",
        "child",
        "num_children",
        "list",
        "strings",
        "structs",
        "nulls",
        "flat",
        "flat_i64",
        "flat_f64",
        "string_at",
        "share",
        "from_arrow",
        "from_borrowed",
        "to_arrow",
    },
    "DataFrame": {
        "column",
        "column_index",
        "num_columns",
        "num_rows",
        "from_arrow",
        "to_arrow",
        "to_ipc",
        "stream",
        "lazy",
    },
    "LazyFrame": {
        "collect",
        "collect_group_state",
        "explain",
        "scan",
        "schema",
        "stream",
    },
}


# Python methods backed by a registered op under another leaf name, on top of
# FOLDS: the pandas / polars spellings the wrapper adds as thin aliases.
PY_ALIASES = {
    "Series": {
        "astype": "cast",
        "isna": "null_mask",
        "isnull": "null_mask",
        "notna": "valid_mask",
        "notnull": "valid_mask",
        "is_not_null": "valid_mask",
        "isin": "is_in",
        "dropna": "drop_nulls",
        "fill_null": "fillna",
        "full_like": "add_scalar",
        "var": "variance",
        "std": "stddev",
        "skew": "skewness",
        "kurt": "kurtosis",
        "n_unique": "nunique",
        "prod": "product",
        "argmin": "arg_min",
        "argmax": "arg_max",
        "idxmin": "arg_min",
        "idxmax": "arg_max",
        "arg_sort": "argsort",
        "cumprod": "cum_prod",
        "cum_sum": "cumsum",
        "cum_max": "cummax",
        "cum_min": "cummin",
        "sort_values": "sort",
        "nlargest": "top_k",
        "nsmallest": "bottom_k",
        "searchsorted": "search_sorted",
        "gather": "take",
        "rolling_sum": "rolling",
        "rolling_mean": "rolling",
        "rolling_min": "rolling",
        "rolling_max": "rolling",
        "ewm": "ewm_mean",
        "expanding": "cumsum",
        "unique_mask": "is_unique",
        "mask": "where",
        # The pandas Series surface as compositions (python/dftracer/utils/
        # _pandas_series.py): each names the kernel it runs.
        "between": "is_between",
        "drop_duplicates": "unique",
        "duplicated": "is_duplicated",
        "is_monotonic_increasing": "is_sorted",
        "is_monotonic_decreasing": "is_sorted",
        "factorize": "search_sorted",
        "repeat": "take",
        "replace": "where",
        "case_when": "where",
        "truncate": "slice",
        "iloc": "take",
        "loc": "take",
        "iat": "take",
        "at": "take",
        "first_valid_index": "valid_mask",
        "last_valid_index": "valid_mask",
        "corr": "reduce",
        "cov": "reduce",
        "autocorr": "reduce",
        "sem": "stddev",
        "asof": "ffill",
        "combine_first": "where",
        "update": "where",
        "drop": "take",
        "xs": "take",
        "sort_index": "reverse",
        "equals": "compare",
        "pop": "take",
        "describe": "reduce",
        "agg": "reduce",
        "aggregate": "reduce",
        "transform": "reduce",
        "divide": "div",
        "truediv": "div",
        "multiply": "mul",
        "subtract": "sub",
        "radd": "add",
        "rsub": "sub",
        "rmul": "mul",
        "rdiv": "div",
        "rtruediv": "div",
        "rfloordiv": "floordiv",
        "rmod": "mod",
        "rpow": "pow",
        "divmod": "floordiv",
        "rdivmod": "floordiv",
        "pad": "ffill",
        "backfill": "bfill",
        "groupby": "reduce",
        "explode": "take",
        "reset_index": "take",
    },
    "DataFrame": {
        "distinct": "unique",
        "top_k": "topk",
        "sort": "sort_by",
        "sort_values": "sort_by_multi",
        "merge": "join",
        "query": "mask",
        "limit": "head",
        "drop": "select",
        "assign": "with_column",
        "with_columns": "with_column",
        "cast": "with_column",
        "astype": "with_column",
        "dropna": "drop_nulls",
        "fillna": "fill_null",
        "duplicated": "is_duplicated",
        "gather": "take",
        "groupby": "group_by",
        "nlargest": "topk",
        "nsmallest": "topk",
        "pivot_table": "pivot",
        "agg": "group_by",
        "aggregate": "group_by",
        "sum": "reduce",
        "mean": "reduce",
        "min": "reduce",
        "max": "reduce",
        "count": "reduce",
        "var": "reduce",
        "std": "reduce",
        "skew": "reduce",
        "kurt": "reduce",
        # The index is a column: positions are slice / take / filter, labels
        # a filter over the index column, a write a masked with_column, and
        # resample a group_by_dynamic over the time column.
        "iloc": "slice",
        "loc": "filter",
        "at": "filter",
        "iat": "slice",
        "at_time": "filter",
        "between_time": "filter",
        "first": "filter",
        "last": "filter",
        "asfreq": "join",
        "tz_localize": "with_timezone",
        "tz_convert": "with_timezone",
        "reset_index": "with_row_index",
        "resample": "group_by_dynamic",
        # The pandas DataFrame surface as compositions (python/dftracer/
        # utils/_pandas_frame.py): column-wise Series kernels, the one-row
        # reductions, and the conveniences over with_column / select / take.
        "abs": "abs",
        "round": "round",
        "clip": "clip",
        "cumsum": "cumsum",
        "rolling": "rolling",
        "expanding": "cumsum",
        "ewm": "ewm_mean",
        "cummax": "cummax",
        "cummin": "cummin",
        "cumprod": "cum_prod",
        "diff": "diff",
        "pct_change": "pct_change",
        "shift": "shift",
        "rank": "rank",
        "interpolate": "interpolate",
        "isna": "null_mask",
        "isnull": "null_mask",
        "notna": "valid_mask",
        "notnull": "valid_mask",
        "where": "where",
        "replace": "where",
        "applymap": "with_column",
        "map": "with_column",
        "transform": "with_column",
        "add": "add",
        "sub": "sub",
        "mul": "mul",
        "div": "div",
        "radd": "add",
        "rsub": "sub",
        "rmul": "mul",
        "rdiv": "div",
        "truediv": "div",
        "rtruediv": "div",
        "divide": "div",
        "multiply": "mul",
        "subtract": "sub",
        "floordiv": "floordiv",
        "rfloordiv": "floordiv",
        "mod": "mod",
        "rmod": "mod",
        "pow": "pow",
        "rpow": "pow",
        "ffill": "ffill",
        "bfill": "bfill",
        "pad": "ffill",
        "backfill": "bfill",
        "eq": "compare",
        "ne": "compare",
        "lt": "compare",
        "le": "compare",
        "gt": "compare",
        "ge": "compare",
        "median": "quantile",
        "quantile": "quantile",
        "nunique": "nunique",
        "prod": "product",
        "product": "product",
        "sem": "stddev",
        "mode": "mode",
        "idxmax": "arg_max",
        "idxmin": "arg_min",
        "any": "any",
        "all": "all",
        "corr": "group_by",
        "cov": "group_by",
        "corrwith": "group_by",
        "value_counts": "group_by",
        "combine_first": "where",
        "combine": "with_column",
        "compare": "compare",
        "isin": "is_in",
        "info": "null_count",
        "rename_axis": "rename",
        "kurtosis": "reduce",
        "swapaxes": "select",
        "update": "where",
        "dot": "dot",
        "xs": "filter",
        "truncate": "filter",
        "sort_index": "sort_by_multi",
        "first_valid_index": "valid_mask",
        "last_valid_index": "valid_mask",
        "select_dtypes": "select",
        "add_prefix": "rename",
        "add_suffix": "rename",
        "set_axis": "rename",
        "pop": "select",
        "insert": "with_column",
        "squeeze": "select",
        "transpose": "select",
        "T": "select",
        "to_numpy": "select",
        "values": "select",
        "memory_usage": "select",
        # The polars frame names (section 21.2).
        "sum_horizontal": "add",
        "mean_horizontal": "add",
        "min_horizontal": "where",
        "max_horizontal": "where",
        "hstack": "with_column",
        "vstack": "concat",
        "gather_every": "take",
        "partition_by": "take",
        "join_asof": "asof",
        "get_column": "select",
        "get_columns": "select",
        "get_column_index": "select",
        "to_series": "select",
        "is_empty": "select",
        "n_unique": "unique",
        "fill_nan": "where",
        "drop_nans": "filter",
    },
    "LazyFrame": {
        "with_columns": "with_column",
        "column_op": "column_op",
        "drop": "select",
        "iloc": "slice",
        "loc": "filter",
        "reset_index": "with_row_index",
        "resample": "group_by_dynamic",
        "limit": "head",
        "sort": "sort_by",
        "sort_values": "sort_by",
        "gather": "take",
        "union": "concat",
        "window": "window",
        "gap_fill": "gap_fill",
        "asof": "asof",
        "interval": "interval",
        "agg": "group_by",
        "aggregate": "group_by",
        "sum": "reduce",
        "mean": "reduce",
        "min": "reduce",
        "max": "reduce",
        "count": "reduce",
        "var": "reduce",
        "std": "reduce",
        "skew": "reduce",
        "kurt": "reduce",
    },
}

# Python surface that is not an op: construction, conversion, handle
# accessors, pickling and the lazy terminals.
PY_ALLOWLIST = {
    "Series": {
        "from_arrow",
        "from_list",
        "from_numpy",
        "from_pandas",
        "from_polars",
        "to_arrow",
        "to_numpy",
        "to_pandas",
        "to_polars",
        "ops",
        "child",
        "encoding",
        "is_null",
        "length",
        "null_count",
        "num_children",
        "time_unit",
        "timezone",
        # Handles, shape, iteration and exports (through pandas / pyarrow).
        "array",
        "values",
        "copy",
        "pipe",
        "empty",
        "ndim",
        "nbytes",
        "memory_usage",
        "name",
        "hasnans",
        "item",
        "items",
        "keys",
        "get",
        "to_dict",
        "to_frame",
        "to_csv",
        "to_json",
        "to_string",
        "share",
        "type",
        "dtype",
        "size",
        "shape",
        "len",
        "to_list",
        "tolist",
        # apply / map run a user function: through the Expr engine when it
        # traces, else in Python; the engine op is whatever the trace builds.
        "apply",
        "map",
    },
    "DataFrame": {
        "from_arrow",
        "from_dict",
        "from_numpy",
        "from_pandas",
        "from_parquet",
        "from_polars",
        "to_arrow",
        "to_ipc",
        "to_pandas",
        "to_polars",
        "to_dict",
        # The Arrow exports under their polars names (rows, cells, writers).
        "iter_rows",
        "rows",
        "row",
        "item",
        "to_dicts",
        "write_parquet",
        "write_csv",
        "write_ipc",
        "column_index",
        "column_names",
        "columns",
        "keys",
        "lazy",
        "num_columns",
        "num_rows",
        "height",
        "width",
        "shape",
        "dtypes",
        "schema",
        "apply",
        # The spec list a broadcast lowers to; the op is group_by.
        "reduce_specs",
        # The index column's name is wrapper metadata; the column is data.
        "set_index",
        "index",
        "index_levels",
        # Handles, shape, iteration and exports (through pandas / pyarrow).
        "copy",
        "pipe",
        "empty",
        "ndim",
        "size",
        "axes",
        "items",
        "iterrows",
        "itertuples",
        "get",
        "equals",
        "from_records",
        "to_csv",
        "to_json",
        "to_string",
        "to_markdown",
        "to_html",
        "to_records",
        "to_parquet",
        "to_feather",
    },
    "LazyFrame": {
        "collect",
        "columns",
        "explain",
        "output_schema",
        "reduce_specs",
        "schema",
        "set_index",
        "stream",
    },
}

# Python methods whose logic is not an engine op. Measured 2026-09-16 at nine
# DataFrame methods, paid down to none on 2026-09-17; an entry here is a
# parity gap by section 19's rule and the pytest bound holds it at zero.
PYTHON_ONLY_DEBT = {"Series": set(), "DataFrame": set(), "LazyFrame": set()}

# Registered ops with no Python method of their own, reached another way:
# the value names the Python method (or the `ops` accessor) that runs them.
REACHED_AS = {
    "series": {
        "reduce": "sum/mean/median/min/max",
        "floordiv_scalar": "floordiv",
        "mod_scalar": "mod",
        "pow_scalar": "pow",
        "format64": "hex64_format",
        "parse64": "hex64_parse",
        "filter_gt": "ops",
    },
    "frame": {
        "sort_by_multi_per_col": "sort_by_multi",
        "mask": "query",
        "column_op": "LazyFrame.column_op",
        "value_counts": "Series.value_counts",
        "unique_by": "unique",
    },
    "lazy": {"unique_by": "unique"},
}


def python_surface():
    """(class name -> public method names, registry leaves by bucket) from the
    installed dftracer.utils extension. A wrapper forwards unknown attributes
    to its native handle, so the surface is the union of both."""
    import dftracer.utils as u
    from dftracer.utils import dftracer_utils_ext as ext

    pairs = {
        "Series": (u.Series, ext._Series),
        "DataFrame": (u.DataFrame, ext._DataFrame),
        "LazyFrame": (u.LazyFrame, ext._LazyFrame),
    }
    surface = {}
    for name, (wrapper, native) in pairs.items():
        names = set(dir(wrapper)) | set(dir(native))
        surface[name] = {n for n in names if not n.startswith("_")}
    # The host utility families (dftu.fs.*, dftu.file.*, dftu.text.*) are
    # plugin services with no column, so they are not Series methods; only
    # dftu.hash.* and dftu.hex.* are, as in registry_leaf_names.
    by_bucket = {"series": set(), "frame": set(), "lazy": set()}
    for op in ext.op_list():
        leaf = op.rsplit(".", 1)[-1]
        if op.startswith("dftu.frame."):
            by_bucket["frame"].add(leaf)
        elif op.startswith("dftu.lazy."):
            by_bucket["lazy"].add(leaf)
        elif op.startswith(("dftu.series.", "dftu.hash.", "dftu.hex.")):
            by_bucket["series"].add(leaf)
    return surface, by_bucket


def check_python() -> list[str]:
    """Drift lines for the Python surface; empty when parity holds."""
    surface, registered = python_surface()
    every_leaf = set().union(*registered.values())
    problems = []
    for class_name, names in surface.items():
        bucket = REGISTRY_BUCKET[class_name]
        reg = registered[bucket]
        backed = {**FOLDS[class_name], **PY_ALIASES[class_name]}
        python_only = sorted(
            n
            for n in names
            if n not in reg
            and backed.get(n) not in every_leaf
            and n not in PY_ALLOWLIST[class_name]
            and n not in PYTHON_ONLY_DEBT[class_name]
        )
        stale_debt = sorted(n for n in PYTHON_ONLY_DEBT[class_name] if n not in names or n in reg)
        reached = {v for v in REACHED_AS[bucket]}
        unreached = sorted(
            leaf
            for leaf in reg
            if leaf not in names and leaf not in backed.values() and leaf not in reached
        )
        print(
            f"{class_name}: {len(names)} Python methods, {len(reg)} "
            f"registered, {len(PYTHON_ONLY_DEBT[class_name])} Python-only "
            f"debt"
        )
        if python_only:
            problems.append(
                f"{class_name}: Python methods with no op behind them: {', '.join(python_only)}"
            )
        if stale_debt:
            problems.append(
                f"{class_name}: PYTHON_ONLY_DEBT entries that are "
                f"no longer debt: {', '.join(stale_debt)}"
            )
        if unreached:
            problems.append(
                f"{class_name}: registered ops no Python method reaches: {', '.join(unreached)}"
            )
    return problems


class OpDesc(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("sig", ctypes.c_int64),
        ("fn", ctypes.c_void_p),
    ]


def public_methods(xml_path: Path, class_name: str) -> set[str]:
    xml = xml_path.read_text()
    names = set()
    for block in MEMBER_RE.findall(xml):
        prot = PROT_RE.search(block)
        if not prot or prot.group(1) != "public":
            continue
        name_m = NAME_RE.search(block)
        if not name_m:
            continue
        name = name_m.group(1)
        if name in (class_name, "~" + class_name) or name.startswith("operator"):
            continue
        names.add(name)
    return names


def registry_leaf_names(lib_path: Path) -> dict[str, set[str]]:
    # Bucketed by registry name prefix, not by dftu_op_kind: kind comes from
    # the return token, so a frame -> series op (mask, is_unique) is kind
    # SERIES while still being a DataFrame method. The prefix is what says
    # which class owns the op.
    lib = ctypes.CDLL(str(lib_path))
    lib.dftu_op_count.restype = ctypes.c_uint32
    lib.dftu_op_at.restype = ctypes.POINTER(OpDesc)
    lib.dftu_op_at.argtypes = [ctypes.c_uint32]

    by_bucket: dict[str, set[str]] = {"series": set(), "frame": set(), "lazy": set()}
    for i in range(lib.dftu_op_count()):
        name = lib.dftu_op_at(i).contents.name.decode()
        leaf = name.rsplit(".", 1)[-1]
        if name.startswith("dftu.frame."):
            by_bucket["frame"].add(leaf)
        elif name.startswith("dftu.lazy."):
            by_bucket["lazy"].add(leaf)
        else:
            # dftu.series.* plus the host utility families (dftu.hash.*,
            # dftu.hex.*), which are reached as Series methods.
            by_bucket["series"].add(leaf)
    return by_bucket


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("doxygen_xml_dir", type=Path, nargs="?")
    ap.add_argument(
        "dataframe_library",
        type=Path,
        nargs="?",
        help="the built libdftracer_utils_dataframe shared library",
    )
    ap.add_argument(
        "--python",
        action="store_true",
        help="check the installed Python surface instead of the "
        "Doxygen XML (needs no other argument)",
    )
    args = ap.parse_args()

    if args.python:
        problems = check_python()
        for line in problems:
            print("  DRIFT " + line)
        return 1 if problems else 0
    if args.doxygen_xml_dir is None or args.dataframe_library is None:
        ap.error("doxygen_xml_dir and dataframe_library are required without --python")

    registered = registry_leaf_names(args.dataframe_library)
    every_leaf = set().union(*registered.values())

    failed = False
    for class_name, xml_name in CLASSES.items():
        xml_path = args.doxygen_xml_dir / xml_name
        if not xml_path.is_file():
            print(f"missing {xml_path}", file=sys.stderr)
            return 1
        names = public_methods(xml_path, class_name)
        reg = registered[REGISTRY_BUCKET[class_name]]
        folds = FOLDS[class_name]
        drifted = sorted(
            n
            for n in names
            if n not in reg and folds.get(n) not in every_leaf and n not in ALLOWLIST[class_name]
        )
        print(
            f"{class_name}: {len(names)} methods, {len(reg)} registered, "
            f"{len(folds)} folded, {len(ALLOWLIST[class_name])} allowlisted"
        )
        if drifted:
            failed = True
            print(f"  DRIFT: {', '.join(drifted)}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
