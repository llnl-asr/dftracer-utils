# SPDX-License-Identifier: MIT
import sys

from dftracer.utils import jit
from dftracer.utils.plugins import Plugins


@jit.plugin
class NameEdges:
    edges = jit.map(key=(jit.i64, jit.str_), value=jit.count())

    @jit.each_event
    def step(self, e):
        if e.fhash != jit.NONE:  # only file-referencing events
            self.edges[(e.pid, e.name)] += 1


@jit.plugin
class CatStats:
    n = jit.map(key=(jit.str_,), value=jit.count())
    total = jit.map(key=(jit.str_,), value=jit.sum())
    avg = jit.map(key=(jit.str_,), value=jit.mean())
    slow = jit.map(key=(jit.str_,), value=jit.max())
    worst = jit.map(key=(jit.str_,), value=jit.argmax(of=jit.str_))
    hot = jit.map(key=(jit.str_,), value=jit.approx_topk(16))

    @jit.each_event
    def step(self, e):
        self.n[(e.cat,)] += 1  # additive aggregates use +=
        self.total[(e.cat,)] += e.dur  # (an arithmetic expr is fine)
        self.avg[(e.cat,)].observe(e.dur)  # others observe a value
        self.slow[(e.cat,)].observe(e.dur)
        self.worst[(e.cat,)].observe(e.name, by=e.dur)  # name at the max dur
        self.hot[(e.cat,)].observe(e.name)  # frequent event names


@jit.plugin
class Lat:
    lat = jit.map(key=(jit.str_,), value=jit.quantiles(qs=(0.5, 0.9, 0.99)))

    @jit.each_event
    def step(self, e):
        self.lat[(e.cat,)].observe(e.dur)


@jit.plugin
class DurHist:
    hist = jit.map(key=(jit.i64,), value=jit.count())

    @jit.each_event
    def step(self, e):
        self.hist[(jit.ilog2(e.dur),)] += 1  # 0..63 log-scale bucket


if __name__ == "__main__":
    trace = sys.argv[1]

    plugins = Plugins([NameEdges])  # compiles + caches the .so, then loads
    run = plugins.run(trace)
    df = run.results["edges"].to_pandas()  # columns: pid, name, value
    assert df.empty  # the trace has no file-referencing events

    run = Plugins([CatStats, Lat, DurHist]).run(trace)
    tbl = run.results["n"]  # DataFrame
    values = tbl.to_arrow().column("value").to_numpy(zero_copy_only=False)
    df = tbl.to_pandas()  # pandas.DataFrame

    # from here it is ordinary NumPy / pandas
    top = df.sort_values("value", ascending=False).head(10)
    total = int(values.sum())
    assert total == 300 and len(top) == 1
    assert {"p50", "p90", "p99"} <= set(run.results["lat"].to_pandas().columns)
    assert int(run.results["hist"].to_pandas()["value"].sum()) == 300
