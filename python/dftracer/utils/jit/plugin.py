# SPDX-License-Identifier: MIT
"""The plugin decorators and the build of a decorated class."""

from __future__ import annotations

import builtins
import inspect
from typing import (
    TYPE_CHECKING,
    Callable,
    Dict,
    List,
    cast,
    get_args,
    get_origin,
)

from .._jit_errors import JitError, Source, class_error
from ..expr import Expr, col
from . import _channels, _flow
from . import ops as _ops
from ._batch import HELPERS, lower_batch
from ._channels import Channel, Latest, Phase, Receiver, Sender
from ._channels import PortDecl as _PortDecl
from ._flow import FlowFn as _FlowFn
from ._flow import TaskSig as _TaskSig
from .compiler import ALL_FIXED_FIELDS, Compiler
from .emit import RAW_ROWS_PER_EVENT, emit_plugin, raw_body, referenced_ops
from .maps import (
    PORT_ID_CHARS,
    RESERVED_NAMESPACE,
    AnyMap,
    EachBatch,
    EachEvent,
    Map,
    PlainField,
    PortHandle,
    StoreField,
    config_fields,
    own_annotations,
    resolve_hint,
    state_fields,
)
from .monoids import Counter, Monoid, Sum

if TYPE_CHECKING:
    from .ops import Result
    from .package import JitPackage


class JitPlugin:
    __slots__ = ("name", "source", "renames", "result_names", "shippable", "lowering")

    def __init__(
        self,
        name: str,
        source: str,
        renames: Dict[str, List[str]],
        result_names: "Dict[str, str] | None" = None,
        shippable: bool = True,
        lowering: str = "branch",
    ) -> None:
        self.name = name
        self.source = source
        self.renames = renames
        # {wire id: attribute name}: a jit.map's provides/results wire id is
        # package-qualified to avoid cross-plugin collisions, but the value a
        # caller indexes results[...] with is always the attribute name, so
        # The extension rewrites native result keys through this map.
        self.result_names = result_names if result_names is not None else {}
        # False when built under an implicit __main__ identity (a script or
        # notebook with no JitPackage namespace); build() refuses those.
        self.shippable = shippable
        # How the per-event body was lowered: "branch" (a C branch per event),
        # "mask" (a guard evaluated over the batch into a selection, then the
        # updates on the selected rows only), "batch" (an @each_batch body of
        # column ops, no per-event loop), "raw" or "vfold".
        self.lowering = lowering


def top_level_package(module: str) -> str:
    """The first dotted segment of an import path (its top-level package)."""
    return module.split(".", 1)[0]


def module_rest(module: str) -> str:
    """``module`` with its top-level package segment stripped."""
    top = top_level_package(module)
    rest = module[len(top) :]
    return rest[1:] if rest.startswith(".") else rest


def check_namespace(fn: str, namespace: str) -> str:
    if not isinstance(namespace, str) or not namespace:
        raise JitError(f"{fn} needs a non-empty namespace such as 'acme'")
    if namespace == RESERVED_NAMESPACE or namespace.startswith(RESERVED_NAMESPACE + "."):
        raise JitError(f"{fn} namespace '{namespace}' uses the reserved dftu namespace")
    if builtins.any(c not in PORT_ID_CHARS for c in namespace):
        raise JitError(f"{fn} namespace '{namespace}' must be lowercase ASCII [a-z0-9._/-]")
    return namespace


def qualname_of(cls: type) -> str:
    qn = getattr(cls, "__qualname__", None) or getattr(cls, "__name__", None)
    if not isinstance(qn, str) or not qn:
        raise JitError("@jit.plugin needs a named class")
    # A class authored inside a function/notebook cell carries "<locals>" in
    # its qualname; drop it rather than emit an identity outside [a-z0-9._/-].
    return qn.replace(".<locals>.", ".")


def module_of(cls: type) -> str:
    mod = getattr(cls, "__module__", None)
    if not isinstance(mod, str) or not mod:
        raise JitError("@jit.plugin needs a class with a __module__")
    return mod


def store_plugin_name(identity: str) -> str:
    """The plugin name a store-using plugin loads under, which is the stem of its
    ``.so`` and the host's namespace prefix: its identity with every byte outside
    ``[a-z0-9-]`` escaped (``_`` as ``__``, ``.`` as ``_d``, ``/`` as ``_s``, any other
    as ``_u<hex>_``), so two identities never share a name."""
    out: List[str] = []
    for c in identity:
        if c in _STORE_NAME_CHARS and c != "_":
            out.append(c)
        elif c == "_":
            out.append("__")
        elif c == ".":
            out.append("_d")
        elif c == "/":
            out.append("_s")
        else:
            out.append(f"_u{ord(c):x}_")
    return "".join(out)


