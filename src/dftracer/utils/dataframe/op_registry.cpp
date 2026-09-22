#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/internal/op_dispatch.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

extern "C" {

dftu_series* dftu_series_take_i32(const dftu_series* v, const int64_t* idx,
                                  int32_t n) {
    return dftu_series_take(v, idx, n);
}
dftu_dataframe* dftu_dataframe_take_i32(const dftu_dataframe* df,
                                        const int64_t* idx, int32_t n) {
    return dftu_dataframe_take(df, idx, n);
}

}  // extern "C"

namespace {

using dftracer::utils::dataframe::internal::op_fn;

// A function-to-void* cast is not constexpr, so this array is dynamically
// initialized before main; fine, since lookups only run at scan time.
const dftu_op_desc BUILTINS[] = {
#define DFTU_SERIES_OP(name, fn, ret, o0, o1, o2) \
    {#name, DFTU_OP_SIG(ret, o0, o1, o2), reinterpret_cast<const void*>(&fn)},
#include <dftracer/utils/dataframe/exported_series_ops.def>
#define DFTU_FRAME_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6)         \
    {"dftu.frame." #name, DFTU_OP_SIG8(ret, o0, o1, o2, o3, o4, o5, o6), \
     reinterpret_cast<const void*>(&fn)},
#include <dftracer/utils/dataframe/exported_frame_ops.def>
#define DFTU_LAZY_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6)         \
    {"dftu.lazy." #name, DFTU_OP_SIG8(ret, o0, o1, o2, o3, o4, o5, o6), \
     reinterpret_cast<const void*>(&fn)},
#include <dftracer/utils/dataframe/exported_lazy_ops.def>
};
constexpr uint32_t BUILTIN_COUNT =
    static_cast<uint32_t>(sizeof(BUILTINS) / sizeof(BUILTINS[0]));

// Checks every .def row's fn against the type op_fn<sig> says the runner will
// cast it to; op_fn is undefined for a signature with no runner case, so an
// unrunnable row fails here instead of dispatching through the wrong type.
#define DFTU_SERIES_OP(name, fn, ret, o0, o1, o2)                            \
    static_assert(std::is_same_v<decltype(&fn),                              \
                                 op_fn<DFTU_OP_SIG(ret, o0, o1, o2)>::type>, \
                  #name ": .def signature does not match the function");
#include <dftracer/utils/dataframe/exported_series_ops.def>

#define DFTU_FRAME_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6)              \
    static_assert(                                                            \
        std::is_same_v<decltype(&fn), op_fn<DFTU_OP_SIG8(ret, o0, o1, o2, o3, \
                                                         o4, o5, o6)>::type>, \
        #name ": .def signature does not match the function");
#include <dftracer/utils/dataframe/exported_frame_ops.def>

#define DFTU_LAZY_OP(name, fn, ret, o0, o1, o2, o3, o4, o5, o6)               \
    static_assert(                                                            \
        std::is_same_v<decltype(&fn), op_fn<DFTU_OP_SIG8(ret, o0, o1, o2, o3, \
                                                         o4, o5, o6)>::type>, \
        #name ": .def signature does not match the function");                \
    static_assert(                                                            \
        DFTU_OP_SIG_ARG(DFTU_OP_SIG8(ret, o0, o1, o2, o3, o4, o5, o6), 6) ==  \
            DFTU_TOK_##o6,                                                    \
        #name ": operand 6 did not survive packing");
#include <dftracer/utils/dataframe/exported_lazy_ops.def>

std::mutex& reg_mutex() {
    static std::mutex m;
    return m;
}

// A user op's name must outlive the plugin that supplied it: the registry, not
// the plugin, owns it. `desc.name` points at `name`, so the two must stay
// together and `name` must not move once inserted (hence unique_ptr storage,
// which also keeps a record's address stable as the vector grows, so a
// pointer from dftu_op_find stays valid after a later registration).
struct UserOp {
    std::string name;
    dftu_op_desc desc;
};

std::vector<std::unique_ptr<UserOp>>& user_ops() {
    static std::vector<std::unique_ptr<UserOp>> v;
    return v;
}

const dftu_op_desc* find_locked(const char* name) {
    for (uint32_t i = 0; i < BUILTIN_COUNT; ++i)
        if (std::strcmp(BUILTINS[i].name, name) == 0) return &BUILTINS[i];
    for (const auto& op : user_ops())
        if (op->name == name) return &op->desc;
    return nullptr;
}

using CS = const dftu_series*;
// Ties the reinterpret_cast to the signature, so a runner case cannot cast
// op->fn to any type other than the one the .def row's own static_assert
// (op_fn<S>, from op_dispatch.h) already proved it is.
template <dftu_op_sig S>
typename op_fn<S>::type as_op(const void* fn) {
    return reinterpret_cast<typename op_fn<S>::type>(const_cast<void*>(fn));
}

const char* tok_name(dftu_op_tok t) {
    switch (t) {
        case DFTU_TOK_NONE:
            return "";
        case DFTU_TOK_SERIES:
            return "series";
        case DFTU_TOK_SCALAR:
            return "scalar";
        case DFTU_TOK_I64:
            return "i64";
        case DFTU_TOK_BOOL:
            return "bool";
        case DFTU_TOK_CMP:
            return "cmp";
        case DFTU_TOK_PRIM:
            return "prim";
        case DFTU_TOK_LOGICAL:
            return "logical";
        case DFTU_TOK_DTYPE:
            return "dtype";
        case DFTU_TOK_REDUCE:
            return "reduce";
        case DFTU_TOK_STR:
            return "str";
        case DFTU_TOK_CHAR:
            return "char";
        case DFTU_TOK_F64:
            return "f64";
        case DFTU_TOK_I32:
            return "i32";
        case DFTU_TOK_RANK:
            return "rank";
        case DFTU_TOK_ROLLING:
            return "rolling";
        case DFTU_TOK_FRAME:
            return "frame";
        case DFTU_TOK_STRLIST:
            return "strlist";
        case DFTU_TOK_I32LIST:
            return "i32list";
        case DFTU_TOK_I64LIST:
            return "i64list";
        case DFTU_TOK_LAZY:
            return "lazy";
        case DFTU_TOK_EXPR:
            return "expr";
        case DFTU_TOK_AGGLIST:
            return "agglist";
        case DFTU_TOK_U64:
            return "u64";
        case DFTU_TOK_QUERY:
            return "query";
        case DFTU_TOK_WINLIST:
            return "winlist";
    }
    return "?";
}

// An operand token that is not the signature's primary operand (the one that
// rides the runner's in[]/frames[] array, per dftu_op_arity) is read from
// dftu_op_arg - including a FRAME/LAZY token on a non-FRAME/non-LAZY-kind op
// (e.g. a SERIES-return op over a whole table), which has no separate array
// to ride.
bool needs_arg(dftu_op_sig sig) {
    dftu_op_kind kind = dftu_op_kind_of(sig);
    dftu_op_tok primary = kind == DFTU_OP_KIND_FRAME  ? DFTU_TOK_FRAME
                          : kind == DFTU_OP_KIND_LAZY ? DFTU_TOK_LAZY
                                                      : DFTU_TOK_SERIES;
    for (int i = 0; i < DFTU_OP_MAX_ARGS; ++i) {
        dftu_op_tok t = DFTU_OP_SIG_ARG(sig, i);
        if (t != DFTU_TOK_NONE && t != primary) return true;
    }
    return false;
}

}  // namespace

extern "C" {

dftu_op_kind dftu_op_kind_of(dftu_op_sig sig) {
    dftu_op_tok r = DFTU_OP_SIG_RET(sig);
    if (r == DFTU_TOK_SERIES) return DFTU_OP_KIND_SERIES;
    if (r == DFTU_TOK_FRAME) return DFTU_OP_KIND_FRAME;
    if (r == DFTU_TOK_LAZY) return DFTU_OP_KIND_LAZY;
    return DFTU_OP_KIND_AGGREGATE;
}

uint32_t dftu_op_arity(dftu_op_sig sig) {
    // The primary positional operand: FRAME for a frame op (in frames[]), LAZY
    // for a lazy op (in in[]), else SERIES (in in[]). Every other operand
    // rides dftu_op_arg.
    dftu_op_kind kind = dftu_op_kind_of(sig);
    dftu_op_tok primary = kind == DFTU_OP_KIND_FRAME  ? DFTU_TOK_FRAME
                          : kind == DFTU_OP_KIND_LAZY ? DFTU_TOK_LAZY
                                                      : DFTU_TOK_SERIES;
    uint32_t n = 0;
    for (int i = 0; i < DFTU_OP_MAX_ARGS; ++i)
        if (DFTU_OP_SIG_ARG(sig, i) == primary) ++n;
    return n;
}

const char* dftu_op_signature(dftu_op_sig sig) {
    static thread_local std::string s;
    s = "(";
    bool first = true;
    for (int i = 0; i < DFTU_OP_MAX_ARGS; ++i) {
        dftu_op_tok t = DFTU_OP_SIG_ARG(sig, i);
        if (t == DFTU_TOK_NONE) break;
        if (!first) s += ", ";
        s += tok_name(t);
        first = false;
    }
    s += ") -> ";
    s += tok_name(DFTU_OP_SIG_RET(sig));
    return s.c_str();
}

const dftu_op_desc* dftu_op_find(const char* name) {
    if (!name) return nullptr;
    std::lock_guard<std::mutex> lock(reg_mutex());
    return find_locked(name);
}

uint32_t dftu_op_count(void) {
    std::lock_guard<std::mutex> lock(reg_mutex());
    return BUILTIN_COUNT + static_cast<uint32_t>(user_ops().size());
}

const dftu_op_desc* dftu_op_at(uint32_t i) {
    std::lock_guard<std::mutex> lock(reg_mutex());
    if (i < BUILTIN_COUNT) return &BUILTINS[i];
    i -= BUILTIN_COUNT;
    if (i < user_ops().size()) return &user_ops()[i]->desc;
    return nullptr;
}

int dftu_op_register(const dftu_op_desc* desc) {
    if (!desc || !desc->name) return 1;
    std::lock_guard<std::mutex> lock(reg_mutex());
    if (find_locked(desc->name)) return 1;  // no silent shadowing
    auto rec = std::make_unique<UserOp>();
    rec->name = desc->name;
    rec->desc = *desc;
    rec->desc.name = rec->name.c_str();
    user_ops().push_back(std::move(rec));
    return 0;
}

int dftu_op_unregister(const char* name) {
    if (!name) return 1;
    std::lock_guard<std::mutex> lock(reg_mutex());
    auto& v = user_ops();
    for (auto it = v.begin(); it != v.end(); ++it) {
        if ((*it)->name == name) {
            v.erase(it);
            return 0;
        }
    }
    return 1;  // not found: a no-op, not an error the caller must react to
}

dftu_series* dftu_op_run(const dftu_op_desc* op, const dftu_series* const* in,
                         uint32_t n, const dftu_op_arg* a) {
    if (!op || !op->fn) return nullptr;
    if (dftu_op_kind_of(op->sig) != DFTU_OP_KIND_SERIES) return nullptr;
    if (n != dftu_op_arity(op->sig)) return nullptr;
    if (n != 0 && !in) return nullptr;
    if (needs_arg(op->sig) && !a) return nullptr;
    const dftu_op_val* g = a ? a->args : nullptr;
    switch (op->sig) {
        case DFTU_OP_SIG(SERIES, SERIES, NONE, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, NONE, NONE)>(op->fn)(
                in[0]);
        case DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SERIES, NONE)>(op->fn)(
                in[0], in[1]);
        case DFTU_OP_SIG(SERIES, SERIES, SERIES, SERIES):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SERIES, SERIES)>(op->fn)(
                in[0], in[1], in[2]);
        case DFTU_OP_SIG(SERIES, SERIES, SCALAR, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SCALAR, NONE)>(op->fn)(
                in[0], g[1].scalar);
        case DFTU_OP_SIG(SERIES, SERIES, SCALAR, I32):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SCALAR, I32)>(op->fn)(
                in[0], g[1].scalar, g[2].i32);
        case DFTU_OP_SIG(SERIES, SERIES, CMP, SCALAR):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, CMP, SCALAR)>(op->fn)(
                in[0], static_cast<dftu_cmp_op>(g[1].i32), g[2].scalar);
        case DFTU_OP_SIG(SERIES, SERIES, PRIM, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, PRIM, NONE)>(op->fn)(
                in[0], static_cast<dftu_prim_op>(g[1].i32));
        case DFTU_OP_SIG(SERIES, SERIES, SERIES, LOGICAL):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SERIES, LOGICAL)>(op->fn)(
                in[0], in[1], static_cast<dftu_logical_op>(g[2].i32));
        case DFTU_OP_SIG(SERIES, SERIES, SERIES, CMP):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SERIES, CMP)>(op->fn)(
                in[0], in[1], static_cast<dftu_cmp_op>(g[2].i32));
        case DFTU_OP_SIG(SERIES, SERIES, DTYPE, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, DTYPE, NONE)>(op->fn)(
                in[0], static_cast<dftu_dtype>(g[1].i32));
        case DFTU_OP_SIG(SERIES, SERIES, STR, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, STR, NONE)>(op->fn)(
                in[0], g[1].str.ptr, g[1].str.len);
        case DFTU_OP_SIG(SERIES, SERIES, STR, I32):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, STR, I32)>(op->fn)(
                in[0], g[1].str.ptr, g[1].str.len, g[2].i32);
        case DFTU_OP_SIG(SERIES, SERIES, I32, I32):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I32, I32)>(op->fn)(
                in[0], g[1].i32, g[2].i32);
        case DFTU_OP_SIG(SERIES, SERIES, I64, I32):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, I32)>(op->fn)(
                in[0], g[1].i64, g[2].i32);
        case DFTU_OP_SIG(SERIES, SERIES, STR, STR):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, STR, STR)>(op->fn)(
                in[0], g[1].str.ptr, g[1].str.len, g[2].str.ptr, g[2].str.len);
        case DFTU_OP_SIG(SERIES, SERIES, STR, I64):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, STR, I64)>(op->fn)(
                in[0], g[1].str.ptr, g[1].str.len, g[2].i64);
        case DFTU_OP_SIG(SERIES, SERIES, I64, I64):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, I64)>(op->fn)(
                in[0], g[1].i64, g[2].i64);
        case DFTU_OP_SIG(SERIES, SERIES, I64, CHAR):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, CHAR)>(op->fn)(
                in[0], g[1].i64, g[2].ch);
        case DFTU_OP_SIG(SERIES, SERIES, I64, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, NONE)>(op->fn)(
                in[0], g[1].i64);
        case DFTU_OP_SIG(SERIES, SERIES, SCALAR, SCALAR):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, SCALAR, SCALAR)>(op->fn)(
                in[0], g[1].scalar, g[2].scalar);
        case DFTU_OP_SIG(SERIES, SERIES, I32, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I32, NONE)>(op->fn)(
                in[0], g[1].i32);
        case DFTU_OP_SIG(SERIES, SERIES, F64, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, F64, NONE)>(op->fn)(
                in[0], g[1].f64);
        case DFTU_OP_SIG(SERIES, SERIES, I64, F64):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, F64)>(op->fn)(
                in[0], g[1].i64, g[2].f64);
        case DFTU_OP_SIG(SERIES, SERIES, I64, ROLLING):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, ROLLING)>(op->fn)(
                in[0], g[1].i64, static_cast<dftu_rolling_op>(g[2].i32));
        // Source ops: no column operand, so the whole column is built from
        // the string operands (a directory listing, say).
        case DFTU_OP_SIG(SERIES, STR, NONE, NONE):
            return as_op<DFTU_OP_SIG(SERIES, STR, NONE, NONE)>(op->fn)(
                g[0].str.ptr, g[0].str.len);
        case DFTU_OP_SIG(SERIES, STR, STR, NONE):
            return as_op<DFTU_OP_SIG(SERIES, STR, STR, NONE)>(op->fn)(
                g[0].str.ptr, g[0].str.len, g[1].str.ptr, g[1].str.len);
        case DFTU_OP_SIG(SERIES, SERIES, RANK, I64):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, RANK, I64)>(op->fn)(
                in[0], static_cast<dftu_rank_method>(g[1].i32),
                static_cast<int32_t>(g[2].i64));
        case DFTU_OP_SIG(SERIES, SERIES, I64, U64):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64, U64)>(op->fn)(
                in[0], g[1].i64, g[2].u64);
        case DFTU_OP_SIG(SERIES, SERIES, I64LIST, NONE):
            return as_op<DFTU_OP_SIG(SERIES, SERIES, I64LIST, NONE)>(op->fn)(
                in[0], g[1].i64list.items, g[1].i64list.n);
        // A frame-shaped SERIES-return op: the frame rides args[0].frame, not
        // in[] (n == 0, see dftu_op_arity).
        case DFTU_OP_SIG(SERIES, FRAME, NONE, NONE):
            if (!g[0].frame) return nullptr;
            return as_op<DFTU_OP_SIG(SERIES, FRAME, NONE, NONE)>(op->fn)(
                g[0].frame);
        case DFTU_OP_SIG(SERIES, FRAME, QUERY, NONE):
            if (!g[0].frame || !g[1].query) return nullptr;
            return as_op<DFTU_OP_SIG(SERIES, FRAME, QUERY, NONE)>(op->fn)(
                g[0].frame, g[1].query);
        case DFTU_OP_SIG(SERIES, FRAME, STRLIST, I64):
            if (!g[0].frame) return nullptr;
            return as_op<DFTU_OP_SIG(SERIES, FRAME, STRLIST, I64)>(op->fn)(
                g[0].frame, g[1].list.items, g[1].list.n, g[2].i64);
        default:
            return nullptr;
    }
}

