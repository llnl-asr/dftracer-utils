#ifndef DFTRACER_UTILS_DATAFRAME_LAZY_FRAME_OP_H
#define DFTRACER_UTILS_DATAFRAME_LAZY_FRAME_OP_H

#include <dftracer/utils/dataframe/lazy/common.h>

#include <algorithm>
#include <iterator>
#include <string_view>

namespace dftracer::utils::dataframe::lazy_internal {

// The column names a window spec's union holds for its function.
inline const char* window_time(const dftu_window_spec& w) {
    if (w.func == DFTU_WINDOW_RATE) return w.param.rate.time;
    if (w.func == DFTU_WINDOW_SESSIONIZE) return w.param.session.time;
    return nullptr;
}

inline const char* window_end(const dftu_window_spec& w) {
    if (w.func == DFTU_WINDOW_FRAME_ARG_MAX ||
        w.func == DFTU_WINDOW_FRAME_ARG_MIN)
        return w.param.frame.by;
    return w.func == DFTU_WINDOW_SESSIONIZE ? w.param.session.end : nullptr;
}

// The operands of a registry frame op, deep-copied so the plan owns them for
// as long as it lives: a LazyOp is shared and re-run, so it cannot borrow the
// caller's strings and lists the way a one-shot dftu_op_run_frame call does.
// A FRAME operand after the primary one is a whole LazyFrame, collected when
// the op runs; SERIES shares the column; EXPR / DUQL / LAZY operands are
// refused (no owned form).
class OwnedFrameOpArgs {
   public:
    OwnedFrameOpArgs(const dftu_op_desc& op, const OpArgs& args,
                     std::vector<LazyFrame> frames)
        : frames_(std::move(frames)) {
        const dftu_op_arg& raw = args.raw();
        std::size_t next_frame = 0;
        for (int i = 0; i < DFTU_OP_MAX_ARGS; ++i) {
            const dftu_op_tok t = DFTU_OP_SIG_ARG(op.sig, i);
            const dftu_op_val& v = raw.args[i];
            Slot s;
            s.tok = t;
            switch (t) {
                case DFTU_TOK_NONE:
                    break;
                case DFTU_TOK_FRAME:
                    if (i == 0) break;
                    if (next_frame >= frames_.size())
                        throw std::invalid_argument(
                            std::string("lazy frame op '") + op.name +
                            "': operand " + std::to_string(i) +
                            " is a frame but no plan was given for it");
                    s.frame_index = next_frame++;
                    break;
                case DFTU_TOK_SERIES:
                    if (!v.series)
                        throw std::invalid_argument(
                            std::string("lazy frame op '") + op.name +
                            "': operand " + std::to_string(i) +
                            " is a column but none was given");
                    s.series = Series{dftu_series_share(v.series)};
                    break;
                case DFTU_TOK_SCALAR:
                    s.val.scalar = v.scalar;
                    break;
                case DFTU_TOK_I64:
                    s.val.i64 = v.i64;
                    break;
                case DFTU_TOK_U64:
                    s.val.u64 = v.u64;
                    break;
                case DFTU_TOK_F64:
                    s.val.f64 = v.f64;
                    break;
                case DFTU_TOK_CHAR:
                    s.val.ch = v.ch;
                    break;
                case DFTU_TOK_BOOL:
                case DFTU_TOK_CMP:
                case DFTU_TOK_PRIM:
                case DFTU_TOK_LOGICAL:
                case DFTU_TOK_DTYPE:
                case DFTU_TOK_REDUCE:
                case DFTU_TOK_I32:
                case DFTU_TOK_RANK:
                case DFTU_TOK_ROLLING:
                    s.val.i32 = v.i32;
                    break;
                case DFTU_TOK_STR:
                    s.strings.emplace_back(v.str.ptr ? std::string(v.str.ptr)
                                                     : std::string());
                    break;
                case DFTU_TOK_STRLIST:
                    for (int32_t k = 0; k < v.list.n; ++k)
                        s.strings.emplace_back(v.list.items[k]);
                    break;
                case DFTU_TOK_I32LIST:
                    s.i32s.assign(v.i32list.items,
                                  v.i32list.items + v.i32list.n);
                    break;
                case DFTU_TOK_I64LIST:
                    s.i64s.assign(v.i64list.items,
                                  v.i64list.items + v.i64list.n);
                    break;
                case DFTU_TOK_AGGLIST:
                    for (int32_t k = 0; k < v.agglist.n; ++k) {
                        const dftu_group_agg& a = v.agglist.items[k];
                        s.strings.emplace_back(a.op ? a.op : "");
                        s.strings.emplace_back(a.column ? a.column : "");
                        s.strings.emplace_back(a.out ? a.out : "");
                        s.aggs.push_back(a);
                    }
                    break;
                case DFTU_TOK_WINLIST:
                    for (int32_t k = 0; k < v.winlist.n; ++k) {
                        const dftu_window_spec& w = v.winlist.items[k];
                        s.strings.emplace_back(w.value ? w.value : "");
                        s.has_value.push_back(w.value != nullptr);
                        const char* time = window_time(w);
                        const char* end = window_end(w);
                        s.strings.emplace_back(time ? time : "");
                        s.has_time.push_back(time != nullptr);
                        s.strings.emplace_back(w.out ? w.out : "");
                        s.strings.emplace_back(end ? end : "");
                        s.has_end.push_back(end != nullptr);
                        s.wins.push_back(w);
                    }
                    break;
                case DFTU_TOK_EXPR:
                case DFTU_TOK_DUQL:
                case DFTU_TOK_LAZY:
                    throw std::invalid_argument(std::string("lazy frame op '") +
                                                op.name + "': operand " +
                                                std::to_string(i) +
                                                " has no owned form in a plan");
            }
            slots_.push_back(std::move(s));
        }
        if (next_frame != frames_.size())
            throw std::invalid_argument(
                std::string("lazy frame op '") + op.name +
                "': " + std::to_string(frames_.size()) + " plan(s) given for " +
                std::to_string(next_frame) + " frame operand(s)");
    }

