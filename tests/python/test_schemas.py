"""Record schemas: registration, classes, loading, detection and explain."""

import gzip
import json
import os
import subprocess
import sys
import textwrap
from typing import Optional

import pyarrow as pa
import pytest

import dftracer.utils as dftu
from dftracer.utils import schemas
from dftracer.utils.schemas import Json, RecordSchema, field

RECORDS = 3000
MEMBER_RECORDS = 100


def _nginx(path):
    # Many small gzip members, so `upstream` runs of 300 records land in
    # separate chunks.
    with open(path, "wb") as out:
        for start in range(0, RECORDS, MEMBER_RECORDS):
            lines = "".join(
                json.dumps(
                    {
                        "remote_addr": f"10.0.0.{i % 7}",
                        "status": 404 if i % 11 == 0 else 200,
                        "request_time": (i % 13) + 0.25,
                        "upstream": f"u{i // 300}",
                    }
                )
                + "\n"
                for i in range(start, start + MEMBER_RECORDS)
            )
            out.write(gzip.compress(lines.encode()))
    return path


def _run(code, env_extra):
    env = dict(os.environ, **env_extra)
    out = subprocess.run(
        [sys.executable, "-c", textwrap.dedent(code)],
        env=env,
        capture_output=True,
        text=True,
    )
    return out.returncode, out.stdout + out.stderr


def test_register_list_and_conflicts():
    sid = schemas.register(
        {
            "id": "py_nginx",
            "fields": {"status": {"type": "int"}, "request_time": {"type": "float"}},
        },
        source="first.json",
    )
    assert sid == "py_nginx"
    ids = [s["id"] for s in schemas.list()]
    assert ids[:2] == ["dftracer", "generic"]
    got = next(s for s in schemas.list() if s["id"] == "py_nginx")
    assert got["decoder"] == "path"
    assert got["require"] == ["status", "request_time"]
    assert [f["type"] for f in got["fields"]] == ["int", "float"]
    assert got["path_budget"] is None

    # The same definition again is fine; another one is not.
    schemas.register(
        {"id": "py_nginx", "fields": {"status": {"type": "int"}, "request_time": {"type": "float"}}}
    )
    with pytest.raises(dftu.DFTUtilsValueError, match="first.json"):
        schemas.register(
            {"id": "py_nginx", "fields": {"status": {"type": "string"}}}, source="second.json"
        )
    with pytest.raises(dftu.DFTUtilsValueError, match="detect"):
        schemas.register("id: py_bad\ndetect: {require: [x]}\n")


def test_a_class_registers_the_same_schema_as_a_spec():
    class PyClassNginx(RecordSchema, id="py_class_nginx"):
        status: int
        request_time: float = field(role="duration", unit="s")
        host: Optional[str] = field(path="meta.host")
        upstream: str = field(always_index=True)
        service: str

    assert PyClassNginx.id == "py_class_nginx"
    got = next(s for s in schemas.list() if s["id"] == "py_class_nginx")
    assert got["require"] == ["status", "request_time", "upstream", "service"]
    by_name = {f["name"]: f for f in got["fields"]}
    assert by_name["host"]["path"] == "meta.host" and by_name["host"]["optional"]
    assert by_name["request_time"]["role"] == "duration"
    assert by_name["request_time"]["unit"] == "s"
    assert by_name["upstream"]["always_index"]
    # The equivalent YAML spec is the same definition.
    schemas.register(
        "id: py_class_nginx\n"
        "fields:\n"
        "  upstream: {type: string, always_index: true}\n"
        "  host: {type: string, path: meta.host, optional: true}\n"
        "  request_time: {type: float, role: duration, unit: s}\n"
        "  status: {type: int}\n"
        "  service: {type: string}\n"
    )


