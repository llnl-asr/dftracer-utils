// SPDX-License-Identifier: MIT
// The typed half of the plugin SDK: SchemaBuilder and SchemaView, FileFormat's
// probe, BindView and SinkWriter::write over a FrameView, typed OpArgs, a
// pushed-filter vote on its way back to the host, the ExprView accessors and
// eval, the schema field getters and ColumnView::boolean_at.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dataframe/internal/schema_types.h>
#include <dftracer/utils/dataframe/plugin_frame.h>
#include <dftracer/utils/dataframe/types.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin/file_format.h>
#include <dftracer/utils/plugins/plugin/node.h>
#include <dftracer/utils/plugins/plugin/source.h>
#include <doctest/doctest.h>
#include <plugins/test_abi_ctx.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

// A frame of two int64 columns: id = 0, 1 and val = 0, 10.
OwnedDataFrame two_columns() {
    FrameBuilder builder;
    builder.add("id", Column::from_int64(dstd::Vector<std::int64_t>{3, 4}));
    builder.add("val", Column::from_int64(dstd::Vector<std::int64_t>{20, 10}));
    auto built = builder.build();
    REQUIRE(built.has_value());
    return std::move(*built);
}

// dftu_scalar is a plain aggregate and the DFTU_SCALAR_* macros are C-only.
dftu_scalar i64_scalar(std::int64_t value) {
    dftu_scalar s{};
    s.kind = DFTU_SCALAR_TAG_I64;
    s.value.i = value;
    return s;
}

// A FLAT int64 column of `values`, owned by the test.
Column int64_column(dstd::Vector<std::int64_t> values) {
    return Column(dftu_series_new_flat(DFTU_TYPE_INT64, values.data(),
                                       static_cast<std::int64_t>(values.size()),
                                       nullptr));
}

// A FLAT Bool column of `rows` rows over `bits`, one bit per row, least
// significant bit first.
Column bool_column(dstd::Vector<std::uint8_t> bits, std::int64_t rows,
                   dstd::Vector<std::uint8_t> validity = {}) {
    return Column(
        dftu_series_new_flat(DFTU_TYPE_BOOL, bits.data(), rows,
                             validity.empty() ? nullptr : validity.data()));
}

// `column <op> rhs`, as a handle the caller owns. The column handle is freed
// here because dftu_expr_cmp shares it and does not take it.
dftu_expr* make_compare(std::int32_t column, std::int32_t op, dftu_scalar rhs) {
    dftu_expr* const col = dftu_expr_col(column);
    dftu_expr* const cmp = dftu_expr_cmp(op, col, rhs);
    dftu_expr_free(col);
    return cmp;
}

// `column <op> pattern`, ownership as make_compare.
dftu_expr* make_str_pred(std::int32_t column, std::int32_t op,
                         const char* pattern) {
    dftu_expr* const col = dftu_expr_col(column);
    dftu_expr* const pred = dftu_expr_str_pred(
        op, col, pattern, static_cast<std::int32_t>(std::strlen(pattern)));
    dftu_expr_free(col);
    return pred;
}

// `column is_in values`, ownership as make_compare.
dftu_expr* make_is_in(std::int32_t column, const dftu_series* values) {
    dftu_expr* const col = dftu_expr_col(column);
    dftu_expr* const in = dftu_expr_is_in(col, values);
    dftu_expr_free(col);
    return in;
}

// col(0) + col(1) + ... over `n` columns.
OwnedExpr column_sum(std::int32_t n) {
    dftu_expr* acc = dftu_expr_col(0);
    for (std::int32_t i = 1; i < n; ++i) {
        dftu_expr* const col = dftu_expr_col(i);
        dftu_expr* const next = dftu_expr_arith(0, acc, col);
        dftu_expr_free(acc);
        dftu_expr_free(col);
        acc = next;
    }
    return OwnedExpr(acc);
}

// What a writer of the sink test recorded, kept by the sink so it outlives the
// writer box finalize() frees.
struct Record {
    std::int32_t writes = 0;
    std::int32_t columns = 0;
    std::int64_t rows = 0;
    dstd::Vector<dstd::String> names;
    dstd::Vector<std::int64_t> first_values;
};