    const std::vector<LazyFrame>& frames() const noexcept { return frames_; }

    // The strings of operand `i` (a STR or STRLIST operand).
    const std::vector<std::string>& strings_at(std::size_t i) const {
        return slots_[i].strings;
    }

    // The value of operand `i` (an I64 operand).
    std::int64_t i64_at(std::size_t i) const { return slots_[i].val.i64; }
    // The value of operand `i` (an I32 operand).
    std::int32_t i32_at(std::size_t i) const { return slots_[i].val.i32; }
    // The value of operand `i` (an F64 operand).
    double f64_at(std::size_t i) const { return slots_[i].val.f64; }
    // The value of operand `i` (a SCALAR operand).
    const dftu_scalar& scalar_at(std::size_t i) const {
        return slots_[i].val.scalar;
    }
    // The column of operand `i` (a SERIES operand).
    const Series& series_at(std::size_t i) const { return slots_[i].series; }

    // The same operands over other frame plans; `frames` must match the
    // frame operand count.
    std::shared_ptr<const OwnedFrameOpArgs> with_frames(
        std::vector<LazyFrame> frames) const {
        auto out = std::shared_ptr<OwnedFrameOpArgs>(new OwnedFrameOpArgs());
        out->slots_.reserve(slots_.size());
        for (const Slot& s : slots_) {
            Slot c;
            c.tok = s.tok;
            c.val = s.val;
            c.series = s.series.valid() ? s.series.share() : Series{};
            c.strings = s.strings;
            c.i32s = s.i32s;
            c.i64s = s.i64s;
            c.aggs = s.aggs;
            c.wins = s.wins;
            c.has_value = s.has_value;
            c.has_time = s.has_time;
            c.has_end = s.has_end;
            c.frame_index = s.frame_index;
            out->slots_.push_back(std::move(c));
        }
        out->frames_ = std::move(frames);
        return out;
    }