def test_a_json_field_reads_canonical_text(tmp_path):
    class PyTagged(RecordSchema, id="py_tagged"):
        kind: str
        tags: Optional[Json]

    path = tmp_path / "tagged.ndjson.gz"
    lines = [
        '{"kind":"x","tags":[ "a" , "b" ]}',
        '{"kind":"x","tags":{"b":1,"a":2.0}}',
        '{"kind":"x"}',
    ]
    path.write_bytes(gzip.compress(("\n".join(lines) + "\n").encode()))
    tv = dftu.TraceViewer(str(path), record_schema=PyTagged)
    tags = pa.table(tv.select("tags").collect()).column("tags").to_pylist()
    assert tags == ['["a","b"]', '{"a":2,"b":1}', None]
    assert pa.table(tv.query('tags == \'{"a": 2, "b": 1}\'').collect()).num_rows == 1
    got = next(s for s in schemas.list() if s["id"] == "py_tagged")
    assert [f["type"] for f in got["fields"]] == ["string", "json"]
    with pytest.raises(dftu.DFTUtilsValueError, match="json"):

        class PyDftJson(schemas.DFTracer, id="py_dft_json"):
            blob: Json = field(path="args.blob")


def test_schema_tree_nests_paths(tmp_path):
    class PyTree(RecordSchema, id="py_tree"):
        op: str
        off: int = field(path="io.off")
        host: Optional[str] = field(path="meta.host")

    path = tmp_path / "tree.ndjson.gz"
    lines = ['{"op":"read","io":{"off":4096}}', '{"op":"write","io":{"off":0}}']
    path.write_bytes(gzip.compress(("\n".join(lines) + "\n").encode()))
    with dftu.Indexer(files=[str(path)], schema=PyTree) as ix:
        ix.ensure_indexed()
    tree = dftu.TraceViewer(str(path)).schema_tree()
    assert tree["op"] == {"type": "string", "count": 2, "field": "op", "declared_type": "string"}
    assert tree["io"]["children"]["off"]["count"] == 2
    assert tree["io"]["children"]["off"]["declared_type"] == "int"
    assert tree["meta"]["children"]["host"] == {
        "count": 0,
        "field": "host",
        "declared_type": "string",
    }


def test_a_class_extends_by_inheritance():
    class PyMyTrace(schemas.DFTracer, id="py_my_dft"):
        step: int = field(path="args.step")

    class PyChild(PyMyTrace, id="py_my_dft_child"):
        epoch: Optional[int] = field(path="args.epoch")

    got = {s["id"]: s for s in schemas.list()}
    assert got["py_my_dft"]["decoder"] == "dftracer"
    assert got["py_my_dft"]["require"] == ["ph", "name", "args.step"]
    assert got["py_my_dft_child"]["require"] == ["ph", "name", "args.step"]
    assert PyChild.id == "py_my_dft_child"


def test_bad_classes_raise_at_definition():
    with pytest.raises(dftu.DFTUtilsValueError, match="tags"):

        class BadType(RecordSchema, id="py_bad_type"):
            tags: list

    with pytest.raises(dftu.DFTUtilsValueError, match="needs an id"):

        class NoId(RecordSchema):
            x: int

    with pytest.raises(dftu.DFTUtilsValueError, match="unit"):

        class BadUnit(RecordSchema, id="py_bad_unit"):
            x: int = field(unit="ms")

    with pytest.raises(dftu.DFTUtilsValueError, match="PyConflict"):

        class PyConflict(RecordSchema, id="py_nginx"):
            other: str

    assert "py_bad_type" not in {s["id"] for s in schemas.list()}


def test_load_a_directory_detect_and_explain(tmp_path):
    specs = tmp_path / "specs"
    specs.mkdir()
    (specs / "nginx.yaml").write_text(
        "id: py_dir_nginx\n"
        "fields:\n"
        "  remote_addr: {type: string}\n"
        "  status: {type: int}\n"
        "  request_time: {type: float}\n"
    )
    loaded = [s["id"] for s in schemas.load(specs)]
    assert "py_dir_nginx" in loaded
    trace = _nginx(str(tmp_path / "access.ndjson.gz"))
    assert schemas.detect(trace) == "py_dir_nginx"
    why = schemas.explain(trace)
    assert why["chosen"] == "py_dir_nginx"
    score = next(s for s in why["scores"] if s["id"] == "py_dir_nginx")
    assert score["required"] == 3 and score["share"] == 1.0


