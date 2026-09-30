"""Tests for utility import paths."""


class TestUtilityImports:
    def test_import_query_field(self):
        from dftracer.utils.columnar import Expr, Field

        cat = Field("cat")
        q = cat == "POSIX"
        assert isinstance(q, Expr)
        assert 'cat == "POSIX"' in str(q)