// A format that records the head bytes and the path probe() was handed.
class ProbedFormat : public FileFormat {
   public:
    std::int32_t probe(dstd::ByteView head, dstd::CStringView path) override {
        head_size = head.size();
        head_bytes.assign(reinterpret_cast<const char*>(head.data()),
                          head.size());
        seen_path = dstd::String(path.c_str());
        return 42;
    }
    dstd::Result<dstd::UniquePtr<FormatFile>> open(dstd::StringView) override {
        return dstd::Unexpected(unsupported("the probe test opens nothing"));
    }

    std::size_t head_size = 0;
    dstd::String head_bytes;
    dstd::String seen_path;
};

// A writer that records the frame it is handed into the sink's record.
class RecordingWriter : public SinkWriter {
   public:
    explicit RecordingWriter(dstd::SharedPtr<Record> record)
        : record_(std::move(record)) {}

    dstd::Result<void> write(FrameView batch) override {
        ++record_->writes;
        record_->columns = batch.column_count();
        record_->rows = batch.row_count();
        for (std::int32_t i = 0; i < batch.column_count(); ++i) {
            record_->names.emplace_back(batch.name(i));
            const ColumnView column = batch.column(i);
            if (column.type() != TypeId::Int64)
                return invalid_argument(
                    "the recording writer reads int64 only");
            const dstd::Span<const std::int64_t> values =
                column.values<std::int64_t>();
            record_->first_values.push_back(values.empty() ? 0
                                                           : values.front());
        }
        return {};
    }
    dstd::Result<void> finalize() override { return {}; }

   private:
    dstd::SharedPtr<Record> record_;
};

class RecordingSink : public Sink {
   public:
    RecordingSink() : record_(dstd::make_shared<Record>()) {}

    dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        const BindView& view) override {
        bind_names = view.names;
        bind_types = view.types;
        option = dstd::String(view.options.get("path"));
        return dstd::UniquePtr<SinkWriter>(
            dstd::make_unique<RecordingWriter>(record_));
    }

    dstd::SharedPtr<Record> record_;
    dstd::Vector<dstd::StringView> bind_names;
    dstd::Vector<TypeId> bind_types;
    dstd::String option;
};

// A source that votes Exact on the first filter and says nothing about the
// rest.
class VotingSource {
   public:
    dstd::Vector<dstd::String> names() const { return {"id"}; }
    void schema(SchemaBuilder& out) const { out.add("id", TypeId::Int64); }

    struct Cursor {
        dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) {
            return dstd::Optional<OwnedDataFrame>();
        }
    };

    dstd::Result<ScanResult<Cursor>> scan(const ScanView&, const Host&) const {
        ScanResult<Cursor> result;
        result.cursor = dstd::make_unique<Cursor>();
        result.filters.push_back(Pushed::Exact);
        return result;
    }
};

}  // namespace

TEST_CASE("SchemaBuilder keeps the time unit of a column") {
    dftu_schema schema{};
    SchemaBuilder builder(&schema);
    CHECK(builder.add("ts", TypeId::Timestamp, TimeUnit::Nano, false) == 0);
    CHECK(builder.add("dur", TypeId::Duration, TimeUnit::Milli) == 1);
    // The form without a unit declares microseconds.
    CHECK(builder.add("plain", TypeId::Int64) == 2);

    const SchemaView view(&schema);
    REQUIRE(view.size() == 3);
    CHECK(view.name(0) == "ts");
    CHECK(view.type(0) == TypeId::Timestamp);
    CHECK(view.unit(0) == TimeUnit::Nano);
    CHECK_FALSE(view.nullable(0));
    CHECK(view.type(1) == TypeId::Duration);
    CHECK(view.unit(1) == TimeUnit::Milli);
    CHECK(view.nullable(1));
    CHECK(view.unit(2) == TimeUnit::Micro);
    CHECK(view.find("dur") == 1);
    CHECK(view.find("absent") == -1);
}

