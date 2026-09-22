#ifndef DFTRACER_UTILS_DATAFRAME_OP_H
#define DFTRACER_UTILS_DATAFRAME_OP_H

#include <dftracer/utils/dataframe/abi.h>
#include <dftracer/utils/dataframe/scalar.h>
#include <dftracer/utils/dataframe/types.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace dftracer::utils::dataframe {

/// Mirrors dftu_op_kind.
enum class OpKind : std::int32_t {
    Series = DFTU_OP_KIND_SERIES,
    Aggregate = DFTU_OP_KIND_AGGREGATE,
    Frame = DFTU_OP_KIND_FRAME,
    Lazy = DFTU_OP_KIND_LAZY,
};
static_assert(static_cast<int>(OpKind::Series) == DFTU_OP_KIND_SERIES);
static_assert(static_cast<int>(OpKind::Aggregate) == DFTU_OP_KIND_AGGREGATE);
static_assert(static_cast<int>(OpKind::Frame) == DFTU_OP_KIND_FRAME);
static_assert(static_cast<int>(OpKind::Lazy) == DFTU_OP_KIND_LAZY);

/// Mirrors dftu_op_tok.
enum class OpTok : std::int32_t {
    None = DFTU_TOK_NONE,
    Series = DFTU_TOK_SERIES,
    Scalar = DFTU_TOK_SCALAR,
    I64 = DFTU_TOK_I64,
    Bool = DFTU_TOK_BOOL,
    Cmp = DFTU_TOK_CMP,
    Prim = DFTU_TOK_PRIM,
    Logical = DFTU_TOK_LOGICAL,
    Dtype = DFTU_TOK_DTYPE,
    Reduce = DFTU_TOK_REDUCE,
    Str = DFTU_TOK_STR,
    Char = DFTU_TOK_CHAR,
    F64 = DFTU_TOK_F64,
    I32 = DFTU_TOK_I32,
    Rank = DFTU_TOK_RANK,
    Rolling = DFTU_TOK_ROLLING,
    Frame = DFTU_TOK_FRAME,
    StrList = DFTU_TOK_STRLIST,
    I32List = DFTU_TOK_I32LIST,
    Lazy = DFTU_TOK_LAZY,
    Expr = DFTU_TOK_EXPR,
    AggList = DFTU_TOK_AGGLIST,
    U64 = DFTU_TOK_U64,
    I64List = DFTU_TOK_I64LIST,
    Query = DFTU_TOK_QUERY,
    WinList = DFTU_TOK_WINLIST,
};
static_assert(static_cast<int>(OpTok::None) == DFTU_TOK_NONE);
static_assert(static_cast<int>(OpTok::Series) == DFTU_TOK_SERIES);
static_assert(static_cast<int>(OpTok::Scalar) == DFTU_TOK_SCALAR);
static_assert(static_cast<int>(OpTok::I64) == DFTU_TOK_I64);
static_assert(static_cast<int>(OpTok::Bool) == DFTU_TOK_BOOL);
static_assert(static_cast<int>(OpTok::Cmp) == DFTU_TOK_CMP);
static_assert(static_cast<int>(OpTok::Prim) == DFTU_TOK_PRIM);
static_assert(static_cast<int>(OpTok::Logical) == DFTU_TOK_LOGICAL);
static_assert(static_cast<int>(OpTok::Dtype) == DFTU_TOK_DTYPE);
static_assert(static_cast<int>(OpTok::Reduce) == DFTU_TOK_REDUCE);
static_assert(static_cast<int>(OpTok::Str) == DFTU_TOK_STR);
static_assert(static_cast<int>(OpTok::Char) == DFTU_TOK_CHAR);
static_assert(static_cast<int>(OpTok::F64) == DFTU_TOK_F64);
static_assert(static_cast<int>(OpTok::I32) == DFTU_TOK_I32);
static_assert(static_cast<int>(OpTok::Rank) == DFTU_TOK_RANK);
static_assert(static_cast<int>(OpTok::Rolling) == DFTU_TOK_ROLLING);
static_assert(static_cast<int>(OpTok::Frame) == DFTU_TOK_FRAME);
static_assert(static_cast<int>(OpTok::StrList) == DFTU_TOK_STRLIST);
static_assert(static_cast<int>(OpTok::I32List) == DFTU_TOK_I32LIST);
static_assert(static_cast<int>(OpTok::Lazy) == DFTU_TOK_LAZY);
static_assert(static_cast<int>(OpTok::Expr) == DFTU_TOK_EXPR);
static_assert(static_cast<int>(OpTok::AggList) == DFTU_TOK_AGGLIST);
static_assert(static_cast<int>(OpTok::U64) == DFTU_TOK_U64);
static_assert(static_cast<int>(OpTok::I64List) == DFTU_TOK_I64LIST);
static_assert(static_cast<int>(OpTok::Query) == DFTU_TOK_QUERY);
static_assert(static_cast<int>(OpTok::WinList) == DFTU_TOK_WINLIST);

