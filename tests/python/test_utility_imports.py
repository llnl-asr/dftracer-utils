# SPDX-License-Identifier: MIT
"""Tests for utility import paths."""


class TestUtilityImports:
    def test_import_query_col(self):
        from dftracer.utils import Expr, col

        cat = col("cat")
        q = cat == "POSIX"
        assert isinstance(q, Expr)
        assert 'cat == "POSIX"' in str(q)