TEST_CASE("ExprView::as_compare reads a column comparison") {
    OwnedExpr expr(make_compare(2, DFTU_CMP_GT, i64_scalar(1000)));
    REQUIRE(static_cast<bool>(expr.view()));
    const ExprView view = expr.view();

    // A comparison is not a bare column and not any other shape.
    CHECK_FALSE(view.column().has_value());
    CHECK_FALSE(view.as_str_pred().has_value());
    CHECK_FALSE(view.as_is_in().has_value());
    CHECK_FALSE(view.as_logical().has_value());
    CHECK_FALSE(view.as_not().has_value());

    const dstd::Optional<ExprView::Compare> cmp = view.as_compare();
    REQUIRE(cmp.has_value());
    CHECK(cmp->column == 2);
    CHECK(cmp->op == CmpOp::Gt);
    const ScalarView literal = cmp->rhs();
    CHECK(literal.kind() == ScalarKind::I64);
    CHECK(literal.i64() == 1000);

    // A bare column is a different shape: a column index and no literal.
    dftu_expr* const bare = dftu_expr_col(2);
    const ExprView bare_view(bare);
    CHECK(bare_view.column() == 2);
    CHECK_FALSE(bare_view.as_compare().has_value());
    dftu_expr_free(bare);

    // An empty view names no expression at all.
    const ExprView none;
    CHECK_FALSE(none.column().has_value());
    CHECK_FALSE(none.as_compare().has_value());
}

TEST_CASE("ExprView::as_str_pred reads a string predicate") {
    OwnedExpr expr(make_str_pred(1, DFTU_STR_PRED_ENDS_WITH, "dur"));
    REQUIRE(static_cast<bool>(expr.view()));
    const ExprView view = expr.view();

    CHECK_FALSE(view.as_compare().has_value());
    CHECK_FALSE(view.as_is_in().has_value());
    CHECK_FALSE(view.as_logical().has_value());
    CHECK_FALSE(view.as_not().has_value());

    const dstd::Optional<ExprView::StrPred> pred = view.as_str_pred();
    REQUIRE(pred.has_value());
    CHECK(pred->column == 1);
    CHECK(pred->op == StrPredOp::EndsWith);
    CHECK(pred->pattern == "dur");
}

TEST_CASE("ExprView::as_is_in reads the candidate values") {
    Column values = int64_column({4, 8, 15});
    REQUIRE(values);
    OwnedExpr expr(make_is_in(3, values.get()));
    REQUIRE(static_cast<bool>(expr.view()));
    const ExprView view = expr.view();

    CHECK_FALSE(view.as_compare().has_value());
    CHECK_FALSE(view.as_str_pred().has_value());
    CHECK_FALSE(view.as_logical().has_value());
    CHECK_FALSE(view.as_not().has_value());

    const dstd::Optional<ExprView::IsIn> in = view.as_is_in();
    REQUIRE(in.has_value());
    CHECK(in->column == 3);
    const ColumnView candidates = in->values();
    REQUIRE(candidates.type() == TypeId::Int64);
    REQUIRE(candidates.size() == 3);
    const dstd::Span<const std::int64_t> nums =
        candidates.values<std::int64_t>();
    REQUIRE(nums.size() == 3);
    CHECK(nums[0] == 4);
    CHECK(nums[1] == 8);
    CHECK(nums[2] == 15);
}

TEST_CASE("ExprView::as_logical reads both children") {
    dftu_expr* const left = make_compare(0, DFTU_CMP_LT, i64_scalar(5));
    dftu_expr* const right = make_compare(1, DFTU_CMP_EQ, i64_scalar(7));
    OwnedExpr expr(dftu_expr_logical(DFTU_LOGICAL_OR, left, right));
    // The logical node shares its children, so the builder handles are freed.
    dftu_expr_free(left);
    dftu_expr_free(right);
    REQUIRE(static_cast<bool>(expr.view()));
    const ExprView view = expr.view();

    CHECK_FALSE(view.as_compare().has_value());
    CHECK_FALSE(view.as_str_pred().has_value());
    CHECK_FALSE(view.as_is_in().has_value());
    CHECK_FALSE(view.as_not().has_value());

    const dstd::Optional<ExprView::Logical> both = view.as_logical();
    REQUIRE(both.has_value());
    CHECK(both->op == LogicalOp::Or);

    const dstd::Optional<ExprView::Compare> left_cmp =
        both->lhs.view().as_compare();
    REQUIRE(left_cmp.has_value());
    CHECK(left_cmp->column == 0);
    CHECK(left_cmp->op == CmpOp::Lt);
    CHECK(left_cmp->rhs().i64() == 5);

    const dstd::Optional<ExprView::Compare> right_cmp =
        both->rhs.view().as_compare();
    REQUIRE(right_cmp.has_value());
    CHECK(right_cmp->column == 1);
    CHECK(right_cmp->op == CmpOp::Eq);
    CHECK(right_cmp->rhs().i64() == 7);
}

