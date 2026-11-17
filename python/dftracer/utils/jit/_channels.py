# SPDX-License-Identifier: MIT
"""The authoring surface of the port constructs of ``@jit.plugin``: bounded
channels between plugins (:class:`Sender`, :class:`Receiver`, :class:`Channel`),
latest-value hints (:class:`Latest`) and phase results (:class:`Phase`).

A port is a class attribute whose annotation says its element type (``int`` or
``float``) and whose value is one of the factories below. The compiler in
``dftracer.utils.jit`` declares it to the host in the plugin factory
(``dftu_svc_ports``) and lowers the calls on it to C; calling one in plain
Python raises :class:`~dftracer.utils.jit.JitError`.
"""

from __future__ import annotations

import builtins
from typing import (
    Any,
    AsyncIterator,
    Awaitable,
    Dict,
    List,
    Literal,
    NoReturn,
    Protocol,
    Tuple,
    TypeVar,
    cast,
    overload,
)

from .._c_text import c_str_literal
from .._jit_errors import JitError

T = TypeVar("T")
T_co = TypeVar("T_co", covariant=True)
T_contra = TypeVar("T_contra", contravariant=True)

Policy = Literal["fail", "block", "drop_oldest"]
Merge = Literal["sum", "min", "max"]

DEFAULT_CREDITS = 16

POLICIES = {
    "fail": "DFTU_OVERFLOW_FAIL",
    "block": "DFTU_OVERFLOW_BLOCK",
    "drop_oldest": "DFTU_OVERFLOW_DROP_OLDEST",
}
MERGES = {"sum": "DFTU_MERGE_SUM", "min": "DFTU_MERGE_MIN", "max": "DFTU_MERGE_MAX"}

ELEMS = {int: "i64", float: "f64"}

_NAME_CHARS = frozenset("abcdefghijklmnopqrstuvwxyz0123456789._/-")


class Sender(Protocol[T_contra]):
    """The sending end of a channel, declared on a ``@jit.plugin`` class as
    ``out: jit.Sender[int] = jit.sender(...)``.

    ``self.out.send(v)`` delivers ``v`` at once or raises :class:`~dftracer.utils.jit.Error`
    when the channel is full or closed; ``await self.out.send(v)`` (in an ``async``
    finalize or task) waits for a credit on a ``"block"`` :class:`Channel`. ``try_send(v)``
    never waits and says whether the value was stored. ``close()`` ends the
    sends: a receiver still gets what is queued, then the end of the channel.
    A plugin's senders close when its ``@jit.finalize`` ends."""

    def send(self, value: T_contra, /) -> Awaitable[None]: ...
    def try_send(self, value: T_contra, /) -> bool: ...
    def close(self) -> None: ...


class Receiver(Protocol[T_co]):
    """The receiving end of a channel, declared as
    ``inp: jit.Receiver[int] = jit.receiver(Producer.out)``.

    ``v = await self.inp.recv()`` (in an ``async`` finalize or task) waits for the
    next value and raises :class:`~dftracer.utils.jit.Closed` once the channel is
    closed and drained. ``async for v in self.inp:`` takes values until then."""

    def recv(self) -> Awaitable[T_co]: ...
    def __aiter__(self) -> AsyncIterator[T_co]: ...


class Channel(Sender[T], Receiver[T], Protocol[T]):
    """Both ends of a channel in one plugin, such as a producer task and a
    consumer task of one ``@jit.finalize``: ``q: jit.Channel[int] = jit.channel()``."""


class Latest(Protocol[T]):
    """A latest-value hint: ``self.cfg.set(v)`` stores the last value and
    ``self.cfg.get(default)`` reads it without waiting (``default`` before the first
    set). A reference, ``jit.latest(Other.cfg)``, reads or sets another plugin's."""

    def set(self, value: T, /) -> None: ...
    def get(self, default: T, /) -> T: ...


