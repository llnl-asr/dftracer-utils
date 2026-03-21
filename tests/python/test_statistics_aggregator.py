"""Tests for StatisticsAggregatorUtility."""

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import StatisticsAggregatorUtility

from .common import Environment


class TestStatisticsAggregatorUtility:
    def test_compute_returns_dict(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = StatisticsAggregatorUtility().process(gz_file)
            assert isinstance(result, dict)
            assert "total_events" in result
            assert "success" in result

    def test_compute_correct_event_count(self):
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = StatisticsAggregatorUtility().process(gz_file)
            assert result["success"] is True
            assert result["total_events"] == 30

    def test_compute_has_statistics_fields(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = StatisticsAggregatorUtility().process(gz_file)
            assert "num_categories" in result
            assert "num_unique_names" in result
            assert "duration_mean_us" in result
            assert "min_timestamp_us" in result
            assert "max_timestamp_us" in result

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            util = StatisticsAggregatorUtility()
            result = util(gz_file)
            assert isinstance(result, dict)
            assert "total_events" in result
