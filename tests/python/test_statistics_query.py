"""Tests for StatisticsQueryUtility."""

import sys

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import StatisticsQueryUtility

from .common import Environment

# Threshold large enough to guarantee bloom/manifest are skipped for any
# test fixture, making WithoutIndex tests deterministic regardless of
# fixture size.
_SKIP_INDEX_THRESHOLD = sys.maxsize


class TestStatisticsQueryUtility:
    def test_query_summary(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsQueryUtility().process(gz_file, query_type="summary")
            assert isinstance(result, dict)
            assert "total_events" in result
            assert result["total_events"] == 20

    def test_query_categories(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsQueryUtility().process(gz_file, query_type="categories")
            assert "results" in result
            assert isinstance(result["results"], list)

    def test_query_names(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsQueryUtility().process(gz_file, query_type="names")
            assert "results" in result

    def test_query_top_n_names(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsQueryUtility().process(gz_file, query_type="top_n_names", top_n=5)
            assert "results" in result
            assert len(result["results"]) <= 5

    def test_query_duration_stats(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            result = StatisticsQueryUtility().process(gz_file, query_type="duration_stats")
            assert "duration_mean_us" in result
            assert "duration_count" in result

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, index_threshold=0
            ) as indexer:
                indexer.build()
            util = StatisticsQueryUtility()
            result = util(gz_file, query_type="summary")
            assert isinstance(result, dict)
            assert "total_events" in result


class TestStatisticsQueryWithoutIndex:
    """Statistics query falls back to sequential reading without bloom."""

    def test_summary_correct_events_without_bloom(self):
        """Sequential fallback produces correct event count."""
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file,
                idx_file,
                build_bloom=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_bloom
            result = StatisticsQueryUtility().process(gz_file, query_type="summary")
            assert isinstance(result, dict)
            assert result["total_events"] == 20

    def test_categories_populated_without_bloom(self):
        """Sequential fallback populates categories."""
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file,
                idx_file,
                build_bloom=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_bloom
            result = StatisticsQueryUtility().process(gz_file, query_type="categories")
            assert "results" in result
            assert isinstance(result["results"], list)
            assert len(result["results"]) > 0