/// Value wrapper over a packed dftu_op_sig.
class OpSig {
   public:
    OpSig() noexcept = default;
    OpSig(dftu_op_sig sig) noexcept : sig_(sig) {}

    dftu_op_sig raw() const noexcept { return sig_; }
    OpKind kind() const noexcept {
        return static_cast<OpKind>(dftu_op_kind_of(sig_));
    }
    std::uint32_t arity() const noexcept { return dftu_op_arity(sig_); }
    OpTok ret() const noexcept {
        return static_cast<OpTok>(DFTU_OP_SIG_RET(sig_));
    }
    OpTok arg(std::uint32_t i) const noexcept {
        return static_cast<OpTok>(DFTU_OP_SIG_ARG(sig_, i));
    }

    /// A human-readable spelling of the signature. Backed by static storage
    /// (dftu_op_signature), so the returned view stays valid indefinitely.
    std::string_view to_string() const noexcept {
        return dftu_op_signature(sig_);
    }

    friend bool operator==(const OpSig& a, const OpSig& b) noexcept {
        return a.sig_ == b.sig_;
    }
    friend bool operator!=(const OpSig& a, const OpSig& b) noexcept {
        return !(a == b);
    }

   private:
    dftu_op_sig sig_ = 0;
};

/// Non-owning view over a registered op. Valid for the registry's lifetime
/// (built-ins are permanent; a user op stays registered for the process).
class OpDesc {
   public:
    OpDesc() noexcept = default;
    OpDesc(const dftu_op_desc* desc) noexcept : desc_(desc) {}

    std::string_view name() const noexcept { return desc_->name; }
    OpSig sig() const noexcept { return OpSig(desc_->sig); }
    const void* fn() const noexcept { return desc_->fn; }

    explicit operator bool() const noexcept { return desc_ != nullptr; }

   private:
    const dftu_op_desc* desc_ = nullptr;
};

/// Looks up a registered op by name (built-in or user). Empty if none.
inline std::optional<OpDesc> find_op(const char* name) noexcept {
    const dftu_op_desc* d = dftu_op_find(name);
    if (d == nullptr) return std::nullopt;
    return OpDesc(d);
}

/// Number of registered ops (built-ins + user), for discovery/listing.
inline std::uint32_t op_count() noexcept { return dftu_op_count(); }

/// The op at index `i` in [0, op_count()); an empty OpDesc if out of range.
inline OpDesc op_at(std::uint32_t i) noexcept { return OpDesc(dftu_op_at(i)); }

/// Builder for a dftu_op_arg. Slots are indexed by operand-TOKEN position (a
/// series op's column operands ride the runner's separate `in[]` array, so
/// those slots are left unset). str()/strlist() write borrowed views: the
/// backing string/array must outlive the dftu_op_run* call the args are passed
/// to.
class OpArgs {
   public:
    OpArgs() noexcept : arg_{} {}
    /// Copies `raw` verbatim, including any pointer-bearing slot it carries -
    /// the borrow contract for that slot is unchanged by wrapping it here.
    explicit OpArgs(const dftu_op_arg& raw) noexcept : arg_(raw) {}