dftu_scalar dftu_op_run_aggregate(const dftu_op_desc* op, const dftu_series* v,
                                  const dftu_op_arg* a, int* ok) {
    dftu_scalar z;
    z.kind = DFTU_SCALAR_TAG_I64;
    z.value.i = 0;
    if (!op || !op->fn || dftu_op_kind_of(op->sig) != DFTU_OP_KIND_AGGREGATE) {
        if (ok) *ok = 0;
        return z;
    }
    if (needs_arg(op->sig) && !a) {
        if (ok) *ok = 0;
        return z;
    }
    if (ok) *ok = 1;
    const dftu_op_val* g = a ? a->args : nullptr;
    switch (op->sig) {
        case DFTU_OP_SIG(SCALAR, SERIES, NONE, NONE):
            return as_op<DFTU_OP_SIG(SCALAR, SERIES, NONE, NONE)>(op->fn)(v);
        case DFTU_OP_SIG(SCALAR, SERIES, REDUCE, NONE):
            return as_op<DFTU_OP_SIG(SCALAR, SERIES, REDUCE, NONE)>(op->fn)(
                v, static_cast<dftu_reduce_op>(g[1].i32));
        case DFTU_OP_SIG(SCALAR, SERIES, SERIES, NONE):
            if (!g || !g[1].series) {
                if (ok) *ok = 0;
                return z;
            }
            return as_op<DFTU_OP_SIG(SCALAR, SERIES, SERIES, NONE)>(op->fn)(
                v, g[1].series);
        case DFTU_OP_SIG(I64, SERIES, NONE, NONE):
            z.value.i = as_op<DFTU_OP_SIG(I64, SERIES, NONE, NONE)>(op->fn)(v);
            return z;
        case DFTU_OP_SIG(BOOL, SERIES, NONE, NONE):
            z.value.i = as_op<DFTU_OP_SIG(BOOL, SERIES, NONE, NONE)>(op->fn)(v);
            return z;
        case DFTU_OP_SIG(BOOL, SERIES, I32, NONE):
            z.value.i = as_op<DFTU_OP_SIG(BOOL, SERIES, I32, NONE)>(op->fn)(
                v, g[1].i32);
            return z;
        case DFTU_OP_SIG(F64, SERIES, NONE, NONE):
            z.kind = DFTU_SCALAR_TAG_F64;
            z.value.d = as_op<DFTU_OP_SIG(F64, SERIES, NONE, NONE)>(op->fn)(v);
            return z;
        case DFTU_OP_SIG(F64, SERIES, I32, NONE):
            z.kind = DFTU_SCALAR_TAG_F64;
            z.value.d =
                as_op<DFTU_OP_SIG(F64, SERIES, I32, NONE)>(op->fn)(v, g[1].i32);
            return z;
        // No column operand: an effectful op reporting success (a file
        // compression, say). `v` is unused.
        case DFTU_OP_SIG(BOOL, STR, STR, NONE):
            z.value.i = as_op<DFTU_OP_SIG(BOOL, STR, STR, NONE)>(op->fn)(
                g[0].str.ptr, g[0].str.len, g[1].str.ptr, g[1].str.len);
            return z;
        case DFTU_OP_SIG(F64, SERIES, F64, NONE):
            z.kind = DFTU_SCALAR_TAG_F64;
            z.value.d =
                as_op<DFTU_OP_SIG(F64, SERIES, F64, NONE)>(op->fn)(v, g[1].f64);
            return z;
        default:
            if (ok) *ok = 0;
            return z;
    }
}