TEST_CASE("ExprView::as_not reads the negated child") {
    dftu_expr* const cmp = make_compare(3, DFTU_CMP_EQ, i64_scalar(7));
    OwnedExpr expr(dftu_expr_not(cmp));
    dftu_expr_free(cmp);
    REQUIRE(static_cast<bool>(expr.view()));
    const ExprView view = expr.view();

    CHECK_FALSE(view.as_compare().has_value());
    CHECK_FALSE(view.as_str_pred().has_value());
    CHECK_FALSE(view.as_is_in().has_value());
    CHECK_FALSE(view.as_logical().has_value());

    const dstd::Optional<OwnedExpr> inner = view.as_not();
    REQUIRE(inner.has_value());
    const dstd::Optional<ExprView::Compare> inner_cmp =
        inner->view().as_compare();
    REQUIRE(inner_cmp.has_value());
    CHECK(inner_cmp->column == 3);
    CHECK(inner_cmp->op == CmpOp::Eq);
    CHECK(inner_cmp->rhs().i64() == 7);
}

TEST_CASE("ExprView::eval reads each of two input columns") {
    OwnedExpr sum = column_sum(2);
    REQUIRE(static_cast<bool>(sum.view()));

    Column a = int64_column({1, 2, 3});
    Column b = int64_column({10, 20, 30});
    REQUIRE(a);
    REQUIRE(b);
    const ColumnView inputs[2] = {ColumnView(a.get()), ColumnView(b.get())};

    Column result = sum.view().eval(dstd::Span<const ColumnView>(inputs, 2));
    REQUIRE(result);
    CHECK(result.size() == 3);
    const ColumnView out(result.get());
    CHECK(out.type() == TypeId::Int64);
    const dstd::Span<const std::int64_t> values = out.values<std::int64_t>();
    REQUIRE(values.size() == 3);
    CHECK(values[0] == 11);
    CHECK(values[1] == 22);
    CHECK(values[2] == 33);
}

TEST_CASE("ExprView::eval past eight inputs allocates the column array") {
    // eval keeps its first eight column pointers in a stack array, so nine
    // inputs is the first size that goes to the heap.
    constexpr std::size_t INPUTS = 9;
    OwnedExpr sum = column_sum(static_cast<std::int32_t>(INPUTS));
    REQUIRE(static_cast<bool>(sum.view()));

    std::array<Column, INPUTS> columns;
    std::array<ColumnView, INPUTS> inputs{};
    for (std::size_t i = 0; i < INPUTS; ++i) {
        const std::int64_t n = static_cast<std::int64_t>(i);
        columns[i] = int64_column({n, 1000 * n});
        REQUIRE(columns[i]);
        inputs[i] = ColumnView(columns[i].get());
    }

    Column result = sum.view().eval(
        dstd::Span<const ColumnView>(inputs.data(), inputs.size()));
    REQUIRE(result);
    CHECK(result.size() == 2);
    const ColumnView out(result.get());
    const dstd::Span<const std::int64_t> values = out.values<std::int64_t>();
    REQUIRE(values.size() == 2);
    // 0 + 1 + ... + 8.
    CHECK(values[0] == 36);
    CHECK(values[1] == 36000);
}

TEST_CASE("the schema field getters read one field") {
    dftu_schema schema{};
    REQUIRE(dftu_schema_add_field(&schema, "ts", DFTU_TYPE_TIMESTAMP, 0,
                                  DFTU_TIME_UNIT_NANO, nullptr, 0, 0, 0) == 0);
    REQUIRE(dftu_schema_add_field(&schema, "id", DFTU_TYPE_INT64, 1,
                                  DFTU_TIME_UNIT_NANO, nullptr, 0, 0, 0) == 1);
    REQUIRE(dftu_schema_field_count(&schema) == 2);

    CHECK(std::strcmp(dftu_schema_field_name(&schema, 0), "ts") == 0);
    CHECK(dftu_schema_field_type(&schema, 0) == DFTU_TYPE_TIMESTAMP);
    CHECK(dftu_schema_field_nullable(&schema, 0) == 0);
    CHECK(dftu_schema_field_time_unit(&schema, 0) == DFTU_TIME_UNIT_NANO);
    CHECK(std::strcmp(dftu_schema_field_name(&schema, 1), "id") == 0);
    CHECK(dftu_schema_field_type(&schema, 1) == DFTU_TYPE_INT64);
    CHECK(dftu_schema_field_nullable(&schema, 1) == 1);
    // A column that is not a time carries no unit of its own.
    CHECK(dftu_schema_field_time_unit(&schema, 1) == DFTU_TIME_UNIT_MICRO);

    for (const std::int32_t bad : {-1, 2}) {
        CHECK(dftu_schema_field_name(&schema, bad) == nullptr);
        CHECK(dftu_schema_field_type(&schema, bad) == -1);
        CHECK(dftu_schema_field_nullable(&schema, bad) == -1);
        CHECK(dftu_schema_field_time_unit(&schema, bad) == -1);
    }

    CHECK(dftu_schema_field_count(nullptr) == -1);
    CHECK(dftu_schema_field_name(nullptr, 0) == nullptr);
    CHECK(dftu_schema_field_type(nullptr, 0) == -1);
    CHECK(dftu_schema_field_nullable(nullptr, 0) == -1);
    CHECK(dftu_schema_field_time_unit(nullptr, 0) == -1);
}