    // The C operand bag, pointing into this object's storage; `cstrs` and
    // `wins` are scratch the bag points into and must outlive the run.
    dftu_op_arg bind(std::vector<std::vector<const char*>>& cstrs,
                     std::vector<std::vector<dftu_window_spec>>& wins,
                     std::vector<std::vector<dftu_group_agg>>& aggs) const {
        dftu_op_arg out{};
        cstrs.assign(slots_.size(), {});
        wins.assign(slots_.size(), {});
        aggs.assign(slots_.size(), {});
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const Slot& s = slots_[i];
            dftu_op_val& v = out.args[i];
            switch (s.tok) {
                case DFTU_TOK_NONE:
                case DFTU_TOK_FRAME:
                case DFTU_TOK_EXPR:
                case DFTU_TOK_DUQL:
                case DFTU_TOK_LAZY:
                    break;
                case DFTU_TOK_SERIES:
                    v.series = s.series.handle();
                    break;
                case DFTU_TOK_SCALAR:
                case DFTU_TOK_I64:
                case DFTU_TOK_U64:
                case DFTU_TOK_F64:
                case DFTU_TOK_CHAR:
                case DFTU_TOK_BOOL:
                case DFTU_TOK_CMP:
                case DFTU_TOK_PRIM:
                case DFTU_TOK_LOGICAL:
                case DFTU_TOK_DTYPE:
                case DFTU_TOK_REDUCE:
                case DFTU_TOK_I32:
                case DFTU_TOK_RANK:
                case DFTU_TOK_ROLLING:
                    v = s.val;
                    break;
                case DFTU_TOK_STR:
                    v.str.ptr = s.strings.front().c_str();
                    v.str.len = static_cast<int32_t>(s.strings.front().size());
                    break;
                case DFTU_TOK_STRLIST:
                    for (const std::string& str : s.strings)
                        cstrs[i].push_back(str.c_str());
                    v.list.items = cstrs[i].data();
                    v.list.n = static_cast<int32_t>(cstrs[i].size());
                    break;
                case DFTU_TOK_I32LIST:
                    v.i32list.items = s.i32s.data();
                    v.i32list.n = static_cast<int32_t>(s.i32s.size());
                    break;
                case DFTU_TOK_I64LIST:
                    v.i64list.items = s.i64s.data();
                    v.i64list.n = static_cast<int32_t>(s.i64s.size());
                    break;
                case DFTU_TOK_AGGLIST:
                    for (std::size_t k = 0; k < s.aggs.size(); ++k) {
                        dftu_group_agg a = s.aggs[k];
                        a.op = s.strings[3 * k].c_str();
                        a.column = s.strings[3 * k + 1].c_str();
                        a.out = s.strings[3 * k + 2].c_str();
                        aggs[i].push_back(a);
                    }
                    v.agglist.items = aggs[i].data();
                    v.agglist.n = static_cast<int32_t>(aggs[i].size());
                    break;
                case DFTU_TOK_WINLIST:
                    for (std::size_t k = 0; k < s.wins.size(); ++k) {
                        dftu_window_spec w = s.wins[k];
                        w.value =
                            s.has_value[k] ? s.strings[4 * k].c_str() : nullptr;
                        const char* time = s.has_time[k]
                                               ? s.strings[4 * k + 1].c_str()
                                               : nullptr;
                        w.out = s.strings[4 * k + 2].c_str();
                        if (w.func == DFTU_WINDOW_FRAME_ARG_MAX ||
                            w.func == DFTU_WINDOW_FRAME_ARG_MIN) {
                            w.param.frame.by =
                                s.has_end[k] ? s.strings[4 * k + 3].c_str()
                                             : nullptr;
                        } else if (w.func == DFTU_WINDOW_RATE) {
                            w.param.rate.time = time;
                        } else if (w.func == DFTU_WINDOW_SESSIONIZE) {
                            w.param.session.time = time;
                            w.param.session.end =
                                s.has_end[k] ? s.strings[4 * k + 3].c_str()
                                             : nullptr;
                        }
                        wins[i].push_back(w);
                    }
                    v.winlist.items = wins[i].data();
                    v.winlist.n = static_cast<int32_t>(wins[i].size());
                    break;
            }
        }
        return out;
    }

   private:
    OwnedFrameOpArgs() = default;

    struct Slot {
        dftu_op_tok tok = DFTU_TOK_NONE;
        dftu_op_val val{};
        Series series;
        std::vector<std::string> strings;
        std::vector<std::int32_t> i32s;
        std::vector<std::int64_t> i64s;
        std::vector<dftu_group_agg> aggs;
        std::vector<dftu_window_spec> wins;
        std::vector<bool> has_value;
        std::vector<bool> has_time;
        std::vector<bool> has_end;
        std::size_t frame_index = 0;
    };
    std::vector<Slot> slots_;
    std::vector<LazyFrame> frames_;
};

// The registry op that applies per partition: its second operand lists the
// partition columns.
inline constexpr const char* WINDOW_OP = "dftu.frame.window";

inline constexpr const char* COLUMN_OP = "dftu.frame.column_op";
inline constexpr const char* WITH_COLUMN_OP = "dftu.frame.with_column";