def test_schemas_next_to_the_index_are_used(tmp_path):
    trace = _nginx(str(tmp_path / "access.ndjson.gz"))
    index_dir = tmp_path / "idx"
    (index_dir / "schemas").mkdir(parents=True)
    (index_dir / "schemas" / "nginx.yaml").write_text(
        "id: py_idx_nginx\n"
        "fields:\n"
        "  remote_addr: {type: string}\n"
        "  status: {type: int}\n"
        "  request_time: {type: float}\n"
        "  upstream: {type: string, always_index: true}\n"
        "index: {path_budget: 0}\n"
    )
    with dftu.Indexer(files=[trace], index_dir=str(index_dir)) as ix:
        ix.ensure_indexed()
        explained = ix.explain('upstream == "u2"')[0]
    assert len(explained["read"]) < explained["chunks"]
    viewer = dftu.TraceViewer(trace, index_path=str(index_dir / ".dftindex"))
    rows = pa.table(viewer.query('upstream == "u2"').collect())
    assert rows.num_rows == 300


def test_trace_viewer_record_schema_override(tmp_path):
    trace = _nginx(str(tmp_path / "access.ndjson.gz"))
    t = pa.table(dftu.TraceViewer(trace, record_schema=schemas.Generic).collect())
    assert {"status", "upstream"} <= set(t.column_names)
    with pytest.raises(dftu.DFTUtilsValueError, match="no_such_schema"):
        dftu.TraceViewer(trace, record_schema="no_such_schema")


def test_schema_path_environment_and_spec_edits(tmp_path):
    trace = _nginx(str(tmp_path / "access.ndjson.gz"))
    index_dir = str(tmp_path / "idx")
    spec_dir = tmp_path / "env_specs"
    spec_dir.mkdir()
    spec = spec_dir / "nginx.yaml"
    spec.write_text("id: env_nginx\nfields: {remote_addr: {type: string}, status: {type: int}}\n")
    env = {"DFTRACER_SCHEMA_PATH": str(spec_dir)}
    build = f"""
        import dftracer.utils as d
        with d.Indexer(files=[{trace!r}], index_dir={index_dir!r}) as ix:
            print("needs", len(ix.resolve().needs_work))
            ix.ensure_indexed()
        print("ids", [s["id"] for s in d.schemas.list()])
    """
    code, out = _run(build, env)
    assert code == 0, out
    assert "needs 1" in out and "env_nginx" in out

    # Built again with the same spec: nothing to do.
    code, out = _run(build, env)
    assert code == 0, out
    assert "needs 0" in out

    # An edited spec rebuilds the file.
    spec.write_text(
        "id: env_nginx\n"
        "fields: {remote_addr: {type: string}, status: {type: int},\n"
        "  upstream: {type: string, always_index: true}}\n"
    )
    code, out = _run(build, env)
    assert code == 0, out
    assert "needs 1" in out

    # Read without the schema registered: an error that says where to load it.
    read = f"""
        import dftracer.utils as d
        d.TraceViewer({trace!r}, index_path={index_dir + "/.dftindex"!r}).collect()
    """
    code, out = _run(read, {"DFTRACER_SCHEMA_PATH": ""})
    assert code != 0
    assert "env_nginx" in out and "DFTRACER_SCHEMA_PATH" in out


def test_old_locations_are_not_read(tmp_path):
    trace = _nginx(str(tmp_path / "access.ndjson.gz"))
    old = tmp_path / "old"
    old.mkdir()
    (old / "nginx.yaml").write_text(
        "id: old_nginx\nfields: {remote_addr: {type: string}, status: {type: int}}\n"
    )
    index_dir = tmp_path / "idx"
    (index_dir / "profiles").mkdir(parents=True)
    (index_dir / "profiles" / "nginx.yaml").write_text((old / "nginx.yaml").read_text())
    code, out = _run(
        f"""
        import dftracer.utils as d
        with d.Indexer(files=[{trace!r}], index_dir={str(index_dir)!r}) as ix:
            ix.ensure_indexed()
        print("ids", [s["id"] for s in d.schemas.list()])
        print("detected", d.schemas.detect({trace!r}))
        """,
        {"DFTRACER_PROFILE_PATH": str(old), "DFTRACER_SCHEMA_PATH": ""},
    )
    assert code == 0, out
    assert "old_nginx" not in out
    assert "detected generic" in out
