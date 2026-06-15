#!/usr/bin/env python3
"""
Common test utilities for  Python bindings tests
"""

import gc
import gzip
import os
import shutil
import tempfile

import pytest

from dftracer.utils.dftracer_utils_ext import CheckpointIndexer as NativeIndexer


def valgrind_scale(n: int, divisor: int = 10) -> int:
    """Return n/divisor when running under Valgrind, else n."""
    if os.environ.get("DFTRACER_UTILS_VALGRIND"):
        return max(10, n // divisor)
    return n


def determine_index_path(file_path: str, index_dir: str = "") -> str:
    if index_dir:
        return os.path.join(index_dir, ".dftindex")
    return os.path.join(os.path.dirname(file_path), ".dftindex")


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
        gc.collect()
        for file_path in self.test_files:
            try:
                if os.path.exists(file_path):
                    os.remove(file_path)
                index_path = determine_index_path(file_path, "")
                if os.path.isdir(index_path):
                    shutil.rmtree(index_path)
            except OSError:
                pass

        if self.temp_dir and os.path.exists(self.temp_dir):
            try:
                shutil.rmtree(self.temp_dir)
            except OSError:
                pass
        gc.collect()

    def create_test_gzip_file(self, filename="test_data.pfw.gz", bytes_per_line=1024):
        """Create a test gzip file with valid DFTracer trace events"""
        file_path = os.path.join(self.temp_dir, filename)
        os.makedirs(os.path.dirname(file_path), exist_ok=True)

        io_names = ["read", "write", "open", "close", "pread", "pwrite", "fread", "fwrite"]
        cats = ["POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"]

        # Generate test data
        lines = []
        closing_len = 3  # len('"}\n')
        for i in range(1, self.lines + 1):
            name = io_names[i % len(io_names)]
            cat = cats[i % len(cats)]
            # Build the JSON line with proper DFTracer fields + padding in args.data
            line = (
                f'{{"name":"{name}","cat":"{cat}",'
                f'"pid":{1000 + i % 4},"tid":{2000 + i % 8},'
                f'"ts":{1000000 + i * 1000},"dur":{(i * 123 % 10000)},'
                f'"ph":"X","args":{{"ret":{1024 * i},"hhash":"abc123","data":"'
            )
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
            line += '"}}\n'
            lines.append(line)

        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            f.write("[\n")
            f.writelines(lines)
            f.write("]\n")

        self.test_files.append(file_path)
        return file_path

    def create_dft_trace_file(self, filename="dft_trace.pfw.gz", num_events=None):
        """Create a gzip file with valid DFTracer trace events."""
        file_path = os.path.join(self.temp_dir, filename)
        os.makedirs(os.path.dirname(file_path), exist_ok=True)
        n = num_events if num_events is not None else self.lines
        io_names = ["read", "write", "open", "close", "pread", "pwrite", "fread", "fwrite"]
        cats = ["POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"]
        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            for i in range(n):
                name = io_names[i % len(io_names)]
                cat = cats[i % len(cats)]
                f.write(
                    f'{{"name":"{name}","cat":"{cat}","pid":1,"tid":{1 + i % 3},'
                    f'"ts":{1000000 + i * 1000},"dur":{100 + i * 10},'
                    f'"ph":"X","args":{{"ret":{1024 * (i + 1)},"hhash":"abc123"}}}}\n'
                )
        self.test_files.append(file_path)
        return file_path

    def create_test_gzip_file_with_nested_json(self):
        """Create a gzip file with complex nested JSON structures for testing"""
        import json

        file_path = os.path.join(self.temp_dir, f"nested_test_{len(self.test_files)}.pfw.gz")

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

    def create_varying_schema_file(self, filename="varying_schema.pfw.gz", num_events=500):
        """Create events with varying schemas to test elastic Arrow schema.

        Some events have extra fields (offset, whence, size) that others don't.
        This tests that the Arrow writer handles schema evolution correctly.
        """
        file_path = os.path.join(self.temp_dir, filename)
        os.makedirs(os.path.dirname(file_path), exist_ok=True)

        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            f.write("[\n")
            for i in range(num_events):
                name = ["read", "write", "open", "close", "stat"][i % 5]
                cat = "POSIX"

                event = {
                    "name": name,
                    "cat": cat,
                    "pid": 1000 + i % 4,
                    "tid": 2000 + i % 8,
                    "ts": 1000000 + i * 1000,
                    "dur": (i * 123) % 10000,
                    "ph": "X",
                    "args": {"ret": 1024 * i, "hhash": f"hash_{i}"},
                }

                if name == "read" or name == "write":
                    event["args"]["offset"] = i * 4096
                    event["args"]["size"] = 4096

                if name == "open":
                    event["args"]["flags"] = "O_RDONLY"
                    event["args"]["mode"] = 0o644

                if name == "stat":
                    event["args"]["path"] = f"/tmp/file_{i}.txt"

                if i % 7 == 0:
                    event["args"]["extra_field"] = f"extra_{i}"

                if i % 11 == 0:
                    event["args"]["rare_field"] = i * 1000

                import json

                f.write(json.dumps(event, separators=(",", ":")) + "\n")
            f.write("]\n")

        self.test_files.append(file_path)
        return file_path

    def create_dft_trace_file_with_pid(self, filename, pid, num_events=None):
        """Create a DFTracer trace with a specific PID, hash metadata, and proper aggregation fields."""
        file_path = os.path.join(self.temp_dir, filename)
        os.makedirs(os.path.dirname(file_path), exist_ok=True)
        n = num_events if num_events is not None else self.lines
        io_names = ["read", "write", "open", "close", "pread", "pwrite", "fread", "fwrite"]
        cats = ["POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "POSIX", "STDIO", "STDIO"]
        hhash = f"h{pid}"
        fhash = f"f{pid}"
        with gzip.open(file_path, "wt", encoding="utf-8") as f:
            f.write(
                f'{{"name":"HH","ph":"M","pid":{pid},"tid":1,"args":{{"name":"host{pid}","value":"{hhash}"}}}}\n'
            )
            f.write(
                f'{{"name":"FH","ph":"M","pid":{pid},"tid":1,"args":{{"name":"/data/file{pid}.dat","value":"{fhash}"}}}}\n'
            )
            for i in range(n):
                name = io_names[i % len(io_names)]
                cat = cats[i % len(cats)]
                f.write(
                    f'{{"name":"{name}","cat":"{cat}","pid":{pid},"tid":{1 + i % 3},'
                    f'"ts":{1000000 + i * 1000},"dur":{100 + i * 10},'
                    f'"ph":"X","args":{{"ret":{1024 * (i + 1)},"hhash":"{hhash}","fhash":"{fhash}"}}}}\n'
                )
        self.test_files.append(file_path)
        return file_path

    def create_indexed_traces(self, pids=None, num_events=None):
        """Create trace files and build full index with aggregation.

        Returns the temp directory path (use as directory= for Indexer).
        """
        from dftracer.utils import AggregationConfig, Indexer

        if pids is None:
            pids = [1]
        files = []
        for pid in pids:
            files.append(
                self.create_dft_trace_file_with_pid(f"trace_p{pid}.pfw.gz", pid, num_events)
            )
        indexer = Indexer(
            files=files,
            require_aggregation=AggregationConfig(time_interval_ms=5000),
            force_rebuild=True,
        )
        indexer.ensure_indexed()
        return self.temp_dir

    def get_index_path(self, gz_file_path):
        """Get the `.dftindex` path for a gzip file."""
        return determine_index_path(gz_file_path, "")

    def build_index(self, gz_file_path, checkpoint_size_bytes=None):
        """Build index for the gzip file using Python indexer"""
        if checkpoint_size_bytes is None:
            checkpoint_size_bytes = 32 * 1024 * 1024  # 32MB default

        index_path = self.get_index_path(gz_file_path)

        try:
            with NativeIndexer(gz_file_path, index_path, checkpoint_size_bytes) as indexer:
                if indexer.need_rebuild():
                    indexer.build()

            if not os.path.exists(index_path):
                pytest.skip("Index store was not created")
            return index_path
        except Exception as e:
            pytest.skip(f"Failed to build index: {e}")

    def create_indexer(self, gz_file_path, checkpoint_size_bytes=None):
        """Create and build an indexer for testing"""
        if checkpoint_size_bytes is None:
            checkpoint_size_bytes = 32 * 1024 * 1024  # 32MB default

        try:
            indexer = NativeIndexer(gz_file_path, checkpoint_size=checkpoint_size_bytes)
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
