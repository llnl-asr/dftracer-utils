import csv
import importlib.util
import os
import stat

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "scripts", "bench", "scale_ladder.py")

spec = importlib.util.spec_from_file_location("scale_ladder", SCRIPT)
ladder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ladder)


def make_env(tmp_path):
    build = tmp_path / "build"
    build.mkdir()
    tool = build / "dftracer_info"
    tool.write_text("#!/bin/sh\nexit 0\n")
    tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
    data = tmp_path / "data"
    data.mkdir()
    for i in range(5):
        (data / ("t%d.pfw" % i)).write_text("[\n{}\n")
    return build, data


def run(tmp_path, monkeypatch, extra):
    build, data = make_env(tmp_path)
    events = []
    real_read = ladder.read_file
    real_run = ladder.run_measured

    def read_file(path):
        events.append(("read", os.path.basename(path)))
        return real_read(path)

    def run_measured(*a, **k):
        events.append(("run", None))
        return real_run(*a, **k)

    monkeypatch.setattr(ladder, "read_file", read_file)
    monkeypatch.setattr(ladder, "run_measured", run_measured)
    out = tmp_path / "out.tsv"
    argv = [
        "--build",
        str(build),
        "--set",
        "s=%s" % data,
        "--scratch",
        str(tmp_path / "scratch"),
        "--out",
        str(out),
        "--tools",
        "info",
        "--threads",
        "1,2",
        "--commit",
        "x",
    ] + extra
    assert ladder.main(argv) == 0
    with open(str(out), newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    return events, rows


def test_warm_reads_each_file_once_before_timed_runs(tmp_path, monkeypatch):
    events, rows = run(tmp_path, monkeypatch, [])
    reads = [n for kind, n in events if kind == "read"]
    assert sorted(reads) == ["t%d.pfw" % i for i in range(5)]
    kinds = [k for k, _ in events]
    assert kinds.count("run") == 2
    assert kinds.index("run") == len(reads)
    assert "warm read 5 files" in rows[0]["note"]
    assert "warm" not in rows[1]["note"]


def test_no_warm_skips_the_pass(tmp_path, monkeypatch):
    events, rows = run(tmp_path, monkeypatch, ["--no-warm"])
    assert [k for k, _ in events] == ["run", "run"]
    assert all("warm" not in r["note"] for r in rows)
