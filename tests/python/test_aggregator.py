"""Tests for AggregatorUtility."""

from dftracer.utils.arrow import ArrowTable
from dftracer.utils.dftracer_utils_ext import AggregatorUtility

from .common import Environment


class TestAggregatorUtility:
    def test_process_returns_arrow_table(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert isinstance(result, ArrowTable)

    def test_process_has_rows(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert result.num_rows > 0

    def test_process_batches_have_arrow_protocol(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            for batch in result.batches():
                assert hasattr(batch, "__arrow_c_array__")
                assert batch.num_columns == 18  # fixed schema

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            util = AggregatorUtility()
            result = util(directory)
            assert isinstance(result, ArrowTable)

    def test_iter_arrow_streams_batches(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            util = AggregatorUtility()
            batches = list(util.iter_arrow(directory))
            assert len(batches) >= 1
            for batch in batches:
                assert hasattr(batch, "__arrow_c_array__")
                assert batch.num_rows > 0

    def test_process_with_categories_filter(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory, categories=["cat_1"])
            assert isinstance(result, ArrowTable)

    def test_process_with_time_interval(self):
        with Environment(lines=20) as env:
            env.create_test_gzip_file()
            directory = env.temp_dir
            result = AggregatorUtility().process(directory, time_interval_ms=1000.0)
            assert isinstance(result, ArrowTable)
            assert result.num_rows > 0

    def test_process_empty_directory(self):
        with Environment(lines=0) as env:
            directory = env.temp_dir
            result = AggregatorUtility().process(directory)
            assert isinstance(result, ArrowTable)
            assert result.num_rows == 0
