"""Tests for utility import paths."""


class TestUtilityImports:
    def test_import_from_utilities_subpackage(self):
        from dftracer.utils.utilities import (
            AggregatorUtility,
            MetadataCollectorUtility,
            ReconstructionPlannerUtility,
            ReorganizationPlannerUtility,
            StatisticsAggregatorUtility,
            StatisticsQueryUtility,
        )

        for cls in [
            AggregatorUtility,
            StatisticsQueryUtility,
            StatisticsAggregatorUtility,
            MetadataCollectorUtility,
            ReorganizationPlannerUtility,
            ReconstructionPlannerUtility,
        ]:
            assert cls is not None

    def test_import_from_ext_directly(self):
        from dftracer.utils.dftracer_utils_ext import (
            StatisticsQueryUtility,
        )

        assert StatisticsQueryUtility is not None

    def test_import_query_field(self):
        from dftracer.utils.query import Expr, Field

        cat = Field("cat")
        q = cat == "POSIX"
        assert isinstance(q, Expr)
        assert 'cat == "POSIX"' in str(q)
