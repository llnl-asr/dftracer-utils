// SPDX-License-Identifier: MIT
#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>

#include <atomic>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

std::atomic<std::uint64_t> g_scan_morsels{0};

// What a morsel needs of the rows before it: the last valid value the op
// produced (a running op: the value after it equals the op over the rows
// so far), or the last rows of its input (a positional op).
enum class Carry { Output, Input };

struct ScanOp {
    dstd::StringView name;
    // The op over floats depends on the order its kernel adds or compares in,
    // so only integers take the streaming form.
    bool integers_only;
    Carry carry;
};

constexpr ScanOp SCAN_OPS[] = {
    {"dftu.series.cumsum", true, Carry::Output},
    {"dftu.series.cum_prod", true, Carry::Output},
    {"dftu.series.cummax", true, Carry::Output},
    {"dftu.series.cummin", true, Carry::Output},
    {"dftu.series.ffill", false, Carry::Output},
    {"dftu.series.diff", false, Carry::Input},
    {"dftu.series.shift", false, Carry::Input},
};

std::int64_t scalar_to_i64(const dftu_scalar& s) {
    switch (s.kind) {
        case DFTU_SCALAR_TAG_F64:
            return static_cast<std::int64_t>(s.value.d);
        case DFTU_SCALAR_TAG_U64:
            return static_cast<std::int64_t>(s.value.u);
        default:
            return s.value.i;
    }
}

// Runs the series op on each morsel of one column with the carried rows in
// front, then drops them from the result. The op runs on the real kernel, so
// the types, the nulls and the wrap-around of an integer sum are those of the
// op over the whole column. Every other column passes through shared.
class ColumnScanCursor : public Cursor {
   public:
    ColumnScanCursor(dstd::UniquePtr<Cursor> in, dstd::Vector<Field> in_fields,
                     std::size_t column, const dftu_op_desc* op, Carry carry,
                     std::int64_t keep, std::int64_t shift)
        : in_(std::move(in)),
          in_fields_(std::move(in_fields)),
          column_(column),
          op_(op),
          carry_mode_(carry),
          keep_(keep),
          shift_(shift) {}

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        for (;;) {
            auto m = co_await in_->next(max_rows);
            if (!m) break;
            if (m->rows == 0) continue;
            sent_ = true;
            co_return scan(std::move(*m));
        }
        if (sent_) co_return dstd::nullopt;
        // An input with no rows still leaves as one morsel of typed columns,
        // the op's column typed by the op itself.
        sent_ = true;
        Morsel none;
        for (const Field& f : in_fields_)
            none.columns.push_back(
                null_column_of(f, 0, "column_op", "the input has no rows"));
        co_return scan(std::move(none));
    }

   private:
    Morsel scan(Morsel&& m) {
        g_scan_morsels.fetch_add(1, std::memory_order_relaxed);
        const Series col = m.columns[column_].share();
        const std::int64_t held = carry_.valid() ? carry_.length() : 0;
        Series whole;
        if (held > 0) {
            const dstd::Vector<const Series*> parts{&carry_, &col};
            whole = concat_columns(parts);
        } else {
            whole = col.share();
        }
        dftu_op_arg arg{};
        arg.args[1].i64 = shift_;
        const dftu_series* in[1] = {whole.handle()};
        dftu_series* raw = dftu_op_run(op_, in, 1, &arg);
        if (!raw)
            throw std::runtime_error(
                "lazy frame op 'dftu.frame.column_op' failed: the op returned "
                "no column");
        Series res{raw};
        if (res.length() != whole.length())
            throw std::runtime_error(
                "lazy frame op 'dftu.frame.column_op' failed: the op is not "
                "row for row");
        if (carry_mode_ == Carry::Output) {
            for (std::int64_t i = res.length() - 1; i >= 0; --i) {
                if (res.is_null(i)) continue;
                carry_ = res.slice(i, 1);
                break;
            }
        } else if (keep_ > 0) {
            const std::int64_t k = std::min(keep_, whole.length());
            carry_ = whole.slice(whole.length() - k, k);
        }
        m.columns[column_] =
            held > 0 ? res.slice(held, m.rows) : std::move(res);
        m.ordering = Ordering::Unordered;
        return std::move(m);
    }

    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<Field> in_fields_;
    std::size_t column_;
    const dftu_op_desc* op_;
    Carry carry_mode_;
    std::int64_t keep_;
    std::int64_t shift_;
    Series carry_;
    bool sent_ = false;
};

