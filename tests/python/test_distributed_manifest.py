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

from dftracer.utils import AggregationConfig, Indexer
from dftracer.utils.dask import distributed_index

from .common import Environment

AGG_CFG = AggregationConfig(time_interval_ms=5000)


def _build_distributed(env, pids, num_events=100, rebuild_root=True):
    files = [env.create_dft_trace_file_with_pid(f"trace_p{p}.pfw.gz", p, num_events) for p in pids]
    index_dir = os.path.join(env.temp_dir, "idx")
    os.makedirs(index_dir, exist_ok=True)
    index_path = os.path.join(index_dir, ".dftindex")
    staging = os.path.join(env.temp_dir, "stage")
    os.makedirs(staging, exist_ok=True)
    result = distributed_index(
        files=files,
        index_path=index_path,
        local_staging=staging,
        shared_staging=staging,
        client=None,
        aggregation_config=AGG_CFG,
        rebuild_root_summaries=rebuild_root,
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

    def test_aggregation_matches_serial(self):
        """Distributed build's aggregation data must equal a serial build."""
        with Environment(lines=200) as env:
            files_dist, dist_index_path, _ = _build_distributed(env, pids=[1, 2])

            uni_index_dir = os.path.join(env.temp_dir, "idx_uni")
            os.makedirs(uni_index_dir, exist_ok=True)
            uni_indexer = Indexer(
                files=files_dist,
                index_dir=uni_index_dir,
                require_aggregation=AGG_CFG,
                force_rebuild=True,
            )
            uni_indexer.ensure_indexed()
            uni_batches = uni_indexer.iter_arrow_dfanalyzer_all(
                time_granularity=5.0,
                time_resolution=1_000_000.0,
            )

            dist_indexer = Indexer(
                files=files_dist,
                index_dir=os.path.dirname(dist_index_path),
                require_aggregation=AGG_CFG,
                force_rebuild=False,
            )
            dist_batches = dist_indexer.iter_arrow_dfanalyzer_all(
                time_granularity=5.0,
                time_resolution=1_000_000.0,
            )

            import pyarrow as pa

            def _total_count(batches_dict, key):
                batches = [pa.record_batch(b) for b in batches_dict.get(key, [])]
                if not batches:
                    return 0
                table = pa.Table.from_batches(batches)
                if "count" in table.column_names:
                    return int(pa.compute.sum(table["count"]).as_py() or 0)
                return table.num_rows

            uni_count = _total_count(uni_batches, "events")
            dist_count = _total_count(dist_batches, "events")
            assert uni_count == dist_count, (
                f"event count mismatch: unified={uni_count} distributed={dist_count}"
            )
            assert uni_count > 0

    def test_move_artifacts_preserves_per_file_agg_ssts(self):
        """With cross-FS staging, per-file `aggregation.sst` must not collapse."""
        with Environment(lines=120) as env:
            files = [
                env.create_dft_trace_file_with_pid(f"trace_p{p}.pfw.gz", p, 120)
                for p in [1, 2, 3, 4]
            ]
            local_staging = os.path.join(env.temp_dir, "local_stage")
            shared_staging = os.path.join(env.temp_dir, "lustre_stage")
            os.makedirs(local_staging, exist_ok=True)
            os.makedirs(shared_staging, exist_ok=True)
            index_dir = os.path.join(env.temp_dir, "idx")
            os.makedirs(index_dir, exist_ok=True)
            index_path = os.path.join(index_dir, ".dftindex")

            distributed_index(
                files=files,
                index_path=index_path,
                local_staging=local_staging,
                shared_staging=shared_staging,
                client=None,
                aggregation_config=AGG_CFG,
            )

            indexer = Indexer(
                files=files,
                index_dir=os.path.dirname(index_path),
                require_aggregation=AGG_CFG,
                force_rebuild=False,
            )
            batches = indexer.iter_arrow_dfanalyzer_all(
                time_granularity=5.0,
                time_resolution=1_000_000.0,
            )
            import pyarrow as pa

            event_batches = [pa.record_batch(b) for b in batches.get("events", [])]
            assert event_batches, "no events: per-file SSTs likely clobbered each other"
            table = pa.Table.from_batches(event_batches)
            total = int(pa.compute.sum(table["count"]).as_py() or 0)
            assert total > 0

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
                require_manifest=True,
                require_aggregation=AGG_CFG,
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
                staging = os.path.join(env.temp_dir, "stage")
                os.makedirs(staging, exist_ok=True)
                result = distributed_index(
                    files=files,
                    index_path=index_path,
                    local_staging=staging,
                    shared_staging=staging,
                    client=client,
                    aggregation_config=AGG_CFG,
                )
                assert result["total_files"] == len(files)
                assert result["artifact_batches"] > 0
                assert not os.path.exists(os.path.join(index_path, "agg_manifest.json"))