_STORE_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789_-")


_PORT_ANNOTATION = {
    "Sender": ("channel", ("send",)),
    "Receiver": ("channel", ("recv",)),
    "Channel": ("channel", ("both",)),
    "Latest": ("latest", ("own", "ref")),
    "Phase": ("phase", ("own", "ref")),
}
_PORT_ORIGINS = {
    Sender: "Sender",
    Receiver: "Receiver",
    Channel: "Channel",
    Latest: "Latest",
    Phase: "Phase",
}
_PORT_FACTORY = {
    ("channel", "send"): "jit.sender(...)",
    ("channel", "recv"): "jit.receiver(...)",
    ("channel", "both"): "jit.channel(...)",
    ("latest", "own"): "jit.latest()",
    ("latest", "ref"): "jit.latest(...)",
    ("phase", "own"): "jit.phase(...)",
    ("phase", "ref"): "jit.phase(...)",
}


def _port_decls(
    cls: type, pkg: "JitPackage", declared: "Dict[str, _PortDecl]"
) -> "Dict[str, _PortDecl]":
    """Complete the port attributes of ``cls``: the element type from the
    annotation, the host name from the plugin identity, and, for a reference, the
    options and type of the declaration it names (checked against this end)."""
    if not declared:
        return declared
    own = own_annotations(cls)
    plugin_name = pkg.identity(cls)
    own_full = {
        f"plugin.{plugin_name}.{a}"
        for a, d in declared.items()
        if d.kind == "channel" and d.role in ("send", "both")
    }
    for attr, decl in declared.items():
        try:
            _channels.check_logical(attr)
        except JitError as exc:
            raise class_error(cls, exc.reason, attr) from None
        if attr not in own:
            raise class_error(
                cls,
                f"the port '{attr}' needs an annotation with its element type, "
                f"such as `{attr}: jit.{'Sender' if decl.role == 'send' else 'Receiver'}[int]`",
                attr,
            )
        tp = resolve_hint(cls, attr, own[attr])
        name = _PORT_ORIGINS.get(get_origin(tp))  # type: ignore[arg-type]
        want = _PORT_FACTORY[(decl.kind, decl.role)]
        args = get_args(tp)
        if name is None or len(args) != 1:
            raise class_error(
                cls,
                f"the port '{attr}' is declared with {want}; annotate it "
                "jit.Sender[int], jit.Receiver[float], jit.Channel[int], jit.Latest[int] or "
                "jit.Phase[float]",
                attr,
            )
        kind, roles = _PORT_ANNOTATION[name]
        if kind != decl.kind or decl.role not in roles:
            raise class_error(
                cls,
                f"the port '{attr}' is annotated jit.{name}[...] but declared with {want}",
                attr,
            )
        elem = _channels.elem_of(args[0])
        if elem is None:
            raise class_error(
                cls,
                f"the port '{attr}' carries {getattr(args[0], '__name__', args[0])!s}; a port "
                "carries an int or a float (a str is a view or an interned id that is valid "
                "only in the body that read it, so it cannot cross a channel)",
                attr,
            )
        decl.attr = attr
        decl.elem = elem
        decl.owner = True
        decl.logical = attr
        decl.full = f"plugin.{plugin_name}.{attr}"
        src = decl.source
        if isinstance(src, str):
            if decl.kind == "channel" and src in own_full:
                raise class_error(
                    cls,
                    f"the port '{attr}' receives {src}, a channel of this plugin: its sends "
                    "close only when its finalize ends, so a receive in it would wait for "
                    "ever; use jit.channel for both ends in one plugin",
                    attr,
                )
            decl.owner = False
            decl.logical = decl.full = src
        elif src is not None:
            if not src.full:
                raise class_error(
                    cls,
                    f"the port '{attr}' refers to a port of a plugin that is not built yet; "
                    "declare and decorate that class first",
                    attr,
                )
            if src.elem != elem:
                raise class_error(
                    cls,
                    f"the port '{attr}' carries {args[0].__name__} but {src.full} carries "
                    f"{'int' if src.elem == 'i64' else 'float'}",
                    attr,
                )
            if src.policy == "block":
                raise class_error(
                    cls,
                    f"the port '{attr}' refers to the Block channel {src.full}, which stays in "
                    "the plugin that owns it (a Block send to another plugin would wait for "
                    "ever once it passed the credits); use policy 'fail' or 'drop_oldest'",
                    attr,
                )
            decl.owner = False
            decl.logical = decl.full = src.full
            decl.credits, decl.policy = src.credits, src.policy
            decl.max_msg_bytes, decl.merge = src.max_msg_bytes, src.merge
    seen: Dict[str, str] = {}
    for attr, decl in declared.items():
        if decl.logical in seen:
            raise class_error(
                cls, f"the ports '{seen[decl.logical]}' and '{attr}' name one port", attr
            )
        seen[decl.logical] = attr
    return declared