// Adds or replaces one column of the frame, giving each morsel the rows of the
// operand column that sit under it.
class SeriesColumnCursor : public Cursor {
   public:
    SeriesColumnCursor(dstd::UniquePtr<Cursor> in,
                       const dstd::Vector<dstd::String>& sch, dstd::String name,
                       Series column)
        : in_(std::move(in)), out_names_(sch) {
        const int at = column_index_of(sch, name);
        if (at < 0) {
            at_ = sch.size();
            out_names_.push_back(name);
        } else {
            at_ = static_cast<std::size_t>(at);
        }
        operand_.names.push_back(std::move(name));
        operand_.columns.push_back(std::move(column));
    }

    coro::CoroTask<dstd::Optional<Morsel>> next(
        std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return dstd::nullopt;
        g_scan_morsels.fetch_add(1, std::memory_order_relaxed);
        if (offset_ + m->rows > operand_.columns[0].length())
            throw std::out_of_range(
                "with_column: the column is shorter than the frame");
        Series part =
            std::move(operand_.slice(offset_, m->rows).columns.front());
        offset_ += m->rows;
        if (at_ < m->columns.size())
            m->columns[at_] = std::move(part);
        else
            m->columns.push_back(std::move(part));
        m->ordering = Ordering::Unordered;
        co_return std::move(*m);
    }

    dstd::Optional<dstd::Vector<dstd::String>> out_names() const override {
        return out_names_;
    }

   private:
    dstd::UniquePtr<Cursor> in_;
    dstd::Vector<dstd::String> out_names_;
    DataFrame operand_;
    std::size_t at_ = 0;
    std::int64_t offset_ = 0;
};

bool static_schema(const dstd::Vector<dstd::String>& sch,
                   const dstd::Vector<Field>& in_fields) {
    return !sch.empty() && !has_rest(sch) && in_fields.size() == sch.size();
}

}  // namespace

std::uint64_t column_scan_morsels() {
    return g_scan_morsels.load(std::memory_order_relaxed);
}

namespace lazy_internal {

dstd::UniquePtr<Cursor> make_column_scan(dstd::UniquePtr<Cursor>& in,
                                         const dstd::Vector<dstd::String>& sch,
                                         const dstd::Vector<Field>& in_fields,
                                         const OwnedFrameOpArgs& args) {
    if (!static_schema(sch, in_fields) || !args.frames().empty())
        return nullptr;
    const dstd::String& series_op = args.strings_at(2).front();
    const ScanOp* scan =
        std::find_if(std::begin(SCAN_OPS), std::end(SCAN_OPS),
                     [&](const ScanOp& s) { return s.name == series_op; });
    if (scan == std::end(SCAN_OPS) || !args.strings_at(3).front().empty())
        return nullptr;
    const dstd::String& column = args.strings_at(1).front();
    const int at = column_index_of(sch, column);
    const auto field =
        std::find_if(in_fields.begin(), in_fields.end(),
                     [&](const Field& f) { return f.name == column; });
    NumClass cls = NumClass::Signed;
    if (at < 0 || field == in_fields.end() || !num_class(field->type.id, cls) ||
        (scan->integers_only && cls == NumClass::Float))
        return nullptr;
    std::int64_t shift = 0;
    std::int64_t keep = 0;
    if (series_op == "dftu.series.shift") {
        shift = scalar_to_i64(args.scalar_at(4));
        if (shift < 0 || shift > NATIVE_SHIFT_MAX) return nullptr;
        keep = shift;
    } else if (series_op == "dftu.series.diff") {
        keep = 1;
    }
    const dftu_op_desc* op = dftu_op_find(series_op.c_str());
    if (!op) return nullptr;
    return dstd::make_unique<ColumnScanCursor>(std::move(in), in_fields,
                                               static_cast<std::size_t>(at), op,
                                               scan->carry, keep, shift);
}

dstd::UniquePtr<Cursor> make_series_column(
    dstd::UniquePtr<Cursor>& in, const dstd::Vector<dstd::String>& sch,
    const dstd::Vector<Field>& in_fields, const OwnedFrameOpArgs& args) {
    if (!static_schema(sch, in_fields) || !args.frames().empty() ||
        !args.series_at(2).valid())
        return nullptr;
    return dstd::make_unique<SeriesColumnCursor>(std::move(in), sch,
                                                 args.strings_at(1).front(),
                                                 args.series_at(2).share());
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
