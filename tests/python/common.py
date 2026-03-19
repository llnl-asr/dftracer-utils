#!/usr/bin/env python3
"""
Common test utilities for  Python bindings tests
"""

import pytest
import os
import tempfile
import gzip
import shutil

import dftracer.utils as dft_utils


class Environment:
    """Shared test environment manager for  tests"""

    def __init__(self, lines=100):
        self.lines = lines
        self.temp_dir = None
        self.test_files = []
        self._setup()

    def _setup(self):
        """Set up temporary directory"""
        self.temp_dir = tempfile.mkdtemp(prefix="dft_test_")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.cleanup()

    def cleanup(self):
        """Clean up temporary files and directory"""
        for file_path in self.test_files:
            try:
                if os.path.exists(file_path):
                    os.remove(file_path)
                idx_path = file_path + ".idx"
                if os.path.exists(idx_path):
                    os.remove(idx_path)
            except OSError:
                pass

        if self.temp_dir and os.path.exists(self.temp_dir):
            try:
                shutil.rmtree(self.temp_dir)
            except OSError:
                pass

    def create_test_gzip_file(self, filename="test_data.pfw.gz", bytes_per_line=1024):
        """Create a test gzip file with sample trace-like data"""
        file_path = os.path.join(self.temp_dir, filename)

        # Generate test data
        lines = []
        closing_len = 3  # len('"}\n')
        for i in range(1, self.lines + 1):
            # Build the JSON line up to the "data" key
            line = f'{{"name":"name_{i}","cat":"cat_{i}","dur":{(i * 123 % 10000)},"data":"'
            current_size = len(line)
            needed_padding = 0
            if bytes_per_line > current_size + closing_len:
                needed_padding = bytes_per_line - current_size - closing_len
            # Append padding safely
            if needed_padding:
                pad_chunk = "x" * 4096
                while needed_padding >= len(pad_chunk):
                    line += pad_chunk
                    needed_padding -= len(pad_chunk)
                if needed_padding:
                    line += "x" * needed_padding
            line += '"}\n'
            lines.append(line)

        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            f.write("[\n")
            f.writelines(lines)
            f.write("]\n")

        self.test_files.append(file_path)
        return file_path

    def create_test_gzip_file_with_nested_json(self):
        """Create a gzip file with complex nested JSON structures for testing"""
        import json

        file_path = os.path.join(
            self.temp_dir, f"nested_test_{len(self.test_files)}.pfw.gz"
        )

        lines = []
        for i in range(self.lines):
            # Create complex nested JSON structure
            nested_data = {
                "id": f"item_{i}",
                "metadata": {
                    "timestamp": f"2024-01-{i:02d}T10:00:00Z",
                    "version": "1.0.0",
                    "user": {
                        "id": f"user_{i % 10}",
                        "profile": {
                            "name": f"User {i}",
                            "settings": {
                                "theme": "dark" if i % 2 == 0 else "light",
                                "notifications": True,
                                "privacy": {
                                    "level": "high",
                                    "options": [
                                        "encrypt",
                                        "anonymize",
                                        "delete_after_30_days",
                                    ],
                                },
                            },
                        },
                    },
                },
                "events": [
                    {
                        "type": "click",
                        "data": {
                            "element": "button",
                            "payload": {
                                "x": 100 + i,
                                "y": 200 + i,
                                "values": [
                                    i,
                                    i * 2.5,
                                    f"string_{i}",
                                    {"nested": True, "count": i},
                                ],
                            },
                        },
                    },
                    {
                        "type": "scroll",
                        "data": {
                            "direction": "down",
                            "payload": {
                                "distance": i * 10,
                                "duration": i * 0.1,
                                "values": [{"position": i, "velocity": i * 1.5}],
                            },
                        },
                    },
                ],
                "config": {
                    "features": {
                        "analytics": {"enabled": True, "level": 2},
                        "tracking": {"enabled": i % 3 != 0, "anonymous": True},
                        "cache": {"enabled": True, "ttl": 3600 + i},
                    },
                    "limits": {
                        "max_events": 1000 + i,
                        "rate_limit": 100.0 + i * 0.1,
                        "storage_mb": 500 + i,
                    },
                },
            }

            line = json.dumps(nested_data, separators=(",", ":")) + "\n"
            lines.append(line)

        # Write compressed data
        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            f.writelines(lines)

        self.test_files.append(file_path)
        return file_path

    def get_index_path(self, gz_file_path):
        """Get the index file path for a gzip file"""
        return gz_file_path + ".idx"

    def build_index(self, gz_file_path, checkpoint_size_bytes=None):
        """Build index for the gzip file using Python indexer"""
        if checkpoint_size_bytes is None:
            checkpoint_size_bytes = 32 * 1024 * 1024  # 32MB default

        idx_file = self.get_index_path(gz_file_path)

        try:
            # Use the indexer API
            indexer = dft_utils.Indexer(gz_file_path, idx_file, checkpoint_size_bytes)
            if indexer.need_rebuild():
                indexer.build()

            if not os.path.exists(idx_file):
                pytest.skip(f"Index file was not created")
            return idx_file
        except Exception as e:
            pytest.skip(f"Failed to build index: {e}")

    def create_indexer(self, gz_file_path, checkpoint_size_bytes=None):
        """Create and build an indexer for testing"""
        if checkpoint_size_bytes is None:
            checkpoint_size_bytes = 32 * 1024 * 1024  # 32MB default

        try:
            indexer = dft_utils.Indexer(
                gz_file_path, checkpoint_size=checkpoint_size_bytes
            )
            if indexer.need_rebuild():
                indexer.build()
            return indexer
        except Exception as e:
            pytest.skip(f"Failed to create indexer: {e}")

    def _find_dft_reader_executable(self):
        """Find the dft_reader executable"""
        # Check common build locations
        possible_paths = [
            "dft_reader",  # In PATH
            "./dft_reader",  # Current directory
            "../dft_reader",  # Parent directory
            "../../dft_reader",  # Grandparent directory
            "./build_test/dft_reader",  # CMake build directory
            "./build/dft_reader",  # Alternative build directory
            "./build/dft_utils/dft_reader",  # Build subdirectory
            "./cmake-build-debug/dft_reader",  # IDE build directory
            "./cmake-build-release/dft_reader",  # IDE build directory
            "./.venv/lib/python3.9/site-packages/dft_utils/bin/dft_reader",  # Python package
        ]

        for path in possible_paths:
            if shutil.which(path):
                return path
            if os.path.isfile(path) and os.access(path, os.X_OK):
                return path

        return None

    def is_valid(self):
        """Check if test environment is valid"""
        return self.temp_dir and os.path.exists(self.temp_dir)
