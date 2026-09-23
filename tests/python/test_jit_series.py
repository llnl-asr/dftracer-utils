"""@jit.series: author a reusable column op as an Expr; it joins the ops
registry and runs via the engine's evaluator (no compile step)."""

import numpy as np
import pytest

from dftracer.utils import jit
from dftracer.utils.jit import ops
from dftracer.utils.series import Series

MOD = __name__


def _col(vals):
    return Series.from_numpy(np.array(vals, dtype=np.int64))


def _vals(series):
    return np.asarray(series).tolist()


def _ns(root):
    """Walk `ops` (or a Series' `.ops`) down the dotted module of this test."""
    for part in MOD.split("."):
        root = getattr(root, part)
    return root


def test_series_op_is_callable_and_registered():
    @jit.series
    def doubled(dur):
        return dur * 2

    s = _col([1, 2, 3])
    assert _vals(doubled(s)) == [2, 4, 6]  # the decorated name is callable
    assert _vals(_ns(ops).doubled(s)) == [2, 4, 6]  # and reachable via ops.<module>.<name>
    assert _vals(ops.run(f"{MOD}.doubled", s)) == [2, 4, 6]  # and by qualified name
    assert f"{MOD}.doubled" in ops.list()
    info = ops.info(f"{MOD}.doubled")
    assert info["kind"] == "series" and info["arity"] == 1


def test_series_op_never_takes_a_bare_name():
    @jit.series
    def unprefixed(a):
        return a + 1

    assert "unprefixed" not in ops.list()  # the bare namespace stays reserved
    assert f"{MOD}.unprefixed" in ops.list()
    with pytest.raises(AttributeError):
        ops.unprefixed


def test_series_op_refuses_the_host_namespace():
    with pytest.raises(jit.JitError):

        @jit.series(module="dftu.fs")
        def shadow(a):
            return a + 1

    with pytest.raises(jit.JitError):

        @jit.series(module="dftu")
        def shadow2(a):
            return a + 1


def test_series_op_two_args_fuses_expr():
    @jit.series
    def weighted(a, b):
        return a * 2 + b

    assert _vals(ops.run(f"{MOD}.weighted", _col([1, 2, 3]), _col([10, 20, 30]))) == [12, 24, 36]


def test_series_op_arity_mismatch_raises():
    @jit.series
    def triple(a):
        return a * 3

    with pytest.raises(TypeError):
        ops.run(f"{MOD}.triple", _col([1]), _col([2]))


def test_ops_attribute_path_elides_the_host_prefix():
    @jit.series(module="mymod")
    def zscore(a):
        return a * 2

    s = _col([1, 2, 3])
    assert _vals(ops.series.add(s, _col([10, 20, 30]))) == [11, 22, 33]  # dftu. elided
    with pytest.raises(AttributeError):
        ops.dftu  # the un-elided spelling does not also work
    assert _vals(ops.mymod.zscore(s)) == [2, 4, 6]  # a user op keeps its own literal path


def test_series_op_module_grouping():
    @jit.series(module="stats")
    def scaled(dur):
        return dur * 10

    s = _col([1, 2, 3])
    assert _vals(ops.run("stats.scaled", s)) == [10, 20, 30]  # by dotted name
    assert _vals(ops.stats.scaled(s)) == [10, 20, 30]  # ops.<module>.<name>
    assert _vals(s.ops.stats.scaled()) == [10, 20, 30]  # s.ops.<module>.<name>
    assert "stats.scaled" in ops.list()


def test_series_op_dotted_module_nests():
    @jit.series(module="pkg.stats")
    def zscore(dur):
        return dur * 100

    s = _col([1, 2])
    assert _vals(ops.run("pkg.stats.zscore", s)) == [100, 200]
    assert _vals(ops.pkg.stats.zscore(s)) == [100, 200]
    assert _vals(s.ops.pkg.stats.zscore()) == [100, 200]


def test_series_op_rejects_non_expr_body():
    with pytest.raises(jit.JitError):

        @jit.series
        def bad(a):
            return 5  # not a column expression


def test_series_ops_accessor_on_a_series():
    @jit.series
    def tripled(a):
        return a * 3

    s = _col([1, 2, 3])
    assert _vals(_ns(s.ops).tripled()) == [3, 6, 9]  # user op as a method
    assert _vals(s.ops.series.add(_col([10, 20, 30]))) == [
        11,
        22,
        33,
    ]  # built-in as a method, dftu. elided
    assert s.ops.series.count() == 3  # reducer returns a scalar


def test_series_op_rejects_a_duplicate_registration():
    @jit.series(module="dup")
    def twice(a):
        return a * 2

    assert _vals(ops.run("dup.twice", _col([1, 2]))) == [2, 4]
    with pytest.raises(ValueError):

        @jit.series(module="dup")
        def twice(a):  # noqa: F811 - same name, already registered
            return a * 3
