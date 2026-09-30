"""The duql builder builds the tree of the equal text and runs like it."""

import gzip
import json

import pytest

import dftracer.utils as dftu
from dftracer.utils.columnar import F
from dftracer.utils.dftracer_utils_ext import duql_canonical
from dftracer.utils.duql import (
    Pipe,
    Source,
    c,
    case_,
    duration,
    fn,
    lit,
    param,
    rowset,
    source,
    sub,
    tup,
)

CASES = [
    (
        source("t.pfw.gz")
        .where((c("name") == "read") & (c("dur") > 1000))
        .group("pid", n=fn.count())
        .sort(-c("n"))
        .take(10),
        'from "t.pfw.gz" | where name == "read" and dur > 1000 | group pid { n = count() } '
        "| sort -n | take 10",
    ),
    (
        Pipe().where(
            (c("a") == None)  # noqa: E711
            | (c("b") == True)  # noqa: E712
            | (c("c") == -5)
            | (c("d") == 1.5)
            | (c("e") == 'x"y')
        ),
        "where a == null or b == true or c == -5 or d == 1.5 or e == 'x\"y'",
    ),
    (
        Pipe().where(
            c("ts").between(duration(1, "s"), duration(5, "s"))
            & (c("dur") > duration(250, "ms"))
            & (c("name") == param("n"))
        ),
        "where ts between 1s and 5s and dur > 250ms and name == $n",
    ),
    (
        Pipe().where(
            (lit(5) - 3 == 2)
            & (-c("x") < duration(10, "us"))
            & ((c("a") // 2) % 3 == lit(1.5) * 2 / 4)
            & (c("a").coalesce(c("b")) + 1 > duration(2.5, "ns"))
            & (1 + c("y") > 2)
        ),
        "where 5 - 3 == 2 and -x < 10us and a // 2 % 3 == 1.5 * 2 / 4 and (a ?? b) + 1 > 2.5ns "
        "and 1 + y > 2",
    ),
    (
        Pipe().where(
            c("a").iregex("x")
            & (c("a").not_regex("^x") | c("b").regex("y"))
            & c("c").not_iregex(c("p"))
        ),
        'where a ~* "x" and (a !~ "^x" or b ~ "y") and c !~* p',
    ),
    (
        Pipe().where(c("name").is_in(param("names")) & c("path").not_ilike(param("q"), escape="!")),
        'where name in $names and path not ilike $q escape "!"',
    ),
    (
        Pipe().where(c("x").is_in([1, 2, 3]) & c("y").not_in(["a", "b"])),
        'where x in [1, 2, 3] and y not in ["a", "b"]',
    ),
    (
        Pipe().where(c("cat").icontains("io") & c("tags").not_icontains("x", any=True)),
        'where "io" in cat and "x" not in any(tags)',
    ),
    (
        Pipe().where(
            c("name").like("%Send%")
            & c("name").not_ilike("%a_b%", escape="\\")
            & c("n").ilike("x")
            & c("n").not_like("y")
        ),
        'where name like "%Send%" and name not ilike "%a_b%" escape "\\\\" and n ilike "x" '
        'and n not like "y"',
    ),
    (
        Pipe().where(
            c("x").is_null()
            | c("y").is_not_missing()
            | c("z").is_not_null()
            | c("w").is_missing()
            | ~(c("v") == 1)
        ),
        "where x is null or y is not missing or z is not null or w is missing or not v == 1",
    ),
    (
        Pipe().where(c("ts").not_between(1, 2) & (fn.any(c("tags")) == "y")),
        'where ts not between 1 and 2 and any(tags) == "y"',
    ),
    (
        Pipe().where(
            (c("run").ref("runs", "app") == "laghos")
            & (c("k").ref("files", "path", key="fhash") == "a")
        ),
        'where run -> runs.app == "laghos" and k -> files(fhash).path == "a"',
    ),
    (
        Pipe().derive(e=fn.myplug.entropy(c("x"), bins=8), q=c("dur").quantile(0.5), l=lit([1, 2])),
        "derive e = myplug.entropy(x, bins = 8), q = quantile(dur, 0.5), l = [1, 2]",
    ),
    (
        Pipe().where(tup(c("a"), c("b")).is_in([tup(1, 2), tup(3, 4)])),
        "where (a, b) in [(1, 2), (3, 4)]",
    ),
    (
        Pipe()
        .where(c("fd").is_in(rowset("data").where(c("name") == "open").select("fd")))
        .derive(n=sub(rowset("data").where(c("run") == c("^.run")).agg(c=fn.count()))),
        'where fd in (from data | where name == "open" | select fd) '
        "| derive n = (from data | where run == ^.run | agg { c = count() })",
    ),
    (
        Pipe().where(c("fd").not_in(rowset("data").select("fd"))),
        "where fd not in (from data | select fd)",
    ),
    (
        Pipe()
        .select("name", (c("dur") * 2).alias("d"), "ts")
        .drop("args.x", "cat")
        .rename(n="name", p="args.path"),
        "select name, d = dur * 2, ts | drop args.x, cat | rename n = name, p = args.path",
    ),
    (Pipe().select("name", d=c("dur") * 2), "select name, d = dur * 2"),
    (Pipe().distinct().distinct(c("pid"), "tid"), "distinct | distinct pid, tid"),
    (
        Pipe().agg(fn.count(), c("dur").sum(), q=fn.quantile(c("dur"), 0.9)),
        "agg { count(), sum(dur), q = quantile(dur, 0.9) }",
    ),
    (
        Pipe().derive(
            s=case_([(c("dur") > 500, "slow")], "fast"),
            t=case_([(c("ok"), 1), (c("bad"), 2)]),
        ),
        'derive s = case { dur > 500 => "slow", else => "fast" }, t = case { ok => 1, bad => 2 }',
    ),
    (
        Pipe().group(n=fn.count()).agg(n=fn.count(), s=c("dur").sum()),
        "group { n = count() } | agg { n = count(), s = sum(dur) }",
    ),
    (
        Pipe().window(
            "pid",
            sort=["ts"],
            a=c("dur").sum().over(rows=3),
            b=c("dur").mean().over(duration(1, "s")),
            d=c("dur").max().over(param("w")),
        ),
        "window pid sort ts { a = sum(dur) over 3 rows, b = mean(dur) over 1s, d = max(dur) over $w }",
    ),
    (
        Pipe()
        .window("pid", "name", sort=["ts"], n=fn.row_number(), gap=c("ts") - c("ts").lag())
        .window(sort=["ts"], r=fn.rank()),
        "window pid, name sort ts { n = row_number(), gap = ts - lag(ts) } | window sort ts { r = rank() }",
    ),
    (
        Pipe().pivot(c("name"), ["a", "b"], s=c("dur").sum()).pivot("name", s=c("dur").sum()),
        'pivot name in ["a", "b"] { s = sum(dur) } | pivot name { s = sum(dur) }',
    ),
    (Pipe().unpivot("a", "b", key="k", value="v"), "unpivot a, b as k, v"),
    (
        Pipe()
        .distinct(c("cat").alias("c"), (c("dur") // 10).alias("d"))
        .pivot(c("k"), ["a", "b"], labels=["x", ""], n=fn.count())
        .take_range(3, param("b")),
        'distinct c = cat, d = dur // 10 | pivot k in ["a" as x, "b"] { n = count() } | take 3..$b',
    ),
    (
        Pipe().sort(c("dur").desc(nulls="last"), c("name").asc(nulls="first")),
        "sort -dur nulls last, name nulls first",
    ),
    (
        Pipe().take(3, by=["pid"], sort=[-c("dur")]).take(param("k")),
        "take 3 by pid sort -dur | take $k",
    ),
    (
        Pipe().skip(5).sample(10, percent=True, seed=7).sample(100),
        "skip 5 | sample 10% seed 7 | sample 100",
    ),
    (
        Pipe().expand("tags", as_="t", with_index="i", keep_empty=True).expand("xs"),
        "expand tags as t with_index i keep_empty | expand xs",
    ),
    (
        Pipe()
        .parse("name", r"(?<op>\w+)")
        .parse(c("args.fname"), 'a"b\\d')
        .parse("p", param("re"))
        .derive(r=fn("regex_replace")(c("name"), "/+", "/")),
        r'parse name ~ "(?<op>\\w+)" | parse args.fname ~ "a\"b\\d" | parse p ~ $re | derive r = regex_replace(name, "/+", "/")',
    ),
    (
        Pipe().lookup("files", ["fhash"], into="f").lookup("files", [("fhash", "fh"), "pid"]),
        "lookup files on fhash into f | lookup files on fhash == fh, pid",
    ),
    (
        Pipe()
        .lookup_asof("samples", ["pid"], ("ts", "t"), "nearest", within=duration(5, "ms"))
        .lookup_asof("samples", ["pid"], "ts", "forward", within=7.5),
        "lookup samples on pid asof ts == t nearest within 5ms | lookup samples on pid asof ts forward within 7.5",
    ),
    (
        Pipe()
        .lookup("files", ["fhash"], how="inner")
        .lookup("files", ["fhash"], how="anti")
        .lookup("files", ["fhash"], into="f", how="inner"),
        "lookup files on fhash inner | lookup files on fhash anti | lookup files on fhash inner into f",
    ),
    (
        Pipe().lookup(source("f.pfw.gz").where(c("x") == 1), ["pid"], into="m", how="inner"),
        'lookup (from "f.pfw.gz" | where x == 1) on pid inner into m',
    ),
    (
        Pipe().lookup(rowset("files").take(3), ["pid"], how="anti"),
        "lookup (from files | take 3) on pid anti",
    ),
    (Pipe().lookup_asof(rowset("s"), ["pid"], "ts"), "lookup (from s) on pid asof ts"),
    (Pipe().lookup_overlap("spans", ["pid"], into="m"), "lookup spans on pid overlap into m"),
    (Pipe().union(source("b.pfw.gz").where(c("x") == 1)), 'union (from "b.pfw.gz" | where x == 1)'),
    (Pipe().call(fn.myplug.table(c("x"), n=3)), "call myplug.table(x, n = 3)"),
    (
        Pipe()
        .time_range(duration(1, "s"), duration(5, "s"), overlap=True)
        .call_tree()
        .bucket(duration(1, "s"), fill=True)
        .bucket(duration(2, "s")),
        "time_range 1s .. 5s overlap | call_tree | bucket 1s fill | bucket 2s",
    ),
    (
        Pipe()
        .bucket(duration(1, "ms"), fill=True, mode="forward")
        .bucket(duration(1, "ms"), fill=True, as_="b", mode="linear", low=0, high=duration(6, "ms"))
        .bucket(
            duration(1, "ms"), fill=True, low=param("lo"), high=param("hi") + duration(1, "ms")
        ),
        "bucket 1ms fill forward | bucket 1ms fill linear from 0 to 6ms as b "
        "| bucket 1ms fill from $lo to $hi + 1ms",
    ),
    (
        Pipe()
        .bucket(duration(5, "s"), every=duration(1, "s"))
        .bucket(duration(5, "s"), every=duration(1, "s"), at=duration(500, "ms"))
        .bucket(
            duration(5, "s"),
            fill=True,
            as_="b",
            mode="forward",
            low=0,
            high=duration(6, "s"),
            every=duration(1, "s"),
            at=duration(500, "ms"),
        )
        .bucket(duration(5, "s"), at=duration(500, "ms"))
        .bucket(param("w"), every=param("e"), at=param("a")),
        "bucket 5s every 1s | bucket 5s every 1s at 500ms | bucket 5s every "
        "1s at 500ms fill forward from 0 to 6s as b | bucket 5s at 500ms | "
        "bucket $w every $e at $a",
    ),
    (
        Pipe()
        .time_range(param("t0"), param("t0") + duration(10, "s"))
        .time_range(duration(1, "s"))
        .time_range(high=duration(5, "s"), overlap=True)
        .bucket(duration(1, "s"), fill=True, as_="sec")
        .sample(param("n"), seed=param("s"))
        .union("runs"),
        "time_range $t0 .. $t0 + 10s | time_range 1s .. | time_range .. 5s overlap "
        "| bucket 1s fill as sec | sample $n seed $s | union runs",
    ),
    (
        Pipe()
        .session("pid", gap=duration(5, "ms"), max=duration(1, "s"), as_="s")
        .session(gap=duration(1, "ms")),
        "session pid gap 5ms max 1s as s | session gap 1ms",
    ),
    (
        rowset("big")
        .let("big", rowset("data").where(c("dur") > 100))
        .define("slow", c("x") > 5, params=["x"])
        .where(fn.slow(c("dur"))),
        "let big = from data | where dur > 100;\ndef slow(x) = x > 5;\nfrom big | where slow(dur)",
    ),
    (
        rowset("data")
        .define(
            "io_rate",
            Pipe().where(c("cat") == "POSIX").bucket(c("d")).agg(b=fn.sum(c("size"))),
            params=["d"],
        )
        .define("pos", Pipe().where(c("size") > 0))
        .use("io_rate", duration(1, "ms"))
        .use("pos")
        .sort("b"),
        'def io_rate(d) = where cat == "POSIX" | bucket d | agg { b = sum(size) };\ndef pos = where size > 0;\nfrom data | io_rate(1ms) | pos() | sort b',
    ),
    (source(param("f")).where(c("x") == 1), "from $f | where x == 1"),
    (source("a.pfw.gz", "b.pfw.gz"), 'from "a.pfw.gz", "b.pfw.gz"'),
    (rowset("all").where(F("cat") == "POSIX"), 'from all | where cat == "POSIX"'),
]


@pytest.mark.parametrize("built,text", CASES, ids=[t for _, t in CASES])
def test_builder_equals_text_tree(built, text):
    assert built.text() == duql_canonical(text)
    assert built.text().startswith("duql 1\n")


ARRAY_CASES = [
    (
        Pipe().where(
            (c("xs")[c("i")] == c("xs")[2])
            & (c("xs")[-1] > 0)
            & (c("xs")[param("n")] > 0)
            & (c("a.b")[c("i") - 1] > 0)
        ),
        "where xs[i] == xs[2] and xs[-1] > 0 and xs[$n] > 0 and a.b[i - 1] > 0",
    ),
    (
        Pipe().derive(l=lit([1, "a", [2]]), s=fn.sort(c("xs"))),
        'derive l = [1, "a", [2]], s = sort(xs)',
    ),
]


@pytest.mark.parametrize("built,text", ARRAY_CASES)
def test_array_builders_equal_text_tree(built, text):
    assert built.text() == duql_canonical(text)


def test_an_index_follows_a_field_path():
    for bad in (fn.sort(c("xs")), c("a") + 1, lit(5)):
        with pytest.raises(TypeError, match="field path"):
            bad[0]


def test_string_values_read_back_exactly():
    for value in ['a"b', "a\"b'c", "ends\\", "tab\tnew\nline", "\x01", "\u00e9"]:
        assert duql_canonical(f"where x == {lit(value).raw()}") == duql_canonical(
            "where x == " + json.dumps(value)
        ), value
    assert lit('a"b\\').raw() == '"a\\"b\\\\"'
    with pytest.raises(ValueError):
        lit(float("nan"))
    with pytest.raises(ValueError, match="unit"):
        duration(1, "weeks")


def test_expressions_have_no_truth_value():
    with pytest.raises(TypeError, match="&"):
        bool(c("a") == 1)


def test_text_reports_a_parse_error():
    with pytest.raises(dftu.DFTUtilsValueError):
        Pipe().where(c("a..b") == 1).text()


def _trace(tmp_path):
    events = [
        {"ph": "X", "name": n, "cat": "POSIX", "pid": p, "tid": 1, "ts": 10 * i, "dur": d}
        for i, (n, p, d) in enumerate(
            [
                ("read", 1, 5),
                ("write", 1, 50),
                ("read", 2, 500),
                ('a"b', 2, 7),
                ("read", 1, 900),
                ("q\"s'b\\d", 3, 11),
            ]
        )
    ]
    path = tmp_path / "t.pfw.gz"
    path.write_bytes(gzip.compress(("\n".join(json.dumps(e) for e in events) + "\n").encode()))
    return str(path)


def test_builder_query_runs_as_its_text(tmp_path):
    path = _trace(tmp_path)
    q = (
        source(path)
        .where((c("name") == param("n")) & (c("dur") > duration(4, "us")))
        .group("pid", total=c("dur").sum(), n=fn.count())
        .sort("pid")
        .bind(n="read")
    )
    want = dftu.TraceViewer(path).duql(
        f'from "{path}" | where name == $n and dur > 4us | group pid {{ total = sum(dur), n = count() }} | sort pid',
        n="read",
    )
    assert (
        q.collect().to_dict()
        == want.collect().to_dict()
        == {"pid": [1, 2], "total": [905, 500], "n": [2, 1]}
    )
    assert "$n" in q.text()
    assert q.count() == 2
    assert q.first() == {"pid": 1, "total": 905, "n": 2}
    assert source(path).where(c("dur") > 10_000).first() is None
    assert sum(len(b.to_dict()["pid"]) for b in q.stream()) == 2


def test_bucket_fill_range_runs_as_its_text(tmp_path):
    path = _trace(tmp_path)
    q = (
        source(path)
        .bucket(duration(10, "us"), fill=True, low=0, high=duration(80, "us"))
        .agg(n=fn.count())
    )
    want = dftu.TraceViewer(path).duql(
        f'from "{path}" | bucket 10us fill from 0 to 80us | agg {{ n = count() }}'
    )
    assert q.collect().to_dict() == want.collect().to_dict()
    assert q.collect().to_dict() == {
        "bucket": [0, 10, 20, 30, 40, 50, 60, 70, 80],
        "n": [1, 1, 1, 1, 1, 1, 0, 0, 0],
    }


def test_bucket_hopping_runs_as_its_text(tmp_path):
    path = _trace(tmp_path)
    q = source(path).bucket(duration(20, "us"), every=duration(10, "us")).agg(n=fn.count())
    want = dftu.TraceViewer(path).duql(
        f'from "{path}" | bucket 20us every 10us | agg {{ n = count() }}'
    )
    assert q.collect().to_dict() == want.collect().to_dict()


def test_bucket_rejects_a_range_without_fill_and_a_bad_mode():
    with pytest.raises(ValueError, match="needs fill"):
        Pipe().bucket(duration(1, "s"), low=0, high=1)
    with pytest.raises(ValueError, match="needs fill"):
        Pipe().bucket(duration(1, "s"), mode="forward")
    with pytest.raises(ValueError, match="mode"):
        Pipe().bucket(duration(1, "s"), fill=True, mode="cubic")
    with pytest.raises(ValueError, match="both"):
        Pipe().bucket(duration(1, "s"), fill=True, low=0)


def test_over_rows_frame_runs_as_its_text(tmp_path):
    path = _trace(tmp_path)
    q = source(path).window("pid", sort=["ts"], s=c("dur").sum().over(rows=2)).sort("pid", "ts")
    want = dftu.TraceViewer(path).duql(
        f'from "{path}" | window pid sort ts {{ s = sum(dur) over 2 rows }} | sort pid, ts'
    )
    assert q.collect().to_dict() == want.collect().to_dict()
    assert q.collect().to_dict()["s"] == [5, 55, 950, 500, 507, 11]


def test_over_needs_exactly_one_of_width_and_rows():
    with pytest.raises(ValueError, match="exactly one"):
        c("x").sum().over()
    with pytest.raises(ValueError, match="exactly one"):
        c("x").sum().over(1, rows=2)


def test_unnamed_aggregates_and_case_run_as_their_text(tmp_path):
    path = _trace(tmp_path)
    q = (
        source(path)
        .derive(s=case_([(c("dur") > 100, "slow")], "fast"))
        .group("s", n=fn.count())
        .sort("s")
    )
    want = dftu.TraceViewer(path).duql(
        f'from "{path}" | derive s = case {{ dur > 100 => "slow", else => "fast" }} | group s {{ n = count() }} | sort s'
    )
    assert q.collect().to_dict() == want.collect().to_dict() == {"s": ["fast", "slow"], "n": [4, 2]}
    assert source(path).agg(fn.count(), c("dur").sum()).first() == {"count": 6, "sum_dur": 1473}
    assert "group" in q.explain()


def test_on_runs_a_sourceless_query_on_a_viewer(tmp_path):
    path = _trace(tmp_path)
    q = Pipe().where(c("name") == 'a"b').select("dur")
    assert q.on(dftu.TraceViewer(path)).collect().to_dict() == {"dur": [7]}
    with pytest.raises(ValueError, match="on"):
        q.collect()
    bound = source(param("f")).where(c("dur") > 100).bind(f=path)
    assert bound.count() == 2


def test_filter_builder_quotes_a_value_with_a_quote(tmp_path):
    path = _trace(tmp_path)
    tv = dftu.TraceViewer(path)
    assert tv.filter(F("name") == 'a"b').collect().to_dict()["dur"] == [7]
    tricky = "q\"s'b\\d"
    assert tv.filter(F("name") == tricky).collect().to_dict()["dur"] == [11]
    assert Pipe().where(c("name") == tricky).select("dur").on(tv).collect().to_dict() == {
        "dur": [11]
    }


def test_list_and_pattern_parameters(tmp_path):
    path = _trace(tmp_path)
    tv = dftu.TraceViewer(path)
    want = tv.duql('where name in ["read", "write"] | select dur').collect().to_dict()
    assert (
        tv.duql("where name in $names | select dur", names=["read", "write"]).collect().to_dict()
        == want
    )
    assert (
        tv.duql("where name in $names | select dur", names=("read", "write")).collect().to_dict()
        == want
    )
    q = Pipe().where(c("name").is_in(param("names"))).select("dur").bind(names=["read", "write"])
    assert q.on(tv).collect().to_dict() == want
    assert "in $names" in q.text()
    assert (
        Pipe()
        .where(c("name").like(param("p")))
        .select("dur")
        .bind(p="wr%")
        .on(tv)
        .collect()
        .to_dict()
        == tv.duql('where name like "wr%" | select dur').collect().to_dict()
    )
    with pytest.raises(dftu.DFTUtilsError):
        tv.duql("where name == $names", names=["read"]).collect()
    with pytest.raises(dftu.DFTUtilsError):
        tv.duql("where name in $one", one="read").collect()


def test_lookup_kinds_run_as_their_text(tmp_path):
    path = _trace(tmp_path)
    side = rowset("data").where(c("name") == "write").group("pid", hit=fn.count())
    for how in ("inner", "anti"):
        q = source(path).lookup(side, ["pid"], how=how).sort("ts")
        want = dftu.TraceViewer(path).duql(
            f'from "{path}" | lookup (from data | where name == "write" | group pid {{ hit = count() }}) on pid {how} | sort ts'
        )
        got = q.collect().to_dict()
        assert got == want.collect().to_dict()
    inner = source(path).lookup(side, ["pid"], how="inner").collect().to_dict()
    anti = source(path).lookup(side, ["pid"], how="anti").collect().to_dict()
    assert set(inner["pid"]) == {1} and 1 not in anti["pid"] and anti["pid"]


def test_lookup_how_errors():
    with pytest.raises(ValueError, match="how"):
        Pipe().lookup("files", ["k"], how="outer")
    with pytest.raises(ValueError, match="anti.*into"):
        Pipe().lookup("files", ["k"], into="f", how="anti")


def test_source_members_in_order():
    s = Source().rowset("data", Pipe().where(c("ph") != "M")).flag("args_fallback", True)
    assert s.text() == 'data = where ph != "M";\ndef args_fallback = true'
    assert Source().flag("x", False).text() == "def x = false"


def test_source_define_and_compile():
    s = (
        Source()
        .define("slow", c("dur") > param("t"), ["t"])
        .define("fast", Pipe().where(c("dur") < param("t")), ["t"])
        .rowset("files", Pipe().where(c("name") == "FH").select(fhash=c("args.value")).distinct())
    )
    assert s.text() == (
        'files = where name == "FH" | select fhash = args.value | distinct;\n'
        "def slow(t) = dur > $t;\ndef fast(t) = where dur < $t"
    )


def test_source_refuses_a_row_set_that_reads_a_file():
    with pytest.raises(ValueError):
        Source().rowset("x", rowset("runs"))
    with pytest.raises(ValueError):
        Source().rowset("x", source("a.pfw"))
