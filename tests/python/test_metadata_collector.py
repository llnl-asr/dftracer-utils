"""Tests for MetadataCollectorUtility."""

from dftracer.utils.dftracer_utils_ext import MetadataCollectorUtility

from .common import Environment


class TestMetadataCollectorUtility:
    def test_collect_returns_dict(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            result = MetadataCollectorUtility().process(gz_file)
            assert isinstance(result, dict)
            assert "file_path" in result
            assert "success" in result

    def test_collect_file_path_matches(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            result = MetadataCollectorUtility().process(gz_file)
            assert result["file_path"] == gz_file

    def test_collect_has_size_info(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            result = MetadataCollectorUtility().process(gz_file)
            assert "size_mb" in result
            assert result["size_mb"] > 0
            assert "valid_events" in result
            assert "format" in result

    def test_call_delegates_to_process(self):
        with Environment(lines=10) as env:
            gz_file = env.create_test_gzip_file()
            util = MetadataCollectorUtility()
            result = util(gz_file)
            assert isinstance(result, dict)
            assert "file_path" in result
