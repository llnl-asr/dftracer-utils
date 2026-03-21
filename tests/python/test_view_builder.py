"""Tests for ViewBuilderUtility."""

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import ViewBuilderUtility

from .common import Environment


class TestViewBuilderUtility:
    def test_build_with_predicates(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = ViewBuilderUtility().process(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, dict)
            assert "file_may_match" in result
            assert "candidates" in result
            assert "success" in result

    def test_build_without_predicates(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = ViewBuilderUtility().process(gz_file)
            assert isinstance(result, dict)

    def test_call_delegates_to_process(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            util = ViewBuilderUtility()
            result = util(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, dict)
            assert "file_may_match" in result
