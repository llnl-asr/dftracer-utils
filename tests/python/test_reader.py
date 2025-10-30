#!/usr/bin/env python3
"""
Test cases for DFTracer Python bindings - updated for new unified API
"""

import pytest

import dftracer.utils as dft_utils
from .common import Environment

class TestReader:
    """Test cases for Reader - unified reader with multiple read methods"""
    
    def test_import(self):
        """Test that we can import the module and classes"""
        assert hasattr(dft_utils, 'Reader')
        assert hasattr(dft_utils, 'Indexer')
        assert hasattr(dft_utils, 'IndexerCheckpoint')
    
    def test_reader_creation_nonexistent_file(self):
        """Test reader creation with non-existent file"""
        with pytest.raises(RuntimeError):
            dft_utils.Reader("nonexistent_file.pfw.gz")
    
    def test_reader_creation_missing_index(self):
        """Test reader creation when index file doesn't exist"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            
            indexer = dft_utils.Indexer(gz_file)
            indexer.build()
            
            # Reader should work with indexer
            with dft_utils.Reader(gz_file, indexer=indexer) as reader:
                assert reader.get_max_bytes() > 0
    
    def test_reader_creation_from_indexer(self):
        """Test reader creation from indexer"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            indexer = env.create_indexer(gz_file)
            
            # Test creating reader from indexer
            reader = dft_utils.Reader(gz_file, indexer=indexer)
            assert reader.get_max_bytes() > 0
            assert reader.gz_path == gz_file
    
    def test_reader_basic_functionality(self):
        """Test reader basic functionality"""
        with Environment() as env:
            # Create and index test file
            gz_file = env.create_test_gzip_file()
            idx_file = env.build_index(gz_file, checkpoint_size_bytes=512*1024)
            
            # Test context manager
            with dft_utils.Reader(gz_file, idx_file) as reader:
                assert reader.get_max_bytes() > 0
                assert reader.gz_path == gz_file
                assert reader.idx_path == idx_file
            
            # Should be able to create another one
            with dft_utils.Reader(gz_file, idx_file) as reader2:
                assert reader2.get_max_bytes() > 0
    
    def test_reader_properties(self):
        """Test reader properties"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            idx_file = env.build_index(gz_file, checkpoint_size_bytes=1536*1024)
            
            with dft_utils.Reader(gz_file, idx_file) as reader:
                # Test property types
                assert isinstance(reader.gz_path, str)
                assert isinstance(reader.idx_path, str) 
                assert isinstance(reader.get_max_bytes(), int)
                assert isinstance(reader.get_num_lines(), int)
                assert isinstance(reader.checkpoint_size, int)
                assert isinstance(reader.buffer_size, int)
                
                # Test getter methods
                assert reader.gz_path == gz_file
                assert reader.idx_path == idx_file
                assert reader.get_max_bytes() > 0
                
                # Test setting buffer size
                original_size = reader.buffer_size
                reader.buffer_size = 512 * 1024
                assert reader.buffer_size == 512 * 1024
                reader.buffer_size = original_size
    
    def test_reader_data_reading_bytes(self):
        """Test reader raw bytes reading"""
        with Environment() as env:
            bytes_per_line = 1024
            gz_file = env.create_test_gzip_file(bytes_per_line=bytes_per_line)
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)
            
            with dft_utils.Reader(gz_file) as reader:
                # read() returns raw bytes
                data = reader.read(0, bytes_per_line)

                assert isinstance(data, bytes)
                assert len(data) == bytes_per_line
                
                # Check content
                if data:
                    assert b'"name"' in data
    
    def test_reader_line_based_reading(self):
        """Test reader line-based reading methods"""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)
            
            with dft_utils.Reader(gz_file) as reader:
                num_lines = reader.get_num_lines()
                assert num_lines > 0
                
                # Test read_lines - operates on line numbers (1-based)
                if num_lines > 10:
                    lines = reader.read_lines(1, 6)
                    assert isinstance(lines, list)
                    assert all(isinstance(line, str) for line in lines)
                    assert len(lines) <= 6  # 0-5 inclusive
                    
                    # Test read_line_bytes - operates on byte ranges
                    max_bytes = reader.get_max_bytes()
                    if max_bytes > 1024:
                        line_bytes = reader.read_line_bytes(0, 1024)
                        assert isinstance(line_bytes, list)
                        assert all(isinstance(line, str) for line in line_bytes)
    
    def test_reader_json_methods(self):
        """Test reader JSON parsing methods"""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)
            
            with dft_utils.Reader(gz_file) as reader:
                num_lines = reader.get_num_lines()
                max_bytes = reader.get_max_bytes()
                
                # Test read_lines_json - operates on line numbers (1-based)
                if num_lines > 5:
                    json_lines = reader.read_lines_json(1, 4)
                    assert isinstance(json_lines, list)
                    for obj in json_lines:
                        # Should be JSON objects, not dicts
                        assert hasattr(obj, '__getitem__')  # Dictionary-like access
                        assert "name" in obj
                
                # Test read_line_bytes_json - operates on byte ranges  
                if max_bytes > 1024:
                    json_bytes = reader.read_line_bytes_json(0, 1024)
                    assert isinstance(json_bytes, list)
                    for obj in json_bytes:
                        # Should be JSON objects, not dicts
                        assert hasattr(obj, '__getitem__')  # Dictionary-like access
                        assert "name" in obj
    
    def test_reader_context_manager(self):
        """Test reader as context manager"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            
            # Test with statement
            with dft_utils.Reader(gz_file) as reader:
                max_bytes = reader.get_max_bytes()
                assert max_bytes > 0
    
    def test_reader_reset(self):
        """Test reader reset functionality"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)

            with dft_utils.Reader(gz_file) as reader:
                max_bytes = reader.get_max_bytes()
                assert max_bytes > 0
                
                # Reset should work without error
                reader.reset()
                assert reader.get_max_bytes() == max_bytes
    
    def test_reader_comprehensive_json_parsing(self):
        """Test comprehensive JSON parsing functionality"""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file_with_nested_json()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)

            with dft_utils.Reader(gz_file) as reader:
                num_lines = reader.get_num_lines()

                if num_lines > 2:
                    # Test JSON line reading (1-based)
                    json_objects = reader.read_lines_json(1, 4)
                    assert isinstance(json_objects, list)

                    for json_obj in json_objects:
                        # Should be JSON objects, not dicts
                        assert hasattr(json_obj, '__getitem__')  # Dictionary-like access
                        # Test nested structure access
                        assert "id" in json_obj
                        assert "metadata" in json_obj
                        if "events" in json_obj:
                            events = json_obj["events"]
                            assert type(events).__name__ == "JSON"

    def test_json_dict_methods(self):
        """Test JSON object dict-like methods"""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file_with_nested_json()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)

            with dft_utils.Reader(gz_file) as reader:
                num_lines = reader.get_num_lines()

                if num_lines > 0:
                    json_objects = reader.read_lines_json(1, 2)
                    assert len(json_objects) > 0

                    json_obj = json_objects[0]

                    # Test keys()
                    keys = json_obj.keys()
                    assert isinstance(keys, list)
                    assert len(keys) > 0
                    assert all(isinstance(k, str) for k in keys)

                    # Test values()
                    values = json_obj.values()
                    assert isinstance(values, list)
                    assert len(values) == len(keys)

                    # Test items()
                    items = json_obj.items()
                    assert isinstance(items, list)
                    assert len(items) == len(keys)
                    for item in items:
                        assert isinstance(item, tuple)
                        assert len(item) == 2
                        assert isinstance(item[0], str)  # key is string

                    # Test __len__()
                    assert len(json_obj) == len(keys)
                    assert len(json_obj) > 0

                    # Test __bool__()
                    assert bool(json_obj) is True  # Non-empty object is truthy
                    if json_obj:  # Should work in if statements
                        pass  # This should execute
                    else:
                        assert False, "Non-empty JSON object should be truthy"

                    # Test that nested objects in values/items are lazy JSON objects
                    if "metadata" in json_obj:
                        metadata = json_obj["metadata"]
                        assert type(metadata).__name__ == "JSON"

                        # Test dict methods on nested object
                        assert len(metadata) > 0
                        assert bool(metadata) is True  # Non-empty nested object is truthy
                        nested_keys = metadata.keys()
                        assert isinstance(nested_keys, list)

                        nested_values = metadata.values()
                        assert isinstance(nested_values, list)

                        nested_items = metadata.items()
                        assert isinstance(nested_items, list)

    def test_json_unwrap_and_copy(self):
        """Test JSON unwrap() and copy() methods"""
        with Environment(lines=50) as env:
            gz_file = env.create_test_gzip_file_with_nested_json()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)

            with dft_utils.Reader(gz_file) as reader:
                num_lines = reader.get_num_lines()

                if num_lines > 0:
                    json_objects = reader.read_lines_json(1, 2)
                    assert len(json_objects) > 0

                    json_obj = json_objects[0]

                    # Test unwrap() - should return native Python dict
                    unwrapped = json_obj.unwrap()
                    assert isinstance(unwrapped, dict)
                    assert "id" in unwrapped
                    assert "metadata" in unwrapped

                    # Verify nested objects are also unwrapped (not lazy JSON)
                    if "metadata" in unwrapped:
                        assert isinstance(unwrapped["metadata"], dict)
                        assert type(unwrapped["metadata"]).__name__ == "dict"

                    # Verify arrays are unwrapped to lists
                    if "events" in unwrapped:
                        assert isinstance(unwrapped["events"], list)

                    # Test copy() - should return a new JSON object
                    json_copy = json_obj.copy()
                    assert type(json_copy).__name__ == "JSON"
                    assert json_copy is not json_obj  # Different object

                    # Copy should have same data
                    assert len(json_copy) == len(json_obj)
                    assert list(json_copy.keys()) == list(json_obj.keys())

                    # Test copy on nested objects
                    if "metadata" in json_obj:
                        metadata = json_obj["metadata"]
                        metadata_copy = metadata.copy()
                        assert type(metadata_copy).__name__ == "JSON"
                        assert metadata_copy is not metadata
                        assert len(metadata_copy) == len(metadata)

    def test_reader_edge_cases(self):
        """Test reader edge cases"""
        with Environment() as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file)
            
            with dft_utils.Reader(gz_file) as reader:
                max_bytes = reader.get_max_bytes()
                num_lines = reader.get_num_lines()
                
                # Test reading beyond file size - should raise RuntimeError
                if max_bytes > 0:
                    with pytest.raises(RuntimeError):
                        reader.read(max_bytes, max_bytes + 100)
                
                # Test reading beyond line count - should raise RuntimeError
                if num_lines > 0:
                    with pytest.raises(RuntimeError):
                        reader.read_lines(num_lines + 1, num_lines + 5)
    
    def test_bytes_vs_line_read_functionality(self):
        """Test different read methods functionality"""
        with Environment(lines=100) as env:
            gz_file = env.create_test_gzip_file()
            env.build_index(gz_file, checkpoint_size_bytes=512*1024)

            with dft_utils.Reader(gz_file) as reader:
                max_bytes = reader.get_max_bytes()
                num_lines = reader.get_num_lines()
                
                if max_bytes > 100 and num_lines > 5:
                    # Test raw bytes read method
                    raw_data = reader.read(0, 100)
                    assert isinstance(raw_data, bytes)
                    assert len(raw_data) == 100
                    
                    # Test line-based read
                    line_data = reader.read_line_bytes(0, 100)
                    assert isinstance(line_data, list)
                    
                    # Test line number read (1-based)
                    lines = reader.read_lines(1, 4)
                    assert isinstance(lines, list)
                    assert all(isinstance(line, str) for line in lines)


if __name__ == "__main__":
    pytest.main([__file__])