dftu_dataframe* dftu_op_run_frame(const dftu_op_desc* op,
                                  const dftu_dataframe* const* frames,
                                  uint32_t n, const dftu_op_arg* a) {
    if (!op || !op->fn) return nullptr;
    if (dftu_op_kind_of(op->sig) != DFTU_OP_KIND_FRAME) return nullptr;
    if (n != dftu_op_arity(op->sig)) return nullptr;
    using CDF = const dftu_dataframe*;
    const dftu_op_val* g = a ? a->args : nullptr;
    CDF df = (n >= 1 && frames) ? frames[0] : nullptr;
    switch (op->sig) {
        case DFTU_OP_SIG(FRAME, FRAME, NONE, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, NONE, NONE)>(op->fn)(df);
        case DFTU_OP_SIG(FRAME, FRAME, I64, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, I64, NONE)>(op->fn)(
                df, g[1].i64);
        case DFTU_OP_SIG(FRAME, FRAME, I64, I64):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, I64, I64)>(op->fn)(
                df, g[1].i64, g[2].i64);
        case DFTU_OP_SIG(FRAME, FRAME, SCALAR, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, SCALAR, NONE)>(op->fn)(
                df, g[1].scalar);
        case DFTU_OP_SIG(FRAME, FRAME, SERIES, NONE):
            if (!g[1].series) return nullptr;
            return as_op<DFTU_OP_SIG(FRAME, FRAME, SERIES, NONE)>(op->fn)(
                df, g[1].series);
        case DFTU_OP_SIG(FRAME, FRAME, STR, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STR, NONE)>(op->fn)(
                df, g[1].str.ptr);
        case DFTU_OP_SIG(FRAME, FRAME, STR, I32):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STR, I32)>(op->fn)(
                df, g[1].str.ptr, g[2].i32);
        case DFTU_OP_SIG(FRAME, FRAME, I32, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, I32, NONE)>(op->fn)(
                df, g[1].i32);
        case DFTU_OP_SIG(FRAME, FRAME, STR, SERIES):
            if (!g[2].series) return nullptr;
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STR, SERIES)>(op->fn)(
                df, g[1].str.ptr, g[2].series);
        case DFTU_OP_SIG(FRAME, FRAME, STRLIST, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STRLIST, NONE)>(op->fn)(
                df, g[1].list.items, g[1].list.n);
        case DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].i32);
        case DFTU_OP_SIG(FRAME, FRAME, STRLIST, STRLIST):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STRLIST, STRLIST)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].list.items, g[2].list.n);
        case DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32LIST):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STRLIST, I32LIST)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].i32list.items,
                g[2].i32list.n);
        case DFTU_OP_SIG(FRAME, FRAME, STRLIST, AGGLIST):
            if (!g[2].agglist.items) return nullptr;
            return as_op<DFTU_OP_SIG(FRAME, FRAME, STRLIST, AGGLIST)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].agglist.items,
                g[2].agglist.n);
        case DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I32, NONE):
            return as_op<DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I32, NONE)>(
                op->fn)(df, g[1].str.ptr, g[2].i64, g[3].i32);
        case DFTU_OP_SIG6(FRAME, FRAME, STR, STR, STR, STR):
            return as_op<DFTU_OP_SIG6(FRAME, FRAME, STR, STR, STR, STR)>(
                op->fn)(df, g[1].str.ptr, g[2].str.ptr, g[3].str.ptr,
                        g[4].str.ptr);
        case DFTU_OP_SIG(FRAME, SERIES, NONE,
                         NONE):  // value_counts: series -> frame
            if (!g[0].series) return nullptr;
            return as_op<DFTU_OP_SIG(FRAME, SERIES, NONE, NONE)>(op->fn)(
                g[0].series);
        case DFTU_OP_SIG6(FRAME, FRAME, I64LIST, NONE, NONE, NONE):
            return as_op<DFTU_OP_SIG6(FRAME, FRAME, I64LIST, NONE, NONE, NONE)>(
                op->fn)(df, g[1].i64list.items, g[1].i64list.n);
        case DFTU_OP_SIG6(FRAME, FRAME, I64, U64, NONE, NONE):
            return as_op<DFTU_OP_SIG6(FRAME, FRAME, I64, U64, NONE, NONE)>(
                op->fn)(df, g[1].i64, g[2].u64);
        case DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I64, AGGLIST):
            if (!g[4].agglist.items) return nullptr;
            return as_op<DFTU_OP_SIG6(FRAME, FRAME, STR, I64, I64, AGGLIST)>(
                op->fn)(df, g[1].str.ptr, g[2].i64, g[3].i64,
                        g[4].agglist.items, g[4].agglist.n);
        case DFTU_OP_SIG8(FRAME, FRAME, STRLIST, I32, I64, I32, I32, NONE):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, STRLIST, I32, I64, I32, I32,
                                      NONE)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].i32, g[3].i64, g[4].i32,
                g[5].i32);
        case DFTU_OP_SIG8(FRAME, FRAME, STR, STR, STR, SCALAR, SCALAR, STR):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, STR, STR, STR, SCALAR,
                                      SCALAR, STR)>(op->fn)(
                df, g[1].str.ptr, g[2].str.ptr, g[3].str.ptr, g[4].scalar,
                g[5].scalar, g[6].str.ptr);
        case DFTU_OP_SIG(FRAME, FRAME, FRAME, NONE):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, FRAME, NONE)>(op->fn)(
                df, frames[1]);
        case DFTU_OP_SIG(FRAME, FRAME, FRAME, I32):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, FRAME, I32)>(op->fn)(
                df, frames[1], static_cast<dftu_concat_how>(g[2].i32));
        case DFTU_OP_SIG(FRAME, FRAME, FRAME, I64):
            return as_op<DFTU_OP_SIG(FRAME, FRAME, FRAME, I64)>(op->fn)(
                df, frames[1], g[2].i64);
        case DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STRLIST, WINLIST, NONE, NONE,
                          NONE):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STRLIST, WINLIST,
                                      NONE, NONE, NONE)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].list.items, g[2].list.n,
                g[3].winlist.items, g[3].winlist.n);
        case DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STR, I64, STRLIST, I32,
                          I64LIST):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, STRLIST, STR, I64, STRLIST,
                                      I32, I64LIST)>(op->fn)(
                df, g[1].list.items, g[1].list.n, g[2].str.ptr, g[3].i64,
                g[4].list.items, g[4].list.n,
                static_cast<dftu_gap_fill_mode>(g[5].i32), g[6].i64list.items,
                g[6].i64list.n);
        case DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STRLIST, I32, I64, NONE):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STRLIST, I32,
                                      I64, NONE)>(op->fn)(
                df, frames[1], g[2].str.ptr, g[3].list.items, g[3].list.n,
                static_cast<dftu_asof_direction>(g[4].i32), g[5].i64);
        case DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STR, STR, STRLIST, I32):
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, FRAME, STR, STR, STR,
                                      STRLIST, I32)>(op->fn)(
                df, frames[1], g[2].str.ptr, g[3].str.ptr, g[4].str.ptr,
                g[5].list.items, g[5].list.n, g[6].i32);
        case DFTU_OP_SIG8(FRAME, FRAME, FRAME, STRLIST, STRLIST, I32, STR,
                          NONE):
            if (g[2].list.n != g[3].list.n) return nullptr;
            return as_op<DFTU_OP_SIG8(FRAME, FRAME, FRAME, STRLIST, STRLIST,
                                      I32, STR, NONE)>(op->fn)(
                df, frames[1], g[2].list.items, g[3].list.items, g[2].list.n,
                static_cast<dftu_join_how>(g[4].i32), g[5].str.ptr);
        default:
            return nullptr;
    }
}