    OpArgs& scalar(std::uint32_t i, Scalar s) noexcept {
        arg_.args[i].scalar = s;
        return *this;
    }
    OpArgs& i64(std::uint32_t i, std::int64_t v) noexcept {
        arg_.args[i].i64 = v;
        return *this;
    }
    OpArgs& u64(std::uint32_t i, std::uint64_t v) noexcept {
        arg_.args[i].u64 = v;
        return *this;
    }
    OpArgs& f64(std::uint32_t i, double v) noexcept {
        arg_.args[i].f64 = v;
        return *this;
    }
    OpArgs& i32(std::uint32_t i, std::int32_t v) noexcept {
        arg_.args[i].i32 = v;
        return *this;
    }
    OpArgs& i32(std::uint32_t i, CmpOp v) noexcept {
        return i32(i, static_cast<std::int32_t>(v));
    }
    OpArgs& i32(std::uint32_t i, PrimOp v) noexcept {
        return i32(i, static_cast<std::int32_t>(v));
    }
    OpArgs& i32(std::uint32_t i, LogicalOp v) noexcept {
        return i32(i, static_cast<std::int32_t>(v));
    }
    OpArgs& i32(std::uint32_t i, RankMethod v) noexcept {
        return i32(i, static_cast<std::int32_t>(v));
    }
    OpArgs& i32(std::uint32_t i, RollingOp v) noexcept {
        return i32(i, static_cast<std::int32_t>(v));
    }
    OpArgs& ch(std::uint32_t i, char v) noexcept {
        arg_.args[i].ch = v;
        return *this;
    }
    OpArgs& series(std::uint32_t i, const dftu_series* v) noexcept {
        arg_.args[i].series = v;
        return *this;
    }
    OpArgs& frame(std::uint32_t i, const dftu_dataframe* v) noexcept {
        arg_.args[i].frame = v;
        return *this;
    }
    /// Borrows `q`: `q` must outlive the dftu_op_run* call this is passed to.
    OpArgs& query(std::uint32_t i, const dftu_query* q) noexcept {
        arg_.args[i].query = q;
        return *this;
    }
    OpArgs& lazy(std::uint32_t i, const dftu_lazyframe* v) noexcept {
        arg_.args[i].lazy = v;
        return *this;
    }
    /// Borrows `e`: `e` must outlive the dftu_op_run* call this is passed to.
    OpArgs& expr(std::uint32_t i, const dftu_expr* e) noexcept {
        arg_.args[i].expr = e;
        return *this;
    }
    /// Borrows `items`: it must outlive the dftu_op_run* call this is passed
    /// to.
    OpArgs& agglist(std::uint32_t i,
                    std::span<const dftu_group_agg> items) noexcept {
        arg_.args[i].agglist.items = items.data();
        arg_.args[i].agglist.n = static_cast<std::int32_t>(items.size());
        return *this;
    }
    /// Borrows `s`: `s` must outlive the dftu_op_run* call this is passed to.
    OpArgs& str(std::uint32_t i, std::string_view s) noexcept {
        arg_.args[i].str.ptr = s.data();
        arg_.args[i].str.len = static_cast<std::int32_t>(s.size());
        return *this;
    }
    /// Borrows `items`: it must outlive the dftu_op_run* call this is passed
    /// to.
    OpArgs& strlist(std::uint32_t i, const char* const* items,
                    std::int32_t n) noexcept {
        arg_.args[i].list.items = items;
        arg_.args[i].list.n = n;
        return *this;
    }
    /// Borrows `items`: it must outlive the dftu_op_run* call this is passed
    /// to.
    OpArgs& i32list(std::uint32_t i,
                    std::span<const std::int32_t> items) noexcept {
        arg_.args[i].i32list.items = items.data();
        arg_.args[i].i32list.n = static_cast<std::int32_t>(items.size());
        return *this;
    }
    /// Borrows `items`: it must outlive the dftu_op_run* call this is passed
    /// to.
    OpArgs& i64list(std::uint32_t i,
                    std::span<const std::int64_t> items) noexcept {
        arg_.args[i].i64list.items = items.data();
        arg_.args[i].i64list.n = static_cast<std::int32_t>(items.size());
        return *this;
    }
    /// Borrows `items`: it must outlive the dftu_op_run* call this is passed
    /// to.
    OpArgs& winlist(std::uint32_t i,
                    std::span<const dftu_window_spec> items) noexcept {
        arg_.args[i].winlist.items = items.data();
        arg_.args[i].winlist.n = static_cast<std::int32_t>(items.size());
        return *this;
    }

    const dftu_op_arg& raw() const noexcept { return arg_; }
    operator const dftu_op_arg*() const noexcept { return &arg_; }

   private:
    dftu_op_arg arg_;
};

}  // namespace dftracer::utils::dataframe

#endif  // DFTRACER_UTILS_DATAFRAME_OP_H