def build_plugin(cls: type, pkg: "JitPackage") -> type:
    maps: Dict[str, AnyMap] = {}
    ports: Dict[str, PortHandle] = {}
    chans: Dict[str, _PortDecl] = {}
    each: List[EachEvent] = []
    batches: List[EachBatch] = []
    task_fns: Dict[str, Callable[..., object]] = {}
    finalize_fns: List[Callable[..., object]] = []
    for attr, val in vars(cls).items():
        if isinstance(val, Map):
            maps[attr] = val
        elif isinstance(val, PortHandle):
            ports[attr] = val
        elif isinstance(val, _PortDecl):
            chans[attr] = val
        elif isinstance(val, EachEvent):
            each.append(val)
        elif isinstance(val, EachBatch):
            batches.append(val)
        elif callable(val) and getattr(val, _flow.TASK_ATTR, False):
            task_fns[attr] = val
        elif callable(val) and getattr(val, _flow.FINALIZE_ATTR, False):
            finalize_fns.append(val)
    plains, stores = state_fields(cls)
    if not maps and not ports and not plains and not stores and not chans:
        raise class_error(
            cls,
            "@jit.plugin needs at least one jit.map, plain-container state field, jit.Store, "
            "jit.publish/jit.consume attribute or port (jit.sender, jit.receiver, jit.channel, "
            "jit.latest, jit.phase)",
        )
    if batches:
        if each:
            raise class_error(
                cls, "@jit.plugin takes one @jit.each_event or one @jit.each_batch, not both"
            )
        if task_fns or finalize_fns:
            raise class_error(
                cls,
                "@jit.task and @jit.finalize need an @jit.each_event plugin, not @jit.each_batch",
            )
        return _build_batch_plugin(
            cls, pkg, maps, ports, plains, stores, batches, _port_decls(cls, pkg, chans)
        )
    if len(each) != 1:
        raise class_error(
            cls, "@jit.plugin needs exactly one @jit.each_event or @jit.each_batch method"
        )
    if (plains or stores) and each[0].raw:
        raise class_error(
            cls, "plain-container state and jit.Store need an AST-compiled each_event body, not raw"
        )
    if (task_fns or finalize_fns) and each[0].raw:
        raise class_error(cls, "@jit.task and @jit.finalize need an AST-compiled each_event body")
    if chans and each[0].raw:
        raise class_error(
            cls, "channels, latest values and phase results need an AST-compiled each_event body"
        )
    chans = _port_decls(cls, pkg, chans)
    if len(finalize_fns) > 1:
        raise class_error(cls, "@jit.plugin takes one @jit.finalize method")
    if task_fns and not finalize_fns:
        raise class_error(
            cls, "a @jit.task is started from the @jit.finalize body; this plugin has none"
        )
    # A publish port's wire id is only knowable once its class is built: it is
    # package + module + class + attribute, resolved here so a later
    # consume(ThisClass.port) sees a real name.
    for attr, port in ports.items():
        if port.role == "publish":
            port.name = pkg.identity(cls, attr)
    for attr, decl in maps.items():
        if decl.is_product and builtins.any(m.elem == "str" for m in decl.values):
            raise class_error(
                cls,
                f"@jit.plugin map '{attr}': a string-valued reduction "
                "(jit.set/list/argmin/argmax/topk/sample with of=jit.str_) cannot be a "
                "product component; declare it as its own jit.map",
                attr,
            )
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise class_error(cls, "@jit.plugin plan_duql must be a query DSL string", "plan_duql")
    configs = config_fields(cls)
    op_defs: List[str] = []
    task_sigs: Dict[str, _TaskSig] = {}
    if each[0].raw:
        body = raw_body(each[0].fn)
        # A raw body is spliced verbatim, so which fields it reads off `e` is
        # not knowable; resolve all of them.
        fields_used: builtins.set[str] = builtins.set(ALL_FIXED_FIELDS)
        str_literals: List[str] = []
        arg_keys: List[str] = []
        arg_helpers: builtins.set[str] = builtins.set()
        # A raw body appends rows itself, so the row budget cannot be counted
        # from the AST; RAW_ROWS_PER_EVENT is the contract it must respect.
        rows_per_event = {attr: RAW_ROWS_PER_EVENT for attr in maps}
    else:
        compiler = Compiler(
            maps,
            referenced_ops(each[0].fn),
            ports=ports,
            config_fields=configs,
            plain=plains,
            stores=stores,
            chans=chans,
        )
        for attr, fn in task_fns.items():
            src = Source(fn)
            try:
                task_sigs[attr] = _flow.task_signature(fn, attr, src.text)
            except JitError as exc:
                raise src.relocate(exc, None) from None
        compiler.tasks = task_sigs
        body = compiler.lower(each[0].fn)
        for attr in plains:
            if attr not in compiler.plain_forms:
                raise class_error(
                    cls,
                    f"'{attr}' is never updated in the each_event body, so its merge "
                    "cannot be inferred; remove it or update it",
                    attr,
                )
        fields_used = compiler.fields_used
        str_literals = compiler.str_literals
        arg_keys = compiler.arg_keys
        arg_helpers = compiler.arg_helpers
        op_defs = compiler.op_defs
        rows_per_event = compiler.rows_per_event
    flow_fns: "List[_FlowFn]" = []
    flow_defs: "List[str] | None" = None
    finalize_lines: "List[str] | None" = None
    flow_uses: "builtins.set[str]" = (
        builtins.set(compiler.uses) if not each[0].raw else builtins.set()
    )
    extra_gets: "builtins.set[str]" = builtins.set()
    if finalize_fns:
        sigs = task_sigs
        for fn, sig in [(finalize_fns[0], None)] + [(task_fns[a], sigs[a]) for a in task_fns]:
            flow_compiler = Compiler(
                maps,
                referenced_ops(fn),
                ports={},
                config_fields=configs,
                plain={},
                stores=stores,
                chans=chans,
            )
            flow_compiler.tasks = sigs
            flow = flow_compiler.lower_flow(fn, sig)
            flow_fns.append(flow)
            for def_text in flow.op_defs:
                if def_text not in op_defs:
                    op_defs.append(def_text)
            extra_gets |= flow.store_gets
            flow_uses |= flow.uses
        cfg_pre: List[str] = ["    const DftuCfg* _c = R->cfg;", "    (void)_c;"] if configs else []
        for cf in configs.values():
            cfg_pre += cf.locals_()
            if cf.kind == "str":
                cfg_pre += [
                    f"    const dftu_str _cfgs_{cf.name} = host->intern(host->h, _c->v_{cf.name}, "
                    f"_c->l_{cf.name});",
                    f"    (void)_cfgs_{cf.name};",
                ]
        flow_defs, finalize_lines, fu = _flow.emit_flows(
            flow_fns,
            has_cfg=bool(configs),
            cfg_pre=cfg_pre,
            has_stores=bool(stores),
            chans=chans,
        )
        flow_uses |= fu
    map_ids = {attr: pkg.identity(cls, attr) for attr in maps}
    source = emit_plugin(
        maps,
        body,
        fields_used,
        plan_duql,
        str_literals,
        arg_keys,
        arg_helpers,
        rows_per_event,
        op_defs,
        ports,
        configs,
        raw_event=each[0].raw,
        map_ids=map_ids,
        stores=stores,
        store_gets=(compiler.store_gets | extra_gets) if not each[0].raw else None,
        store_puts=compiler.store_puts if not each[0].raw else None,
        mask_guard=compiler.mask_guard if not each[0].raw else None,
        can_raise=compiler.can_raise if not each[0].raw else False,
        flow_uses=flow_uses,
        res_decls=compiler.res_decls if not each[0].raw else None,
        res_frees=compiler.res_frees if not each[0].raw else None,
        flow_defs=flow_defs,
        finalize_lines=finalize_lines,
        chans=chans,
        chan_uses=compiler.chan_uses if not each[0].raw else None,
        plugin_name=pkg.identity(cls) if chans else "dftracer.jit",
    )
    result_names = {qid: attr for attr, qid in map_ids.items()}
    name = store_plugin_name(pkg.identity(cls)) if stores else cls.__name__
    if each[0].raw:
        lowering = "raw"
    elif compiler.mask_guard is not None:
        lowering = "mask"
    else:
        lowering = "branch"
    plugin = JitPlugin(name, source, {}, result_names, pkg.shippable(cls), lowering)
    setattr(cls, "_jit_plugin", plugin)
    if configs:
        setattr(cls, "_jit_config", configs)
    if chans:
        setattr(cls, "_jit_ports", chans)
    return cls


