// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: a file format that reads a CSV file into
// columns the query engine can scan.
//
// It registers as the format "csv_reader", the plugin's own bare name, and
// claims a file whose name ends in .csv and whose first line looks like a CSV
// header: letters, digits, underscores, spaces and commas, with at least one
// comma. The header names the columns and every column is typed from its data:
// int64 when every cell parses as an integer, double when every cell parses as
// a number, string otherwise.
//
// The rows are split into chunks of ROWS_PER_CHUNK, so a scan opens one cursor
// per chunk and the engine reads the chunks in parallel. A projection keeps
// exactly the columns the caller asked for, in its order. Filters are left to
// the engine: this format votes on none of them.
//
// Use: dftracer_view people.csv -q 'select name, age | where age > 30'
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o csv_reader.so
// csv_reader.cpp

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/file_format.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t ROWS_PER_CHUNK = 1024;

using Fields = dstd::Vector<dstd::StringView>;

bool is_integer(dstd::StringView text, std::int64_t* out = nullptr) {
    const dstd::Optional<std::int64_t> value =
        dstd::from_chars<std::int64_t>(text);
    if (!value) return false;
    if (out != nullptr) *out = *value;
    return true;
}

bool is_number(dstd::StringView text, double* out = nullptr) {
    const dstd::Optional<double> value = dstd::from_chars<double>(text);
    if (!value) return false;
    if (out != nullptr) *out = *value;
    return true;
}

// The cells of one line, spaces around each cell dropped. A quoted cell is not
// honoured: this format keeps to the plain subset its probe accepts.
Fields split_fields(dstd::StringView line) {
    Fields fields;
    std::size_t at = 0;
    while (true) {
        const std::size_t comma = line.find(',', at);
        std::size_t end = comma == dstd::StringView::npos ? line.size() : comma;
        std::size_t begin = at;
        while (begin < end && (line[begin] == ' ' || line[begin] == '\t'))
            ++begin;
        while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t'))
            --end;
        fields.push_back(line.substr(begin, end - begin));
        if (comma == dstd::StringView::npos) break;
        at = comma + 1;
    }
    return fields;
}

// The line without its line ending. The body is a file's whole text.
dstd::StringView next_line(dstd::StringView body, std::size_t* at) {
    const std::size_t start = *at;
    const std::size_t newline = body.find('\n', start);
    std::size_t end = newline == dstd::StringView::npos ? body.size() : newline;
    *at = newline == dstd::StringView::npos ? body.size() : newline + 1;
    if (end > start && body[end - 1] == '\r') --end;
    return body.substr(start, end - start);
}

struct CsvColumn {
    dstd::String name;
    TypeId type = TypeId::String;
    dstd::Vector<dstd::String> cells;  ///< one per row of the file
};

TypeId type_of(dstd::Span<const dstd::String> cells) {
    if (cells.empty()) return TypeId::String;
    bool integral = true;
    bool numeric = true;
    for (const dstd::String& cell : cells) {
        const dstd::StringView text(cell);
        if (integral && !is_integer(text)) integral = false;
        if (numeric && !is_number(text)) numeric = false;
        if (!integral && !numeric) return TypeId::String;
    }
    if (integral) return TypeId::Int64;
    if (numeric) return TypeId::Float64;
    return TypeId::String;
}

Column make_column(const CsvColumn& column, std::int64_t from, std::int64_t n) {
    if (column.type == TypeId::String) {
        dstd::Vector<std::int32_t> offsets;
        dstd::String data;
        offsets.reserve(static_cast<std::size_t>(n) + 1);
        offsets.push_back(0);
        for (std::int64_t i = 0; i < n; ++i) {
            const dstd::String& cell =
                column.cells[static_cast<std::size_t>(from + i)];
            data.append(cell.data(), cell.size());
            offsets.push_back(static_cast<std::int32_t>(data.size()));
        }
        return Column::from_strings(std::move(offsets), std::move(data));
    }
    if (column.type == TypeId::Int64) {
        dstd::Vector<std::int64_t> values(static_cast<std::size_t>(n), 0);
        for (std::int64_t i = 0; i < n; ++i)
            is_integer(column.cells[static_cast<std::size_t>(from + i)],
                       &values[static_cast<std::size_t>(i)]);
        return Column::from_int64(std::move(values));
    }
    dstd::Vector<double> values(static_cast<std::size_t>(n), 0.0);
    for (std::int64_t i = 0; i < n; ++i)
        is_number(column.cells[static_cast<std::size_t>(from + i)],
                  &values[static_cast<std::size_t>(i)]);
    return Column::from_double(std::move(values));
}

class CsvCursor : public FormatCursor {
   public:
    /// @p picked indexes into @p columns, in projection order (a repeated name
    /// gives its column twice); @p base is the first row of the chunk this
    /// cursor hands out, and @p rows the chunk's row count. The table is
    /// shared, not copied: a frame reads its chunk's rows straight out of it.
    CsvCursor(dstd::SharedPtr<const dstd::Vector<CsvColumn>> columns,
              dstd::Vector<std::size_t> picked, std::int64_t base,
              std::int64_t rows)
        : columns_(std::move(columns)),
          picked_(std::move(picked)),
          base_(base),
          rows_(rows) {}

    dstd::Result<dstd::Optional<OwnedDataFrame>> next(
        std::int64_t max_rows) override {
        if (row_ >= rows_) return dstd::Optional<OwnedDataFrame>();
        std::int64_t take = rows_ - row_;
        if (max_rows > 0 && max_rows < take) take = max_rows;
        OwnedDataFrame frame = build(take);
        if (!frame)
            return dstd::Unexpected(
                invalid_argument("csv_reader: cannot build a frame"));
        row_ += take;
        return dstd::Optional<OwnedDataFrame>(std::move(frame));
    }

