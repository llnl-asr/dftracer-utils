"""Tests for utility import paths."""


class TestUtilityImports:
    def test_import_from_utilities_subpackage(self):
        from dftracer.utils.utilities import (
            BloomQueryUtility,
            MetadataCollectorUtility,
            ReconstructionPlannerUtility,
            ReorganizationPlannerUtility,
            StatisticsAggregatorUtility,
            StatisticsQueryUtility,
            ViewBuilderUtility,
            ViewReaderUtility,
        )

        for cls in [
            StatisticsQueryUtility,
            BloomQueryUtility,
            StatisticsAggregatorUtility,
            MetadataCollectorUtility,
            ViewBuilderUtility,
            ViewReaderUtility,
            ReorganizationPlannerUtility,
            ReconstructionPlannerUtility,
        ]:
            assert cls is not None

    def test_import_from_ext_directly(self):
        from dftracer.utils.dftracer_utils_ext import (
            StatisticsQueryUtility,
        )

        assert StatisticsQueryUtility is not None