def _build_batch_plugin(
    cls: type,
    pkg: "JitPackage",
    maps: Dict[str, AnyMap],
    ports: Dict[str, PortHandle],
    plains: Dict[str, PlainField],
    stores: Dict[str, StoreField],
    batches: "List[EachBatch]",
    chans: "Dict[str, _PortDecl]",
) -> type:
    if len(batches) != 1:
        raise class_error(cls, "@jit.plugin needs exactly one @jit.each_batch method")
    if ports or stores:
        raise class_error(
            cls,
            "an @jit.each_batch plugin folds into scalar state fields and key-less jit.map "
            "descriptors; publish/consume ports and jit.Store need @jit.each_event",
        )
    plan_duql = getattr(cls, "plan_duql", None)
    if plan_duql is not None and not isinstance(plan_duql, str):
        raise class_error(cls, "@jit.plugin plan_duql must be a query DSL string", "plan_duql")
    if config_fields(cls):
        raise class_error(cls, "an @jit.each_batch plugin takes no config; use @jit.each_event")
    hook = lower_batch(
        batches[0].fn,
        cast("Dict[str, object]", maps),
        cast("Dict[str, object]", plains),
        cast("Dict[str, object]", chans),
    )
    for attr in plains:
        if attr not in hook.updated:
            raise class_error(
                cls,
                f"'{attr}' is never updated in the each_batch body, so its merge cannot be "
                "inferred; remove it or update it",
                attr,
            )
        field = plains[attr]
        monoid: Monoid = (
            Counter("DFTU_AGG_SUM", elem="i64")
            if field.elem == "i64"
            else Sum("DFTU_AGG_SUM", elem="f64")
        )
        maps[attr] = Map((), (monoid,), False)
    map_ids = {attr: pkg.identity(cls, attr) for attr in maps}
    source = emit_plugin(
        maps,
        [],
        builtins.set(),
        plan_duql,
        [],
        [],
        builtins.set(),
        hook.rows,
        HELPERS,
        {},
        {},
        map_ids=map_ids,
        batch=hook,
        flow_uses=builtins.set(["ports"]) if chans else None,
        chans=chans,
        chan_uses=hook.chan_uses,
        plugin_name=pkg.identity(cls) if chans else "dftracer.jit",
    )
    result_names = {qid: attr for attr, qid in map_ids.items()}
    plugin = JitPlugin(cls.__name__, source, {}, result_names, pkg.shippable(cls), "batch")
    setattr(cls, "_jit_plugin", plugin)
    if chans:
        setattr(cls, "_jit_ports", chans)
    return cls