dftu_lazyframe* dftu_op_run_lazy(const dftu_op_desc* op,
                                 const dftu_lazyframe* const* in, uint32_t n,
                                 const dftu_op_arg* a) {
    if (!op || !op->fn) return nullptr;
    if (dftu_op_kind_of(op->sig) != DFTU_OP_KIND_LAZY) return nullptr;
    if (n != dftu_op_arity(op->sig)) return nullptr;
    using CLF = const dftu_lazyframe*;
    const dftu_op_val* g = a ? a->args : nullptr;
    CLF lf = (n >= 1 && in) ? in[0] : nullptr;
    switch (op->sig) {
        case DFTU_OP_SIG(LAZY, LAZY, NONE, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, NONE, NONE)>(op->fn)(lf);
        case DFTU_OP_SIG(LAZY, LAZY, STR, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STR, NONE)>(op->fn)(
                lf, g[1].str.ptr);
        case DFTU_OP_SIG(LAZY, LAZY, SCALAR, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, SCALAR, NONE)>(op->fn)(
                lf, g[1].scalar);
        case DFTU_OP_SIG(LAZY, LAZY, EXPR, NONE):
            if (!g[1].expr) return nullptr;
            return as_op<DFTU_OP_SIG(LAZY, LAZY, EXPR, NONE)>(op->fn)(
                lf, g[1].expr);
        case DFTU_OP_SIG(LAZY, LAZY, STR, EXPR):
            if (!g[2].expr) return nullptr;
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STR, EXPR)>(op->fn)(
                lf, g[1].str.ptr, g[2].expr);
        case DFTU_OP_SIG(LAZY, LAZY, I64, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, I64, NONE)>(op->fn)(lf,
                                                                     g[1].i64);
        case DFTU_OP_SIG(LAZY, LAZY, I64, I64):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, I64, I64)>(op->fn)(
                lf, g[1].i64, g[2].i64);
        case DFTU_OP_SIG(LAZY, LAZY, U64, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, U64, NONE)>(op->fn)(in[0],
                                                                     g[1].u64);
        case DFTU_OP_SIG(LAZY, LAZY, I64, U64):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, I64, U64)>(op->fn)(
                in[0], g[1].i64, g[2].u64);
        case DFTU_OP_SIG(LAZY, LAZY, STRLIST, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STRLIST, NONE)>(op->fn)(
                lf, g[1].list.items, g[1].list.n);
        case DFTU_OP_SIG(LAZY, LAZY, STRLIST, STRLIST):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STRLIST, STRLIST)>(op->fn)(
                lf, g[1].list.items, g[1].list.n, g[2].list.items, g[2].list.n);
        case DFTU_OP_SIG(LAZY, LAZY, STRLIST, AGGLIST):
            if (!g[2].agglist.items) return nullptr;
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STRLIST, AGGLIST)>(op->fn)(
                lf, g[1].list.items, g[1].list.n, g[2].agglist.items,
                g[2].agglist.n);
        case DFTU_OP_SIG(LAZY, LAZY, STR, I32):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STR, I32)>(op->fn)(
                lf, g[1].str.ptr, g[2].i32);
        case DFTU_OP_SIG(LAZY, LAZY, SERIES, NONE):
            if (!g[1].series) return nullptr;
            return as_op<DFTU_OP_SIG(LAZY, LAZY, SERIES, NONE)>(op->fn)(
                lf, g[1].series);
        case DFTU_OP_SIG(LAZY, LAZY, STRLIST, I32):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, STRLIST, I32)>(op->fn)(
                lf, g[1].list.items, g[1].list.n, g[2].i32);
        case DFTU_OP_SIG(LAZY, LAZY, I32, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, I32, NONE)>(op->fn)(lf,
                                                                     g[1].i32);
        case DFTU_OP_SIG6(LAZY, LAZY, STR, I64, I32, NONE):
            return as_op<DFTU_OP_SIG6(LAZY, LAZY, STR, I64, I32, NONE)>(op->fn)(
                lf, g[1].str.ptr, g[2].i64, g[3].i32);
        case DFTU_OP_SIG6(LAZY, LAZY, STR, STR, STR, STR):
            return as_op<DFTU_OP_SIG6(LAZY, LAZY, STR, STR, STR, STR)>(op->fn)(
                lf, g[1].str.ptr, g[2].str.ptr, g[3].str.ptr, g[4].str.ptr);
        case DFTU_OP_SIG(LAZY, LAZY, I64LIST, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, I64LIST, NONE)>(op->fn)(
                lf, g[1].i64list.items, g[1].i64list.n);
        case DFTU_OP_SIG8(LAZY, LAZY, STR, I64, I64, AGGLIST, I64, I32):
            if (!g[4].agglist.items) return nullptr;
            return as_op<DFTU_OP_SIG8(LAZY, LAZY, STR, I64, I64, AGGLIST, I64,
                                      I32)>(op->fn)(
                lf, g[1].str.ptr, g[2].i64, g[3].i64, g[4].agglist.items,
                g[4].agglist.n, g[5].i64, g[6].i32);
        case DFTU_OP_SIG8(LAZY, LAZY, STRLIST, I32, I64, I32, I32, NONE):
            return as_op<DFTU_OP_SIG8(LAZY, LAZY, STRLIST, I32, I64, I32, I32,
                                      NONE)>(op->fn)(
                lf, g[1].list.items, g[1].list.n, g[2].i32, g[3].i64, g[4].i32,
                g[5].i32);
        case DFTU_OP_SIG8(LAZY, LAZY, LAZY, STRLIST, STRLIST, I32, STR, NONE):
            if (g[2].list.n != g[3].list.n) return nullptr;
            return as_op<DFTU_OP_SIG8(LAZY, LAZY, LAZY, STRLIST, STRLIST, I32,
                                      STR, NONE)>(op->fn)(
                lf, in[1], g[2].list.items, g[3].list.items, g[2].list.n,
                static_cast<dftu_join_how>(g[4].i32), g[5].str.ptr);
        case DFTU_OP_SIG(LAZY, LAZY, LAZY, NONE):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, LAZY, NONE)>(op->fn)(lf,
                                                                      in[1]);
        case DFTU_OP_SIG(LAZY, LAZY, LAZY, I64):
            return as_op<DFTU_OP_SIG(LAZY, LAZY, LAZY, I64)>(op->fn)(lf, in[1],
                                                                     g[2].i64);
        default:
            return nullptr;
    }
}

}  // extern "C"