TEST_CASE("boolean_at reads a FLAT Bool column, a null row as false") {
    // Rows 0-7 are the bits of 0b10101101, rows 8-9 those of 0b00000001, so
    // row 8 is true and lives in the second byte. Row 5 is null and its value
    // bit is set.
    Column column =
        bool_column({0b10101101, 0b00000001}, 10, {0b11011111, 0b11111111});
    REQUIRE(column);
    CHECK(dftu_series_encoding(column.get()) ==
          static_cast<std::int32_t>(dataframe::Encoding::Flat));

    const ColumnView view(column.get());
    REQUIRE(view.type() == TypeId::Bool);
    REQUIRE(view.size() == 10);
    CHECK(view.boolean_at(0));
    CHECK_FALSE(view.boolean_at(1));
    CHECK(view.boolean_at(2));
    CHECK(view.boolean_at(3));
    CHECK_FALSE(view.boolean_at(4));
    REQUIRE(view.is_null(5));
    CHECK_FALSE(view.boolean_at(5));
    CHECK_FALSE(view.boolean_at(6));
    CHECK(view.boolean_at(7));
    CHECK(view.boolean_at(8));
    CHECK_FALSE(view.boolean_at(9));
    CHECK_FALSE(view.boolean_at(-1));
    CHECK_FALSE(view.boolean_at(10));
}

TEST_CASE("boolean_at on a SELECTION is false until the column is flat") {
    Column base =
        bool_column({0b10101101, 0b00000001}, 10, {0b11011111, 0b11111111});
    REQUIRE(base);
    // Keep rows 5 (null) and 8 (true).
    Column mask = bool_column({0b00100000, 0b00000001}, 10);
    REQUIRE(mask);
    Column selected(dftu_series_filter(base.get(), mask.get()));
    REQUIRE(selected);
    CHECK(dftu_series_encoding(selected.get()) ==
          static_cast<std::int32_t>(dataframe::Encoding::Selection));

    const ColumnView view(selected.get());
    REQUIRE(view.type() == TypeId::Bool);
    REQUIRE(view.size() == 2);
    // Not FLAT, so there are no bits of its own at row 0 and row 1.
    CHECK_FALSE(view.boolean_at(0));
    CHECK_FALSE(view.boolean_at(1));

    const dstd::Result<Column> flat = view.flat();
    REQUIRE(flat.has_value());
    const ColumnView flat_view(flat->get());
    REQUIRE(flat_view.type() == TypeId::Bool);
    REQUIRE(flat_view.size() == 2);
    CHECK(flat_view.is_null(0));
    CHECK_FALSE(flat_view.boolean_at(0));
    CHECK(flat_view.boolean_at(1));
}

TEST_CASE("probe gets the head bytes and the path") {
    auto self =
        dstd::UniquePtr<ProbedFormat>(dstd::make_unique<ProbedFormat>());
    ProbedFormat* const seen = self.get();
    void* const box = make_file_format_self(std::move(self));
    const dftu_file_format_vt* const vt = file_format_vtable<ProbedFormat>();

    const char head[] = "a,b\n1,2\n";
    const std::int32_t score =
        vt->probe(box, reinterpret_cast<const std::uint8_t*>(head),
                  sizeof(head) - 1, "/traces/a.csv");
    CHECK(score == 42);
    CHECK(seen->head_size == sizeof(head) - 1);
    CHECK(seen->head_bytes == head);
    CHECK(seen->seen_path == "/traces/a.csv");
    vt->destroy(box);
}

