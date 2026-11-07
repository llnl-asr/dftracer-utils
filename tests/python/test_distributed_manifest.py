# SPDX-License-Identifier: MIT
"""Tests for the distributed-build path producing a unified-shape index.

Covers per-file AGG markers, ensure_indexed no-op after build, and end-to-end
correctness vs a serial single-node build.
"""

import os

import pytest

try:
    import dask  # noqa: F401

    DASK_AVAILABLE = True
except ImportError:
    DASK_AVAILABLE = False

from dftracer.utils import AggregationConfig, Indexer, TraceViewer
from dftracer.utils.dask import distributed_index

from .common import Environment

AGG_CFG = AggregationConfig(time_interval_ms=5000)


def _agg_event_count(files, index_dir):
    """Total aggregated event count read from the index via the View tier."""
    import pyarrow as pa

    tbl = TraceViewer(files, index_path=index_dir).group_by("name").agg("count").collect()
    return int(pa.compute.sum(pa.table(tbl)["count"]).as_py() or 0)


def _build_distributed(env, pids, num_events=100):
    files = [env.create_dft_trace_file_with_pid(f"trace_p{p}.pfw.gz", p, num_events) for p in pids]
    index_dir = os.path.join(env.temp_dir, "idx")
    os.makedirs(index_dir, exist_ok=True)
    index_path = os.path.join(index_dir, ".dftindex")
    result = distributed_index(
        files=files,
        index_path=index_path,
        client=None,
        aggregation_config=AGG_CFG,
    )
    return files, index_path, result


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDistributedIndexUnified:
    def test_no_manifest_left_behind(self):
        with Environment(lines=50) as env:
            _, index_path, _ = _build_distributed(env, pids=[1, 2])
            manifest_path = os.path.join(index_path, "agg_manifest.json")
            assert not os.path.exists(manifest_path), (
                "distributed_index should produce a unified-shape index"
            )

    @pytest.mark.valgrind
    def test_aggregation_matches_serial(self):
        """Distributed build's aggregation data must equal a serial build."""
        with Environment(lines=200) as env:
            files_dist, dist_index_path, _ = _build_distributed(env, pids=[1, 2])

            uni_index_dir = os.path.join(env.temp_dir, "idx_uni")
            os.makedirs(uni_index_dir, exist_ok=True)
            uni_indexer = Indexer(
                files=files_dist,
                index_dir=uni_index_dir,
                aggregation_config=AGG_CFG,
                force_rebuild=True,
            )
            uni_indexer.ensure_indexed()

            uni_count = _agg_event_count(files_dist, uni_index_dir)
            dist_count = _agg_event_count(files_dist, os.path.dirname(dist_index_path))
            assert uni_count == dist_count, (
                f"event count mismatch: unified={uni_count} distributed={dist_count}"
            )
            assert uni_count > 0

    def test_ensure_indexed_is_noop_after_distributed_build(self):
        import time as _time

        with Environment(lines=150) as env:
            files, index_path, _ = _build_distributed(env, pids=[1, 2, 3])

            t0 = _time.monotonic()
            indexer = Indexer(
                files=files,
                index_dir=os.path.dirname(index_path),
                require_checkpoint=True,
                require_bloom=True,
                aggregation_config=AGG_CFG,
                force_rebuild=False,
            )
            status = indexer.ensure_indexed()
            elapsed = _time.monotonic() - t0

            assert status.total_files == len(files)
            assert len(status.ready) == len(files), (
                f"post-distributed ensure_indexed wants to rebuild "
                f"{len(status.needs_work)} files (markers missing?)"
            )
            assert len(status.needs_work) == 0
            assert elapsed < 5.0, (
                f"ensure_indexed took {elapsed:.2f}s on a {len(files)}-file "
                "distributed index; likely re-running the build"
            )


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDistributedWithDask:
    def test_multi_worker_with_local_cluster(self):
        from dask.distributed import Client, LocalCluster

        with LocalCluster(
            n_workers=2, threads_per_worker=1, dashboard_address=None, processes=True
        ) as cluster, Client(cluster) as client:
            with Environment(lines=40) as env:
                pids = [1, 2, 3, 4]
                files = [
                    env.create_dft_trace_file_with_pid(f"trace_p{p}.pfw.gz", p, 40) for p in pids
                ]
                index_dir = os.path.join(env.temp_dir, "idx")
                os.makedirs(index_dir, exist_ok=True)
                index_path = os.path.join(index_dir, ".dftindex")
                result = distributed_index(
                    files=files,
                    index_path=index_path,
                    client=client,
                    aggregation_config=AGG_CFG,
                )
                assert result["total_files"] == len(files)
                assert not os.path.exists(os.path.join(index_path, "agg_manifest.json"))

                # The coordinator process reads back what the worker processes
                # wrote, and it matches a serial build.
                status = Indexer(
                    files=files,
                    index_dir=index_dir,
                    require_checkpoint=True,
                    require_bloom=True,
                    aggregation_config=AGG_CFG,
                    force_rebuild=False,
                ).ensure_indexed()
                assert len(status.ready) == len(files)
                assert len(status.needs_work) == 0

                serial_dir = os.path.join(env.temp_dir, "idx_serial")
                os.makedirs(serial_dir, exist_ok=True)
                Indexer(
                    files=files,
                    index_dir=serial_dir,
                    aggregation_config=AGG_CFG,
                    force_rebuild=True,
                ).ensure_indexed()
                serial_count = _agg_event_count(files, serial_dir)
                assert serial_count > 0
                assert _agg_event_count(files, index_dir) == serial_count


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDistributedWholeFiles:
    def test_a_multi_member_file_keeps_every_event(self):
        """A file larger than its share of the work is built whole by one
        worker, so no member of it is left out of the index."""
        from dask.distributed import Client, LocalCluster

        with LocalCluster(
            n_workers=2, threads_per_worker=1, dashboard_address=None, processes=True
        ) as cluster, Client(cluster) as client:
            with Environment(lines=40) as env:
                parts = [
                    env.create_dft_trace_file_with_pid(f"part{i}.pfw.gz", 1, 400) for i in range(4)
                ]
                big = os.path.join(env.temp_dir, "big.pfw.gz")
                with open(big, "wb") as out:
                    for part in parts:
                        with open(part, "rb") as member:
                            out.write(member.read())
                for part in parts:
                    os.remove(part)
                small = env.create_dft_trace_file_with_pid("small.pfw.gz", 2, 5)
                files = [big, small]

                index_dir = os.path.join(env.temp_dir, "idx")
                os.makedirs(index_dir, exist_ok=True)
                result = distributed_index(
                    files=files,
                    index_path=os.path.join(index_dir, ".dftindex"),
                    client=client,
                    aggregation_config=AGG_CFG,
                )
                assert sorted(result["per_worker"]) == [1, 1]

                serial_dir = os.path.join(env.temp_dir, "idx_serial")
                os.makedirs(serial_dir, exist_ok=True)
                Indexer(
                    files=files,
                    index_dir=serial_dir,
                    aggregation_config=AGG_CFG,
                    force_rebuild=True,
                ).ensure_indexed()
                serial_count = _agg_event_count(files, serial_dir)
                assert serial_count > 1600
                assert _agg_event_count(files, index_dir) == serial_count