class Phase(Protocol[T]):
    """A phase result merged over every worker: ``self.p.contribute(v)`` folds a
    value in with the declared merge (``"sum"``, ``"min"`` or ``"max"``), ``seal()``
    closes it to contributions (a ``@jit.finalize`` or task call) and
    ``self.p.result(default)`` reads the merged value of a sealed phase (``default``
    when nothing was contributed)."""

    def contribute(self, value: T, /) -> None: ...
    def seal(self) -> None: ...
    def result(self, default: T = ..., /) -> T: ...


class PortDecl:
    """What a port attribute was declared as; the compiler completes it from the
    class annotation (``elem``) and the plugin identity (``full``)."""

    __slots__ = (
        "attr",
        "credits",
        "elem",
        "full",
        "kind",
        "logical",
        "max_msg_bytes",
        "merge",
        "owner",
        "policy",
        "role",
        "source",
    )

    def __init__(
        self,
        kind: str,
        role: str,
        *,
        credits: "int | None" = None,
        policy: "str | None" = None,
        max_msg_bytes: "int | None" = None,
        merge: "str | None" = None,
        source: "PortDecl | str | None" = None,
    ) -> None:
        self.kind = kind
        self.role = role
        self.credits = credits
        self.policy = policy
        self.max_msg_bytes = max_msg_bytes
        self.merge = merge
        self.source = source
        self.attr = ""
        self.elem: "str | None" = None
        self.owner = True
        self.logical = ""
        self.full = ""

    def __getattr__(self, name: str) -> "NoReturn":
        if name.startswith("__"):
            raise AttributeError(name)
        raise JitError(
            f"{name} is a method of a plugin port; call it on self.<port> inside a "
            "@jit.each_event, @jit.finalize or @jit.task body, where it lowers to C."
        )


def _options(
    fn: str, credits: int, policy: str, max_msg_bytes: "int | None"
) -> "Tuple[int, str, int | None]":
    if not isinstance(credits, int) or isinstance(credits, bool) or credits < 1:
        raise JitError(f"jit.{fn}: credits is a count of at least 1, got {credits!r}")
    if policy not in POLICIES:
        raise JitError(
            f"jit.{fn}: policy is 'fail', 'block' or 'drop_oldest', got {policy!r}",
        )
    if policy == "block" and fn != "channel":
        raise JitError(
            f"jit.{fn}: a Block channel waits for its other end, and the finalize bodies of "
            "two plugins run one after the other, so a send to another plugin would wait for "
            "ever once it passed the credits. Keep both ends in one plugin with jit.channel "
            "(a producer task and a consumer task), or use policy 'fail' or 'drop_oldest' "
            "between plugins"
        )
    if max_msg_bytes is not None and (
        not isinstance(max_msg_bytes, int) or isinstance(max_msg_bytes, bool) or max_msg_bytes < 1
    ):
        raise JitError(f"jit.{fn}: max_msg_bytes is a size of at least 1, got {max_msg_bytes!r}")
    return credits, policy, max_msg_bytes


def sender(
    *, credits: int = DEFAULT_CREDITS, policy: Policy = "fail", max_msg_bytes: "int | None" = None
) -> "Sender[T]":
    """Declare the sending end of a channel owned by this plugin. ``credits`` is
    the number of messages the channel holds; ``policy`` is what a send does when
    it is full: ``"fail"`` reports it and ``"drop_oldest"`` drops the oldest
    message. (``"block"``, which waits for a credit, is for :func:`channel`: the
    finalize bodies of two plugins run one after the other, so a blocked send to
    another plugin could never be drained.) Both ends of one channel declare the
    same options; a reference made with :func:`receiver` takes them from this
    declaration."""
    c, p, m = _options("sender", credits, policy, max_msg_bytes)
    return cast("Sender[T]", PortDecl("channel", "send", credits=c, policy=p, max_msg_bytes=m))