   private:
    OwnedDataFrame build(std::int64_t n) {
        FrameBuilder builder;
        for (const std::size_t at : picked_) {
            const CsvColumn& column = (*columns_)[at];
            builder.add(column.name, make_column(column, base_ + row_, n));
        }
        auto built = builder.build();
        return built ? std::move(*built) : OwnedDataFrame();
    }

    dstd::SharedPtr<const dstd::Vector<CsvColumn>> columns_;
    dstd::Vector<std::size_t> picked_;
    std::int64_t base_ = 0;
    std::int64_t rows_ = 0;
    std::int64_t row_ = 0;
};

class CsvFile : public FormatFile {
   public:
    CsvFile(dstd::Vector<CsvColumn> columns, std::int64_t rows)
        : columns_(dstd::make_shared<const dstd::Vector<CsvColumn>>(
              std::move(columns))),
          rows_(rows) {}

    void schema(SchemaBuilder& out) const override {
        for (const CsvColumn& column : *columns_)
            out.add(column.name.c_str(), column.type, false);
    }

    std::uint64_t chunks() const override {
        const std::int64_t chunks =
            (rows_ + ROWS_PER_CHUNK - 1) / ROWS_PER_CHUNK;
        return static_cast<std::uint64_t>(chunks < 1 ? 1 : chunks);
    }

    dstd::Optional<std::int64_t> chunk_rows(
        std::uint64_t chunk) const override {
        const std::int64_t first =
            static_cast<std::int64_t>(chunk) * ROWS_PER_CHUNK;
        if (first >= rows_) return std::int64_t{0};
        const std::int64_t left = rows_ - first;
        return left < ROWS_PER_CHUNK ? left : ROWS_PER_CHUNK;
    }

    dstd::Result<ScanResult<FormatCursor>> scan(std::uint64_t chunk,
                                                const ScanView& view) override {
        // Resolve the projection to column indexes, in its order, and let the
        // cursor read the chunk's rows out of the shared table: no whole
        // column is copied per chunk.
        dstd::Vector<std::size_t> picked;
        if (view.projection.empty()) {
            for (std::size_t i = 0; i < columns_->size(); ++i)
                picked.push_back(i);
        } else {
            for (const dstd::StringView& wanted : view.projection) {
                std::size_t found = columns_->size();
                for (std::size_t i = 0; i < columns_->size(); ++i)
                    if (dstd::StringView((*columns_)[i].name) == wanted) {
                        found = i;
                        break;
                    }
                if (found == columns_->size()) {
                    dstd::String message("csv_reader: no column ");
                    message += wanted;
                    return dstd::Unexpected(
                        invalid_argument(dstd::StringView(message)));
                }
                picked.push_back(found);
            }
        }

        const std::int64_t first =
            static_cast<std::int64_t>(chunk) * ROWS_PER_CHUNK;
        const std::int64_t end =
            first + ROWS_PER_CHUNK < rows_ ? first + ROWS_PER_CHUNK : rows_;
        const std::int64_t rows = first < end ? end - first : 0;

        ScanResult<FormatCursor> result;
        result.filters.assign(view.filters.size(), Pushed::No);
        result.cursor =
            dstd::UniquePtr<FormatCursor>(dstd::make_unique<CsvCursor>(
                columns_, std::move(picked), first, rows));
        return result;
    }

   private:
    dstd::SharedPtr<const dstd::Vector<CsvColumn>> columns_;
    std::int64_t rows_ = 0;
};

class CsvFormat : public FileFormat {
   public:
    std::int32_t probe(dstd::ByteView head, dstd::CStringView path) override {
        if (!dstd::StringView(path.c_str()).ends_with(".csv")) return 0;
        const dstd::StringView text(reinterpret_cast<const char*>(head.data()),
                                    head.size());
        std::size_t at = 0;
        const dstd::StringView first = next_line(text, &at);
        if (first.empty() || first.find(',') == dstd::StringView::npos)
            return 0;
        for (const char c : first)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == ' ' || c == ','))
                return 0;
        return 80;
    }

    dstd::Result<dstd::UniquePtr<FormatFile>> open(
        dstd::StringView path) override {
        auto text = dstd::fs::read_file(dstd::PathView(path));
        if (!text)
            return dstd::Unexpected(
                io_error("csv_reader: cannot read the file"));

        const dstd::StringView body(*text);
        std::size_t at = 0;
        const Fields header = split_fields(next_line(body, &at));
        if (header.empty())
            return dstd::Unexpected(
                invalid_argument("csv_reader: the file has no header"));

        dstd::Vector<CsvColumn> columns;
        columns.reserve(header.size());
        for (const dstd::StringView& name : header) {
            CsvColumn column;
            column.name.assign(name.data(), name.size());
            columns.push_back(std::move(column));
        }

        std::int64_t rows = 0;
        while (at < body.size()) {
            const dstd::StringView line = next_line(body, &at);
            if (line.empty()) continue;
            const Fields cells = split_fields(line);
            if (cells.size() != header.size())
                return dstd::Unexpected(
                    invalid_argument("csv_reader: a row has a different number "
                                     "of cells than the header"));
            for (std::size_t i = 0; i < cells.size(); ++i)
                columns[i].cells.emplace_back(cells[i].data(), cells[i].size());
            ++rows;
        }

        for (CsvColumn& column : columns) column.type = type_of(column.cells);

        dstd::UniquePtr<FormatFile> file =
            dstd::make_unique<CsvFile>(std::move(columns), rows);
        return file;
    }
};

}  // namespace

DFTU_PLUGIN("csv_reader", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.file_format<CsvFormat>("csv_reader",
                                         dstd::make_unique<CsvFormat>());
                return {};
            })
