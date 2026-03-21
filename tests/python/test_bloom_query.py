"""Tests for BloomQueryUtility."""

import dftracer.utils as dft_utils
from dftracer.utils.dftracer_utils_ext import BloomQueryUtility

from .common import Environment


class TestBloomQueryUtility:
    def test_query_with_index(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = BloomQueryUtility().process(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, dict)
            assert "file_may_match" in result
            assert "candidate_checkpoints" in result
            assert "success" in result

    def test_query_nonexistent_category(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = BloomQueryUtility().process(gz_file, predicates={"cat": ["NONEXISTENT"]})
            assert isinstance(result, dict)

    def test_call_delegates_to_process(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            util = BloomQueryUtility()
            result = util(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, dict)
            assert "file_may_match" in result