def channel(
    *, credits: int = DEFAULT_CREDITS, policy: Policy = "fail", max_msg_bytes: "int | None" = None
) -> "Channel[T]":
    """Declare both ends of a channel in this plugin (see :func:`sender`)."""
    c, p, m = _options("channel", credits, policy, max_msg_bytes)
    return cast("Channel[T]", PortDecl("channel", "both", credits=c, policy=p, max_msg_bytes=m))


@overload
def receiver(source: "Sender[T]", /) -> "Receiver[T]": ...
@overload
def receiver(
    source: str,
    /,
    *,
    credits: int = DEFAULT_CREDITS,
    policy: Policy = "fail",
    max_msg_bytes: "int | None" = None,
) -> "Receiver[T]": ...
def receiver(
    source: "Sender[Any] | str",
    /,
    *,
    credits: int = DEFAULT_CREDITS,
    policy: Policy = "fail",
    max_msg_bytes: "int | None" = None,
) -> "Receiver[Any]":
    """Declare the receiving end of a channel another plugin owns: pass its
    :func:`sender` attribute (``jit.receiver(Producer.out)``), which carries the
    options and the element type, so a mismatch is a compile error. A string
    names a channel of a hand-written plugin in full, ``"plugin.<owner>.<name>"``,
    and then ``credits``, ``policy`` and ``max_msg_bytes`` must equal the owner's."""
    if isinstance(source, str):
        _check_full_name("receiver", source)
        c, p, m = _options("receiver", credits, policy, max_msg_bytes)
        return cast(
            "Receiver[Any]",
            PortDecl("channel", "recv", credits=c, policy=p, max_msg_bytes=m, source=source),
        )
    if not isinstance(source, PortDecl) or source.kind != "channel" or source.role == "recv":
        raise JitError(
            "jit.receiver(...) needs the jit.sender or jit.channel attribute of another "
            "plugin, or the full name of a channel such as 'plugin.owner.name'"
        )
    return cast("Receiver[Any]", PortDecl("channel", "recv", source=source))


@overload
def latest() -> "Latest[T]": ...
@overload
def latest(source: "Latest[T]", /) -> "Latest[T]": ...
@overload
def latest(source: str, /) -> "Latest[T]": ...
def latest(source: "Latest[Any] | str | None" = None, /) -> "Latest[Any]":
    """Declare a latest-value port. With no argument this plugin owns it; with the
    :func:`latest` attribute of another plugin (or a full name,
    ``"plugin.<owner>.<name>"``) it reads and sets that one."""
    if source is None:
        return cast("Latest[Any]", PortDecl("latest", "own"))
    if isinstance(source, str):
        _check_full_name("latest", source)
        return cast("Latest[Any]", PortDecl("latest", "ref", source=source))
    if not isinstance(source, PortDecl) or source.kind != "latest":
        raise JitError("jit.latest(...) needs the jit.latest attribute of another plugin")
    return cast("Latest[Any]", PortDecl("latest", "ref", source=source))


@overload
def phase(*, merge: Merge = "sum") -> "Phase[T]": ...
@overload
def phase(source: "Phase[T]", /) -> "Phase[T]": ...
def phase(source: "Phase[Any] | None" = None, /, *, merge: Merge = "sum") -> "Phase[Any]":
    """Declare a phase result with a merge (``"sum"``, ``"min"`` or ``"max"``). With
    the :func:`phase` attribute of another plugin, it contributes to and reads that
    one."""
    if source is None:
        if merge not in MERGES:
            raise JitError(f"jit.phase: merge is 'sum', 'min' or 'max', got {merge!r}")
        return cast("Phase[Any]", PortDecl("phase", "own", merge=merge))
    if not isinstance(source, PortDecl) or source.kind != "phase":
        raise JitError("jit.phase(...) needs the jit.phase attribute of another plugin")
    return cast("Phase[Any]", PortDecl("phase", "ref", source=source))


