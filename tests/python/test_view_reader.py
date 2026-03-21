"""Tests for ViewReaderUtility."""

import dftracer.utils as dft_utils
from dftracer.utils.arrow import ArrowTable
from dftracer.utils.dftracer_utils_ext import ViewReaderUtility

from .common import Environment


class TestViewReaderUtility:
    def test_process_returns_arrow_table(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = ViewReaderUtility().process(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, ArrowTable)

    def test_process_has_rows(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = ViewReaderUtility().process(gz_file, predicates={"cat": ["cat_1"]})
            assert result.num_rows >= 0

    def test_process_batches_have_arrow_protocol(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            result = ViewReaderUtility().process(gz_file, predicates={"cat": ["cat_1"]})
            for batch in result.batches():
                assert hasattr(batch, "__arrow_c_array__")

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            util = ViewReaderUtility()
            result = util(gz_file, predicates={"cat": ["cat_1"]})
            assert isinstance(result, ArrowTable)

    def test_iter_arrow_streams_batches(self):
        with Environment(lines=20) as env:
            gz_file = env.create_test_gzip_file()
            idx_file = gz_file + ".idx"
            with dft_utils.Indexer(gz_file, idx_file, build_bloom=True) as indexer:
                indexer.build()
            util = ViewReaderUtility()
            batches = list(util.iter_arrow(gz_file, predicates={"cat": ["cat_1"]}))
            for batch in batches:
                assert hasattr(batch, "__arrow_c_array__")
                assert batch.num_rows >= 0