@pytest.mark.skipif(not DASK_AVAILABLE, reason="Dask not available")
class TestDistributedIndexIdempotent:
    """distributed_index skips the parse fan-out for already-indexed files."""

    def _run(self, files, index_path, **kw):
        return distributed_index(
            files=files,
            index_path=index_path,
            client=None,
            aggregation_config=AGG_CFG,
            **kw,
        )

    def _setup(self, env, pids):
        files = [env.create_dft_trace_file_with_pid(f"t{p}.pfw.gz", p, 100) for p in pids]
        index_path = os.path.join(env.temp_dir, ".dftindex")
        return files, index_path

    def test_warm_call_skips_reparse(self):
        with Environment(lines=50) as env:
            files, index_path = self._setup(env, [1, 2])
            cold = self._run(files, index_path)
            assert sum(cold["per_worker"]) == 2
            warm = self._run(files, index_path)
            assert warm["per_worker"] == []
            assert warm["total_files"] == 2

    def test_partial_builds_only_new_file(self):
        with Environment(lines=50) as env:
            files, index_path = self._setup(env, [1, 2])
            self._run(files, index_path)
            files.append(env.create_dft_trace_file_with_pid("t3.pfw.gz", 3, 100))
            part = self._run(files, index_path)
            assert part["total_files"] == 3
            assert sum(part["per_worker"]) == 1

    def test_force_rebuild_reparses_all(self):
        with Environment(lines=50) as env:
            files, index_path = self._setup(env, [1, 2])
            self._run(files, index_path)
            forced = self._run(files, index_path, force_rebuild=True)
            assert sum(forced["per_worker"]) == 2


class TestStoreSeesOtherProcesses:
    def test_long_lived_process_sees_another_process_write(self):
        import subprocess
        import sys
        import textwrap
        import time

        with Environment(lines=50) as env:
            first = env.create_dft_trace_file_with_pid("a.pfw.gz", 1, 100)
            second = env.create_dft_trace_file_with_pid("b.pfw.gz", 2, 100)
            index_dir = os.path.join(env.temp_dir, "idx")
            os.makedirs(index_dir, exist_ok=True)

            def resolve(files):
                return Indexer(
                    files=files,
                    index_dir=index_dir,
                    require_checkpoint=True,
                    require_bloom=True,
                    force_rebuild=False,
                ).resolve()

            Indexer(
                files=[first], index_dir=index_dir, require_checkpoint=True, require_bloom=True
            ).ensure_indexed()
            # This process now holds the index, and sees the second file as new.
            assert second in resolve([first, second]).needs_work

            child = textwrap.dedent(
                f"""
                from dftracer.utils import Indexer
                Indexer(files=[{second!r}], index_dir={index_dir!r},
                        require_checkpoint=True, require_bloom=True).ensure_indexed()
                """
            )
            subprocess.run([sys.executable, "-c", child], check=True)

            # Past the check interval the held index sees the other process's
            # file without a refresh() call.
            time.sleep(1.2)
            assert resolve([first, second]).needs_work == []