def _check_full_name(fn: str, name: str) -> None:
    if not name.startswith("plugin."):
        raise JitError(f"jit.{fn}: a name is a full name 'plugin.<owner>.<name>', got {name!r}")
    if builtins.any(c not in _NAME_CHARS for c in name):
        raise JitError(f"jit.{fn}: the name {name!r} must be lowercase ASCII [a-z0-9._/-]")


def check_logical(attr: str) -> str:
    """The logical host name of an attribute; the host takes ``[a-z0-9._/-]``."""
    if not attr or builtins.any(c not in _NAME_CHARS for c in attr):
        raise JitError(
            f"the port attribute '{attr}' must be lowercase ASCII [a-z0-9._/-] to name a port"
        )
    return attr


def elem_of(tp: object) -> "str | None":
    """``i64`` or ``f64`` for an ``int`` or ``float`` annotation argument."""
    return ELEMS.get(tp) if isinstance(tp, type) else None


# ---- C helpers emitted into a plugin that uses ports -------------------------

C_PORT_HELPERS = [
    "static dftu_status dftu_jit_chan_open(dftu_ctx* ctx, const dftu_svc_ports* p, void* h,",
    "                                      const char* name, uint64_t* out) {",
    "    dftu_channel_desc d;",
    "    memset(&d, 0, sizeof(d));",
    "    d.struct_size = sizeof(d);",
    "    d.name = name;",
    "    return p->channel_open(ctx, h, &d, out);",
    "}",
    "",
]


def channels_of(chans: "Dict[str, PortDecl]") -> "List[PortDecl]":
    return [d for d in chans.values() if d.kind == "channel"]


def senders_of(chans: "Dict[str, PortDecl]") -> "List[PortDecl]":
    return [d for d in channels_of(chans) if d.role in ("send", "both")]


def factory_lines(chans: "Dict[str, PortDecl]") -> "List[str]":
    """The plugin factory's declarations: each channel end and each phase this
    plugin owns. The host checks every end of a channel against the others at plan
    time, so a mismatch fails the load."""
    channels = channels_of(chans)
    phases = [d for d in chans.values() if d.kind == "phase" and d.owner]
    if not channels and not phases:
        return []
    out = [
        "    {",
        "        const dftu_svc_ports* _p = (const dftu_svc_ports*)h->get_service(h->h, DFTU_SVC_PORTS);",
        '        if (!_p) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no ports service");',
        "        dftu_status _s = DFTU_OK;",
    ]
    if channels:
        out += ["        dftu_channel_desc _d;", "        uint64_t _ch = 0;"]
    if phases:
        out.append("        uint64_t _gen = 0;")
    for d in channels:
        mask = ["DFTU_CHANNEL_F_CREDITS", "DFTU_CHANNEL_F_OVERFLOW"]
        if d.max_msg_bytes is not None:
            mask.append("DFTU_CHANNEL_F_MAX_MSG_BYTES")
        if d.policy == "block":
            mask.append("DFTU_CHANNEL_F_TASK_END")
        if d.role in ("send", "both"):
            mask.append("DFTU_CHANNEL_F_SENDS")
        if d.role in ("recv", "both"):
            mask.append("DFTU_CHANNEL_F_RECEIVES")
        assert d.policy is not None and d.credits is not None
        out += [
            "        memset(&_d, 0, sizeof(_d));",
            "        _d.struct_size = sizeof(_d);",
            f"        _d.field_mask = {' | '.join(mask)};",
            f"        _d.name = {c_str_literal(d.logical)};",
            f"        _d.credits = {d.credits};",
            f"        _d.overflow = {POLICIES[d.policy]};",
        ]
        if d.max_msg_bytes is not None:
            out.append(f"        _d.max_msg_bytes = {d.max_msg_bytes};")
        out += [
            "        _s = _p->channel_open(ctx, h->h, &_d, &_ch);",
            "        if (_s < 0) return _s;",
        ]
    for d in phases:
        assert d.merge is not None and d.elem is not None
        ty = "DFTU_T_I64" if d.elem == "i64" else "DFTU_T_F64"
        out += [
            f"        _s = _p->phase_declare(ctx, h->h, {c_str_literal(d.logical)}, "
            f"{MERGES[d.merge]}, {ty}, 0, &_gen);",
            "        if (_s < 0) return _s;",
        ]
    out.append("    }")
    return out


