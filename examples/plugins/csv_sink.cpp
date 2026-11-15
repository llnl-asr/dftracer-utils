// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: a sink that writes every frame it is given to
// one CSV file, the column names as the header line. It registers as
// "csv.write" and reads one option:
//
//   path   the file to write (required)
//
// Use: dftracer_view ./traces -q '| where dur > 1000' --plugin ./csv.dylib
//                                             --sink csv.write --sink-option
//                                             path=out.csv
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o csv.dylib
// csv_sink.cpp

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

// One value of a numeric column, printed the way its own type reads: an
// unsigned type unsigned, a floating type as %g. A row past the end of the
// values prints nothing, which is how a column of another length shows.
template <class T>
void put_number(std::FILE* file, ColumnView column, std::int64_t row) {
    const dstd::Span<const T> values = column.values<T>();
    if (row < 0 || static_cast<std::size_t>(row) >= values.size()) return;
    const T value = values[static_cast<std::size_t>(row)];
    if constexpr (std::is_floating_point_v<T>) {
        std::fprintf(file, "%g", static_cast<double>(value));
    } else if constexpr (std::is_unsigned_v<T>) {
        std::fprintf(file, "%llu", static_cast<unsigned long long>(value));
    } else {
        std::fprintf(file, "%lld", static_cast<long long>(value));
    }
}

void put_value(std::FILE* file, ColumnView column, std::int64_t row) {
    switch (column.type()) {
        case TypeId::String: {
            const dstd::StringView text = column.string_at(row);
            if (text.data() == nullptr) return;
            // A field holding a comma, a quote or a newline is quoted, and a
            // quote inside it doubles.
            const bool quote =
                text.find_first_of(",\"\n") != dstd::StringView::npos;
            if (!quote) {
                std::fwrite(text.data(), 1, text.size(), file);
                return;
            }
            std::fputc('"', file);
            for (const char c : text) {
                if (c == '"') std::fputc('"', file);
                std::fputc(c, file);
            }
            std::fputc('"', file);
            return;
        }
        case TypeId::Float32:
            put_number<float>(file, column, row);
            return;
        case TypeId::Float64:
            put_number<double>(file, column, row);
            return;
        case TypeId::Bool:
            std::fputs(column.boolean_at(row) ? "true" : "false", file);
            return;
        case TypeId::Int8:
            put_number<std::int8_t>(file, column, row);
            return;
        case TypeId::Int16:
            put_number<std::int16_t>(file, column, row);
            return;
        case TypeId::Int32:
        case TypeId::Date32:
        case TypeId::Time32:
            put_number<std::int32_t>(file, column, row);
            return;
        case TypeId::Uint8:
            put_number<std::uint8_t>(file, column, row);
            return;
        case TypeId::Uint16:
            put_number<std::uint16_t>(file, column, row);
            return;
        case TypeId::Uint32:
            put_number<std::uint32_t>(file, column, row);
            return;
        case TypeId::Uint64:
            put_number<std::uint64_t>(file, column, row);
            return;
        default:
            put_number<std::int64_t>(file, column, row);
            return;
    }
}

class CsvWriter : public SinkWriter {
   public:
    explicit CsvWriter(std::FILE* file) : file_(file) {}
    ~CsvWriter() override {
        if (file_ != nullptr) std::fclose(file_);
    }

    dstd::Result<void> write(FrameView frame) override {
        if (file_ == nullptr)
            return invalid_argument(dstd::fmt<"csv: no frame">());
        const std::int32_t columns = frame.column_count();
        const std::int64_t rows = frame.row_count();
        // A column the batch holds in another encoding is flattened once, so
        // values<T>() reads a flat span; a column that already is FLAT is read
        // in place.
        held_.clear();
        held_.reserve(static_cast<std::size_t>(columns));
        views_.clear();
        views_.reserve(static_cast<std::size_t>(columns));
        for (std::int32_t i = 0; i < columns; ++i) {
            const ColumnView column = frame.column(i);
            if (!header_) {
                if (i != 0) std::fputc(',', file_);
                const dstd::StringView name = frame.name(i);
                std::fwrite(name.data(), 1, name.size(), file_);
            }
            switch (column.type()) {
                case TypeId::String:
                    views_.push_back(column);
                    break;
                default: {
                    dstd::Result<Column> flat = column.flat();
                    if (!flat)
                        return invalid_argument(
                            dstd::fmt<"csv: no column {}">(i));
                    held_.push_back(std::move(*flat));
                    views_.push_back(ColumnView(held_.back().get()));
                    break;
                }
            }
        }
        if (!header_) {
            std::fputc('\n', file_);
            header_ = true;
        }
        for (std::int64_t row = 0; row < rows; ++row) {
            for (std::int32_t i = 0; i < columns; ++i) {
                if (i != 0) std::fputc(',', file_);
                put_value(file_, views_[static_cast<std::size_t>(i)], row);
            }
            std::fputc('\n', file_);
            if (std::ferror(file_) != 0)
                return invalid_argument(dstd::fmt<"csv: write failed">());
        }
        return {};
    }

    dstd::Result<void> finalize() override {
        if (file_ == nullptr)
            return invalid_argument(dstd::fmt<"csv: already closed">());
        if (std::fflush(file_) != 0 || std::ferror(file_) != 0)
            return invalid_argument(dstd::fmt<"csv: write failed">());
        std::fclose(file_);
        file_ = nullptr;
        return {};
    }

    void abort() override {
        if (file_ == nullptr) return;
        std::fclose(file_);
        file_ = nullptr;
    }

   private:
    std::FILE* file_ = nullptr;
    bool header_ = false;
    dstd::Vector<Column> held_;
    dstd::Vector<ColumnView> views_;
};

class CsvSink : public Sink {
   public:
    dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        const BindView& view) override {
        const dstd::StringView option = view.options.get("path");
        if (option.empty())
            return invalid_argument(
                dstd::fmt<"csv.write: bind needs the option path, e.g. path "
                          "\"out.csv\"">());
        // The config text is a view into the caller's tree and is not
        // NUL-terminated, so the path is copied once here.
        const dstd::String path(option);
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr)
            return invalid_argument(dstd::fmt<"csv: cannot open {}">(path));
        return dstd::UniquePtr<SinkWriter>(dstd::make_unique<CsvWriter>(file));
    }
};

}  // namespace

DFTU_PLUGIN("csv", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.sink("csv.write",
                       dstd::UniquePtr<Sink>(dstd::make_unique<CsvSink>()));
                return {};
            })