# The op registry is shared with the engine's built-ins, whose names are bare
# (`add`, `sum`) or `dftu.`-prefixed; both namespaces are the host's, so a user
# op must be `<module>.<name>` (plugin ABI rule, see plugins/reserved_names.h).
_HOST_NAME_PREFIX = "dftu."


def series_module(fn: Callable[..., Expr], module: "str | None", pkg: "JitPackage") -> str:
    if module is not None:
        mod = module
    else:
        fn_module = getattr(fn, "__module__", None)
        if not isinstance(fn_module, str) or not fn_module:
            raise JitError('@jit.series needs a module prefix; pass module="<name>"')
        # namespace + rest reconstructs fn.__module__ exactly for a default
        # package (namespace is its top-level segment), so an explicit
        # JitPackage only ever changes the leading segment.
        rest = module_rest(fn_module)
        mod = pkg._namespace_for(fn_module) + (f".{rest}" if rest else "")
    if mod == _HOST_NAME_PREFIX[:-1] or mod.startswith(_HOST_NAME_PREFIX):
        raise JitError(
            f"module '{mod}' is reserved for host ops; the 'dftu.' namespace is not "
            "available to a user op"
        )
    return mod


def series_impl(
    fn: Callable[..., Expr], module: "str | None", pkg: "JitPackage"
) -> Callable[..., Result]:
    fname = getattr(fn, "__name__", None)
    if not isinstance(fname, str):
        raise JitError("@jit.series must decorate a named function")
    params = builtins.list(inspect.signature(fn).parameters.values())
    if builtins.any(p.kind not in (p.POSITIONAL_OR_KEYWORD, p.POSITIONAL_ONLY) for p in params):
        raise JitError("@jit.series takes only positional column arguments")
    n = len(params)
    if n == 0:
        raise JitError("@jit.series needs at least one column argument")
    built = fn(*[col(f"__x{i}__") for i in range(n)])
    if not isinstance(built, Expr):
        raise JitError("@jit.series body must return a column expression built from its arguments")
    name = f"{series_module(fn, module, pkg)}.{fname}"
    _ops._register_user(name, n, built)
    return _ops.get(name)