def root_fields(chans: "Dict[str, PortDecl]") -> "List[str]":
    out = ["    const dftu_svc_ports* prt;"]
    out += [f"    uint64_t ch_{d.attr};" for d in channels_of(chans)]
    return out


def root_open_lines(chans: "Dict[str, PortDecl]") -> "List[str]":
    """In ``on_finalize``: open every channel handle once for the frames."""
    out: "List[str]" = []
    for d in channels_of(chans):
        out += [
            "    {",
            f"        dftu_status _o = dftu_jit_chan_open(ctx, R->prt, host->h, {c_str_literal(d.logical)}, &R->ch_{d.attr});",
            "        if (_o < 0) {",
            "            _root_free(R);",
            "            return _o;",
            "        }",
            "    }",
        ]
    return out


def root_close_lines(chans: "Dict[str, PortDecl]") -> "List[str]":
    """At the end of the finalize frame: end the sends, so a receiver that runs
    after this plugin gets the end of the channel instead of waiting for it."""
    return [
        f"    if (R->prt) R->prt->channel_close(ctx, host->h, R->ch_{d.attr});"
        for d in senders_of(chans)
    ]


def event_open_lines(chans: "Dict[str, PortDecl]", uses: "builtins.set[str]") -> "List[str]":
    """In ``on_batch``: the handles of the channels the per-event body uses."""
    out: "List[str]" = []
    for d in channels_of(chans):
        if d.attr not in uses:
            continue
        out += [
            f"    uint64_t _ch_{d.attr} = 0;",
            "    if (_st >= 0 && _prt) {",
            f"        _st = dftu_jit_chan_open(ctx, _prt, host->h, {c_str_literal(d.logical)}, &_ch_{d.attr});",
            "    }",
        ]
    return out


def close_without_finalize(chans: "Dict[str, PortDecl]") -> "List[str]":
    """The body of an ``on_finalize`` for a plugin that has no finalize method: it
    still ends its sends."""
    senders = senders_of(chans)
    out = [
        "static dftu_status on_finalize(dftu_ctx* ctx, void* slice,",
        "                               const dftu_plugin_host* host, dftu_task** pending) {",
        "    (void)slice;",
        "    *pending = NULL;",
    ]
    if not senders:
        return out + ["    (void)ctx;", "    (void)host;", "    return DFTU_OK;", "}", ""]
    out += [
        "    const dftu_svc_ports* _p = (const dftu_svc_ports*)host->get_service(host->h, DFTU_SVC_PORTS);",
        '    if (!_p) return dftu_fail(ctx, DFTU_E_UNSUPPORTED, "the host has no ports service");',
        "    uint64_t _ch = 0;",
    ]
    for d in senders:
        out += [
            f"    if (dftu_jit_chan_open(ctx, _p, host->h, {c_str_literal(d.logical)}, &_ch) >= 0)",
            "        _p->channel_close(ctx, host->h, _ch);",
        ]
    return out + ["    return DFTU_OK;", "}", ""]


def provided_names(chans: "Dict[str, PortDecl]") -> "List[str]":
    """The channels this plugin owns a sending end of, as ``provides`` names: the
    host runs the sender's finalize before the receiver's, so a receiver that waits
    for the end of the channel is not started before the sender can close it."""
    return [d.full for d in senders_of(chans) if d.owner]


def consumed_names(chans: "Dict[str, PortDecl]") -> "List[str]":
    """Every channel this plugin receives, by full name, as ``consumes`` names, so
    the host runs the sender's plugin first. A hand-written sender lists the same
    name in its ``provides``; without it the host refuses the plan."""
    return [d.full for d in channels_of(chans) if d.role == "recv"]
