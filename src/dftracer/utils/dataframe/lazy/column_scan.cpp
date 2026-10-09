#include <dftracer/utils/dataframe/lazy/cursors.h>
#include <dftracer/utils/dataframe/lazy/numeric.h>

#include <atomic>

namespace dftracer::utils::dataframe {

using namespace lazy_internal;

namespace {

std::atomic<std::uint64_t> g_scan_morsels{0};

// What a morsel needs of the rows before it: the last valid value the op
// produced (a running op: the value after it equals the op over the rows
// so far), or the last rows of its input (a positional op).
enum class Carry { OUTPUT, INPUT };

struct ScanOp {
    std::string_view name;
    // The op over floats depends on the order its kernel adds or compares in,
    // so only integers take the streaming form.
    bool integers_only;
    Carry carry;
};

constexpr ScanOp SCAN_OPS[] = {
    {"dftu.series.cumsum", true, Carry::OUTPUT},
    {"dftu.series.cum_prod", true, Carry::OUTPUT},
    {"dftu.series.cummax", true, Carry::OUTPUT},
    {"dftu.series.cummin", true, Carry::OUTPUT},
    {"dftu.series.ffill", false, Carry::OUTPUT},
    {"dftu.series.diff", false, Carry::INPUT},
    {"dftu.series.shift", false, Carry::INPUT},
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
    ColumnScanCursor(std::unique_ptr<Cursor> in, std::vector<Field> in_fields,
                     std::size_t column, const dftu_op_desc* op, Carry carry,
                     std::int64_t keep, std::int64_t shift)
        : in_(std::move(in)),
          in_fields_(std::move(in_fields)),
          column_(column),
          op_(op),
          carry_mode_(carry),
          keep_(keep),
          shift_(shift) {}

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        while (auto m = co_await in_->next(max_rows)) {
            if (m->rows == 0) continue;
            sent_ = true;
            co_return scan(std::move(*m));
        }
        if (sent_) co_return std::nullopt;
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
            const std::vector<const Series*> parts{&carry_, &col};
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
        if (carry_mode_ == Carry::OUTPUT) {
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

    std::unique_ptr<Cursor> in_;
    std::vector<Field> in_fields_;
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
    SeriesColumnCursor(std::unique_ptr<Cursor> in,
                       const std::vector<std::string>& sch, std::string name,
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

    coro::CoroTask<std::optional<Morsel>> next(std::int64_t max_rows) override {
        auto m = co_await in_->next(max_rows);
        if (!m) co_return std::nullopt;
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

    std::optional<std::vector<std::string>> out_names() const override {
        return out_names_;
    }

   private:
    std::unique_ptr<Cursor> in_;
    std::vector<std::string> out_names_;
    DataFrame operand_;
    std::size_t at_ = 0;
    std::int64_t offset_ = 0;
};

bool static_schema(const std::vector<std::string>& sch,
                   const std::vector<Field>& in_fields) {
    return !sch.empty() && !has_rest(sch) && in_fields.size() == sch.size();
}

}  // namespace

std::uint64_t column_scan_morsels() {
    return g_scan_morsels.load(std::memory_order_relaxed);
}

namespace lazy_internal {

std::unique_ptr<Cursor> make_column_scan(std::unique_ptr<Cursor>& in,
                                         const std::vector<std::string>& sch,
                                         const std::vector<Field>& in_fields,
                                         const OwnedFrameOpArgs& args) {
    if (!static_schema(sch, in_fields) || !args.frames().empty())
        return nullptr;
    const std::string& series_op = args.strings_at(2).front();
    const ScanOp* scan =
        std::find_if(std::begin(SCAN_OPS), std::end(SCAN_OPS),
                     [&](const ScanOp& s) { return s.name == series_op; });
    if (scan == std::end(SCAN_OPS) || !args.strings_at(3).front().empty())
        return nullptr;
    const std::string& column = args.strings_at(1).front();
    const int at = column_index_of(sch, column);
    const auto field =
        std::find_if(in_fields.begin(), in_fields.end(),
                     [&](const Field& f) { return f.name == column; });
    NumClass cls = NumClass::SIGNED;
    if (at < 0 || field == in_fields.end() || !num_class(field->type.id, cls) ||
        (scan->integers_only && cls == NumClass::FLOAT))
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
    return std::make_unique<ColumnScanCursor>(std::move(in), in_fields,
                                              static_cast<std::size_t>(at), op,
                                              scan->carry, keep, shift);
}

std::unique_ptr<Cursor> make_series_column(std::unique_ptr<Cursor>& in,
                                           const std::vector<std::string>& sch,
                                           const std::vector<Field>& in_fields,
                                           const OwnedFrameOpArgs& args) {
    if (!static_schema(sch, in_fields) || !args.frames().empty() ||
        !args.series_at(2).valid())
        return nullptr;
    return std::make_unique<SeriesColumnCursor>(std::move(in), sch,
                                                args.strings_at(1).front(),
                                                args.series_at(2).share());
}

}  // namespace lazy_internal

}  // namespace dftracer::utils::dataframe
