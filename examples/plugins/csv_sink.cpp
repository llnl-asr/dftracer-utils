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

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/plugins/plugin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

const char* option_of(const dftu_value* options, const char* key) {
    if (options == nullptr || options->kind != DFTU_VAL_OBJECT) return nullptr;
    for (std::uint64_t i = 0; i < options->count; ++i) {
        const dftu_value* value = options->as.members[i].value;
        if (value != nullptr && value->kind == DFTU_VAL_STR &&
            std::strcmp(options->as.members[i].key, key) == 0)
            return value->as.str;
    }
    return nullptr;
}

// The column `index` of `frame`, flattened. Null when the C ABI refuses it.
Column flat_column(const dftu_dataframe* frame, std::int32_t index,
                   dstd::String* name) {
    const char* text = dftu_dataframe_column_name(frame, index);
    if (text == nullptr) return Column();
    *name = dstd::String(text);
    Column column(dftu_dataframe_column(frame, name->c_str()));
    if (!column) return Column();
    return Column(dftu_series_materialize(column.get()));
}

void put_int(std::FILE* file, const dftu_series* column, std::int32_t type,
             std::int64_t row) {
    const void* data = dftu_series_data(column);
    if (data == nullptr) return;
    switch (type) {
        case DFTU_TYPE_INT8:
            std::fprintf(
                file, "%d",
                static_cast<int>(static_cast<const std::int8_t*>(data)[row]));
            return;
        case DFTU_TYPE_INT16:
            std::fprintf(
                file, "%d",
                static_cast<int>(static_cast<const std::int16_t*>(data)[row]));
            return;
        case DFTU_TYPE_INT32:
            std::fprintf(file, "%d",
                         static_cast<const std::int32_t*>(data)[row]);
            return;
        case DFTU_TYPE_UINT8:
            std::fprintf(file, "%u",
                         static_cast<unsigned>(
                             static_cast<const std::uint8_t*>(data)[row]));
            return;
        case DFTU_TYPE_UINT16:
            std::fprintf(file, "%u",
                         static_cast<unsigned>(
                             static_cast<const std::uint16_t*>(data)[row]));
            return;
        case DFTU_TYPE_UINT32:
            std::fprintf(file, "%u",
                         static_cast<const std::uint32_t*>(data)[row]);
            return;
        case DFTU_TYPE_UINT64:
            std::fprintf(file, "%llu",
                         static_cast<unsigned long long>(
                             static_cast<const std::uint64_t*>(data)[row]));
            return;
        default:
            std::fprintf(file, "%lld",
                         static_cast<long long>(
                             static_cast<const std::int64_t*>(data)[row]));
            return;
    }
}

void put_value(std::FILE* file, const dftu_series* column, std::int64_t row) {
    const std::int32_t type = dftu_series_type(column);
    if (type == DFTU_TYPE_STRING) {
        std::int64_t size = 0;
        const char* text = dftu_series_string_at(column, row, &size);
        if (text == nullptr) return;
        // A field holding a comma, a quote or a newline is quoted, and a
        // quote inside it doubles.
        const bool quote = std::strpbrk(text, ",\"\n") != nullptr;
        if (!quote) {
            std::fwrite(text, 1, static_cast<std::size_t>(size), file);
            return;
        }
        std::fputc('"', file);
        for (std::int64_t i = 0; i < size; ++i) {
            if (text[i] == '"') std::fputc('"', file);
            std::fputc(text[i], file);
        }
        std::fputc('"', file);
    } else if (type == DFTU_TYPE_FLOAT32) {
        const auto* data = static_cast<const float*>(dftu_series_data(column));
        if (data != nullptr)
            std::fprintf(file, "%g", static_cast<double>(data[row]));
    } else if (type == DFTU_TYPE_FLOAT64) {
        const auto* data = static_cast<const double*>(dftu_series_data(column));
        if (data != nullptr) std::fprintf(file, "%g", data[row]);
    } else if (type == DFTU_TYPE_BOOL) {
        const auto* data =
            static_cast<const std::uint8_t*>(dftu_series_data(column));
        if (data != nullptr)
            std::fputs(data[row] != 0 ? "true" : "false", file);
    } else {
        put_int(file, column, type, row);
    }
}

class CsvWriter : public SinkWriter {
   public:
    explicit CsvWriter(std::FILE* file) : file_(file) {}
    ~CsvWriter() override {
        if (file_ != nullptr) std::fclose(file_);
    }

    dstd::Result<void> write(const dftu_dataframe* frame) override {
        if (frame == nullptr || file_ == nullptr)
            return invalid_argument(dstd::fmt<"csv: no frame">());
        const std::int32_t columns = dftu_dataframe_num_columns(frame);
        const std::int64_t rows = dftu_dataframe_num_rows(frame);
        dstd::Vector<Column> already;
        for (std::int32_t i = 0; i < columns; ++i) {
            dstd::String name;
            Column column = flat_column(frame, i, &name);
            if (!column)
                return invalid_argument(dstd::fmt<"csv: no column {}">(i));
            already.push_back(std::move(column));
            if (!header_) {
                if (i != 0) std::fputc(',', file_);
                std::fputs(name.c_str(), file_);
            }
        }
        if (!header_) {
            std::fputc('\n', file_);
            header_ = true;
        }
        for (std::int64_t row = 0; row < rows; ++row) {
            for (std::int32_t i = 0; i < columns; ++i) {
                if (i != 0) std::fputc(',', file_);
                put_value(file_, already[static_cast<std::size_t>(i)].get(),
                          row);
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
};

class CsvSink : public Sink {
   public:
    dstd::Result<dstd::UniquePtr<SinkWriter>> bind(
        const BindView& view) override {
        const char* path = option_of(view.options, "path");
        if (path == nullptr || path[0] == '\0')
            return invalid_argument(
                dstd::fmt<"csv.write: bind needs the option path, e.g. path "
                          "\"out.csv\"">());
        std::FILE* file = std::fopen(path, "wb");
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
