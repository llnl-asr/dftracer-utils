"""Tests for ReorganizationPlannerUtility."""

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import ReorganizationPlannerUtility

from .common import Environment


class TestReorganizationPlannerUtility:
    def test_plan_returns_dict(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, build_manifest=True
            ) as indexer:
                indexer.build()
            groups = [{"name": "posix", "predicate": "cat=cat_1"}]
            result = ReorganizationPlannerUtility().process(source_files=[gz_file], groups=groups)
            assert isinstance(result, dict)
            assert "groups" in result
            assert "source_files" in result
            assert "tasks" in result
            assert "total_events" in result

    def test_call_delegates_to_process(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(
                gz_file, idx_file, build_bloom=True, build_manifest=True
            ) as indexer:
                indexer.build()
            util = ReorganizationPlannerUtility()
            groups = [{"name": "posix", "predicate": "cat=cat_1"}]
            result = util(source_files=[gz_file], groups=groups)
            assert isinstance(result, dict)
            assert "tasks" in result
