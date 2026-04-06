"""Tests for StatisticsAggregatorUtility."""

import sys

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import StatisticsAggregatorUtility

from .common import Environment

# Threshold large enough to guarantee bloom/manifest are skipped for any
# test fixture, making WithoutIndex tests deterministic regardless of
# fixture size.
_SKIP_INDEX_THRESHOLD = sys.maxsize


class TestStatisticsAggregatorUtility:
    def test_compute_returns_dict(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file, index_path, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsAggregatorUtility().process(gz_file)
            assert isinstance(result, dict)
            assert "total_events" in result
            assert "success" in result

    def test_compute_correct_event_count(self):
        with Environment(lines=30) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file, index_path, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsAggregatorUtility().process(gz_file)
            assert result["success"] is True
            assert result["total_events"] == 30

    def test_compute_has_statistics_fields(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file, index_path, build_bloom=True, index_threshold=0
            ) as indexer:
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
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file, index_path, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            util = StatisticsAggregatorUtility()
            result = util(gz_file)
            assert isinstance(result, dict)
            assert "total_events" in result


class TestStatisticsAggregatorWithoutIndex:
    """Statistics aggregator falls back to sequential reading without bloom."""

    def test_returns_dict_without_bloom(self):
        """Without bloom data the aggregator streams the file and succeeds."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_bloom
            result = StatisticsAggregatorUtility().process(gz_file)
            assert isinstance(result, dict)
            assert "total_events" in result
            assert "success" in result
            assert result["success"] is True

    def test_correct_event_count_without_bloom(self):
        """Sequential fallback produces the same event count as indexed path."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_bloom
            result = StatisticsAggregatorUtility().process(gz_file)
            assert result["success"] is True
            assert result["total_events"] == 20

    def test_has_statistics_fields_without_bloom(self):
        """Sequential fallback populates all statistics fields."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_bloom
            result = StatisticsAggregatorUtility().process(gz_file)
            assert result["success"] is True
            assert "num_categories" in result
            assert "num_unique_names" in result
            assert "duration_mean_us" in result
            assert "min_timestamp_us" in result
            assert "max_timestamp_us" in result
