"""Tests for ReorganizationPlannerUtility."""

import sys

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import ReorganizationPlannerUtility

from .common import Environment

# Threshold large enough to guarantee bloom/manifest are skipped for any
# test fixture, making WithoutIndex tests deterministic regardless of
# fixture size.
_SKIP_INDEX_THRESHOLD = sys.maxsize


class TestReorganizationPlannerUtility:
    def test_plan_returns_dict(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                build_manifest=True,
                index_threshold=0,
            ) as indexer:
                indexer.build()
            groups = [{"name": "posix", "query": 'cat == "POSIX"'}]
            result = ReorganizationPlannerUtility().process(source_files=[gz_file], groups=groups)
            assert isinstance(result, dict)
            assert "groups" in result
            assert "source_files" in result
            assert "tasks" in result
            assert "total_events" in result

    def test_call_delegates_to_process(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                build_manifest=True,
                index_threshold=0,
            ) as indexer:
                indexer.build()
            util = ReorganizationPlannerUtility()
            groups = [{"name": "posix", "query": 'cat == "POSIX"'}]
            result = util(source_files=[gz_file], groups=groups)
            assert isinstance(result, dict)
            assert "tasks" in result


class TestReorganizationPlannerWithoutIndex:
    """Reorganization planner falls back to whole-file streaming without manifest."""

    def test_plan_succeeds_without_manifest(self):
        """Without manifest the planner streams the file and succeeds."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=128)
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                build_manifest=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_manifest
            groups = [{"name": "posix", "query": 'cat == "POSIX"'}]
            result = ReorganizationPlannerUtility().process(source_files=[gz_file], groups=groups)
            assert isinstance(result, dict)
            assert "tasks" in result
            assert "total_events" in result
            assert result["total_events"] > 0

    def test_plan_has_tasks_without_manifest(self):
        """Whole-file fallback produces extraction tasks."""
        with Environment(lines=5) as env:
            gz_file = env.create_test_gzip_file(bytes_per_line=128)
            index_path = env.get_index_path(gz_file)
            with dft_utils.Indexer(
                gz_file,
                index_path,
                build_bloom=True,
                build_manifest=True,
                index_threshold=_SKIP_INDEX_THRESHOLD,
            ) as indexer:
                indexer.build()
                assert not indexer.has_manifest
            groups = [{"name": "posix", "query": 'cat == "POSIX"'}]
            result = ReorganizationPlannerUtility().process(source_files=[gz_file], groups=groups)
            assert len(result["tasks"]) > 0
            for task in result["tasks"]:
                assert task["start_byte"] == 0
                assert task["end_byte"] > 0