// The column ops whose result row depends only on the same row of their
// operands: they run over one morsel at a time, the same rows as over the whole
// input. Any other column op (a running, rolling, sorting, ranking or
// reducing one) needs the whole column.
inline constexpr std::string_view ELEMENTWISE_COLUMN_OPS[] = {
    "dftu.series.add",
    "dftu.series.sub",
    "dftu.series.mul",
    "dftu.series.div",
    "dftu.series.add_scalar",
    "dftu.series.sub_scalar",
    "dftu.series.mul_scalar",
    "dftu.series.div_scalar",
    "dftu.series.floordiv",
    "dftu.series.mod",
    "dftu.series.pow",
    "dftu.series.floordiv_scalar",
    "dftu.series.mod_scalar",
    "dftu.series.pow_scalar",
    "dftu.series.compare",
    "dftu.series.compare_series",
    "dftu.series.cast",
    "dftu.series.logical",
    "dftu.series.logical_not",
    "dftu.series.abs",
    "dftu.series.round",
    "dftu.series.ceil",
    "dftu.series.floor",
    "dftu.series.trunc",
    "dftu.series.sign",
    "dftu.series.negate",
    "dftu.series.sqrt",
    "dftu.series.exp",
    "dftu.series.log",
    "dftu.series.clip",
    "dftu.series.is_between",
    "dftu.series.null_mask",
    "dftu.series.valid_mask",
    "dftu.series.is_nan",
    "dftu.series.is_finite",
    "dftu.series.is_infinite",
    "dftu.series.str_eq",
    "dftu.series.str_contains",
    "dftu.series.str_starts_with",
    "dftu.series.str_ends_with",
    "dftu.series.str_len_bytes",
    "dftu.series.str_len_chars",
    "dftu.series.to_lowercase",
    "dftu.series.to_uppercase",
    "dftu.series.str_strip",
    "dftu.series.str_lstrip",
    "dftu.series.str_rstrip",
    "dftu.series.str_replace",
    "dftu.series.str_replace_all",
    "dftu.series.str_slice",
    "dftu.series.str_pad_start",
    "dftu.series.str_pad_end",
    "dftu.series.str_zfill",
    "dftu.series.str_remove_prefix",
    "dftu.series.str_remove_suffix",
    "dftu.series.str_repeat",
    "dftu.series.list_len",
    "dftu.series.list_get",
    "dftu.series.dt_part",
    "dftu.series.dt_round",
    "dftu.series.dt_format",
    "dftu.series.with_timezone"};

inline bool column_op_elementwise(std::string_view series_op) {
    return std::find(std::begin(ELEMENTWISE_COLUMN_OPS),
                     std::end(ELEMENTWISE_COLUMN_OPS),
                     series_op) != std::end(ELEMENTWISE_COLUMN_OPS);
}

inline constexpr const char* COMPARE_AGG_OP = "dftu.frame.compare_agg";
inline constexpr const char* ASOF_OP = "dftu.frame.asof";
inline constexpr const char* INTERVAL_OP = "dftu.frame.interval";
inline constexpr const char* GROUP_TRANSFORM_OP = "dftu.frame.group_transform";
inline constexpr std::int64_t NATIVE_SHIFT_MAX = 1024;

// Runs the registry op `op` over `primary`, collecting each further frame
// operand's plan, and returns its result.
inline coro::CoroTask<DataFrame> run_frame_op(
    const dftu_op_desc* op, const OwnedFrameOpArgs& args, DataFrame primary,
    const std::string& name, const std::vector<std::string>& sch) {
    std::vector<dftu_dataframe*> handles;
    struct Free {
        std::vector<dftu_dataframe*>& h;
        ~Free() {
            for (dftu_dataframe* p : h) dftu_dataframe_free(p);
        }
    } guard{handles};
    handles.push_back(dataframe_handle_wrap(std::move(primary)));
    for (const LazyFrame& other : args.frames()) {
        DataFrame f = co_await other.collect();
        handles.push_back(dataframe_handle_wrap(std::move(f)));
    }
    std::vector<std::vector<const char*>> cstrs;
    std::vector<std::vector<dftu_window_spec>> wins;
    std::vector<std::vector<dftu_group_agg>> aggs;
    const dftu_op_arg bag = args.bind(cstrs, wins, aggs);
    std::vector<const dftu_dataframe*> frames(handles.begin(), handles.end());
    dftu_dataframe* result = dftu_op_run_frame(
        op, frames.data(), static_cast<uint32_t>(frames.size()), &bag);
    if (!result)
        throw std::runtime_error("lazy frame op '" + name +
                                 "' failed: the op returned no frame");
    DataFrame out = dataframe_handle_take(result);
    if (has_rest(sch)) {
        if (column_index_of(out.names, std::string(REST_COLUMN)) < 0)
            throw std::runtime_error(
                "lazy frame op '" + name +
                "' dropped the columns the scan returned beyond the "
                "plan's schema");
        out = rest_to_back(std::move(out));
    }
    co_return out;
}

}  // namespace dftracer::utils::dataframe::lazy_internal

#endif  // DFTRACER_UTILS_DATAFRAME_LAZY_FRAME_OP_H