TEST_CASE("a sink writer gets a FrameView of the bound columns") {
    auto self =
        dstd::UniquePtr<RecordingSink>(dstd::make_unique<RecordingSink>());
    RecordingSink* const sink = self.get();
    void* const box = make_sink_self(std::move(self));
    const dftu_sink_vt* const vt = sink_vtable<RecordingSink>();

    // The caller's options tree: an object of one string member.
    dftu_value option_value{};
    option_value.kind = DFTU_VAL_STR;
    option_value.count = 5;
    option_value.as.str = "out.c";
    dftu_member member{};
    member.key = "path";
    member.key_len = 4;
    member.value = &option_value;
    dftu_value options{};
    options.kind = DFTU_VAL_OBJECT;
    options.count = 1;
    options.as.members = &member;

    const char* const names[] = {"id", "val"};
    const std::int32_t types[] = {DFTU_TYPE_INT64, DFTU_TYPE_INT64};
    test_plugin_common::Ctx ctx;
    dftu_ctx* const raw_ctx = ctx.get();
    void* writer = nullptr;
    REQUIRE(vt->bind(raw_ctx, box, names, types, 2, &options, &writer) ==
            DFTU_OK);

    REQUIRE(sink->bind_names.size() == 2);
    CHECK(sink->bind_names[0] == "id");
    CHECK(sink->bind_types[1] == TypeId::Int64);
    CHECK(sink->option == "out.c");

    OwnedDataFrame frame = two_columns();
    REQUIRE(vt->write(raw_ctx, writer, frame.get()) == DFTU_OK);
    CHECK(vt->finalize(raw_ctx, writer) == DFTU_OK);

    const Record& record = *sink->record_;
    CHECK(record.writes == 1);
    CHECK(record.columns == 2);
    CHECK(record.rows == 2);
    REQUIRE(record.names.size() == 2);
    CHECK(record.names[0] == "id");
    CHECK(record.names[1] == "val");
    REQUIRE(record.first_values.size() == 2);
    CHECK(record.first_values[0] == 3);
    CHECK(record.first_values[1] == 20);
    vt->destroy(box);
}

TEST_CASE("typed OpArgs read every operand kind") {
    dftu_op_arg args{};
    args.args[0].i64 = 7;
    args.args[1].f64 = 1.5;
    args.args[2].str = {"args.x", 6};
    args.args[3].i32 = 2;

    const OpArgs operands(&args);
    CHECK(OpArgs::size() == DFTU_OP_MAX_ARGS);
    CHECK(operands.i64(0) == 7);
    CHECK(operands.f64(1) == 1.5);
    CHECK(operands.str(2) == "args.x");
    CHECK(operands.column(2) == "args.x");
    CHECK(operands.column_index(3) == 2);

    // A node given no operands reads a zero rather than dereferencing NULL.
    const OpArgs none(nullptr);
    CHECK(none.i64(0) == 0);
    CHECK(none.column(2).empty());
    CHECK(none.column_index(3) == -1);
}

TEST_CASE("a pushed filter vote reaches the host as its C code") {
    auto self =
        dstd::UniquePtr<VotingSource>(dstd::make_unique<VotingSource>());
    void* const box = make_source_self(std::move(self));
    const dftu_source_vt* const vt = source_vtable<VotingSource>();

    const dftu_expr* filters[2] = {nullptr, nullptr};
    dftu_scan_request request{};
    request.filters = filters;
    request.n_filters = 2;
    request.limit = -1;

    test_plugin_common::Ctx ctx;
    dftu_ctx* const raw_ctx = ctx.get();
    // The host pre-fills every entry with NO and trusts only what comes back.
    std::int32_t pushed[2] = {DFTU_PUSHED_NO, DFTU_PUSHED_NO};
    void* cursor = nullptr;
    const dftu_cursor_vt* cursor_vt = nullptr;
    REQUIRE(vt->scan(raw_ctx, box, nullptr, &request, pushed, &cursor,
                     &cursor_vt) == DFTU_OK);
    CHECK(pushed[0] == DFTU_PUSHED_EXACT);
    // A filter the source did not vote on stays NO, so the host re-applies it.
    CHECK(pushed[1] == DFTU_PUSHED_NO);
    REQUIRE(cursor_vt != nullptr);
    cursor_vt->destroy(cursor);
    vt->destroy(box);
}
