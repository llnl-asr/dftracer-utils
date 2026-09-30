"""A corrupt index is named, and a repair that does not cure it says so.

The copies of an index that cannot be repaired come from a developer machine, so
the two tests that need them are gated on environment variables:

  DFTRACER_TEST_CORRUPT_INDEX  a `.dftindex` directory that fails every build
  DFTRACER_TEST_RELEASED_PYTHON  a Python with the released dftracer-utils 0.0.13
"""

import gzip
import json
import os
import shutil
import subprocess
import textwrap

import pytest

from dftracer.utils import AggregationConfig, Indexer

ADVICE = "delete that directory and build the index again"


def write_trace(directory, name="t.pfw.gz", n=50):
    path = os.path.join(directory, name)
    with gzip.open(path, "wt") as f:
        for i in range(n):
            f.write(
                json.dumps(
                    {
                        "name": "read",
                        "cat": "POSIX",
                        "pid": 1,
                        "tid": 1,
                        "ph": "X",
                        "ts": 1000 + i,
                        "dur": 5,
                        "args": {"fhash": "f", "hhash": "h", "ret": 10},
                    }
                )
                + "\n"
            )
    return path


def build(files, index_dir):
    with Indexer(
        files=files,
        index_dir=index_dir,
        require_aggregation=AggregationConfig(time_interval_ms=5000),
    ) as ix:
        ix.ensure_indexed()


@pytest.mark.skipif(
    "DFTRACER_TEST_CORRUPT_INDEX" not in os.environ, reason="needs a corrupt .dftindex copy"
)
def test_a_repair_that_does_not_cure_the_index_names_the_directory(tmp_path):
    index = str(tmp_path / ".dftindex")
    shutil.copytree(os.environ["DFTRACER_TEST_CORRUPT_INDEX"], index)
    trace = write_trace(str(tmp_path))
    with pytest.raises(Exception) as caught:
        build([trace], index)
    message = str(caught.value)
    assert index in message
    assert ADVICE in message
    assert "did not repair it" in message


@pytest.mark.skipif(
    "DFTRACER_TEST_CORRUPT_INDEX" not in os.environ, reason="needs a corrupt .dftindex copy"
)
def test_deleting_the_directory_as_told_fixes_it(tmp_path):
    index = str(tmp_path / ".dftindex")
    shutil.copytree(os.environ["DFTRACER_TEST_CORRUPT_INDEX"], index)
    trace = write_trace(str(tmp_path))
    with pytest.raises(Exception):
        build([trace], index)
    shutil.rmtree(index)
    build([trace], index)  # no error


@pytest.mark.skipif(
    "DFTRACER_TEST_RELEASED_PYTHON" not in os.environ,
    reason="needs a Python with dftracer-utils 0.0.13",
)
def test_an_index_built_by_the_released_version_is_reused_or_rebuilt(tmp_path):
    trace = write_trace(str(tmp_path))
    index = str(tmp_path / ".dftindex")
    script = textwrap.dedent(
        f"""
        from dftracer.utils import Indexer, AggregationConfig
        with Indexer(files=[{trace!r}], index_dir={index!r},
                     require_aggregation=AggregationConfig(time_interval_ms=5000)) as ix:
            ix.ensure_indexed()
        """
    )
    subprocess.run([os.environ["DFTRACER_TEST_RELEASED_PYTHON"], "-c", script], check=True)
    build([trace], index)  # opens the older index: no error, whether it is reused or rebuilt
    from dftracer.utils import TraceViewer

    counted = (
        TraceViewer([trace], index_path=index).group_by("name").agg("count").collect().to_pandas()
    )
    assert int(counted["count"].sum()) == 50  # every event once, never twice


def test_a_fresh_index_builds(tmp_path):
    trace = write_trace(str(tmp_path))
    build([trace], str(tmp_path / ".dftindex"))
