// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: a table built in code, not read from a file.
//
// It registers as the provider "range_provider.numbers". The table has `rows`
// rows (config key, 10 by default) and two int64 columns: `i` counts from 0 and
// `square` is `i * i`. A projection keeps only the columns asked for, the scan
// hands out the rows in frames of BATCH_ROWS, and the cursor carries the
// position, so a collect pulls the whole table without holding it in memory.
// Filters, sorting and the other apply_* hooks are left out: the engine does
// that work above the provider.
//
// Use: dftracer_view --provider range_provider.numbers -q 'select square |
// where i > 3'
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o range_provider.so
// range_provider.cpp

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/source.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t BATCH_ROWS = 1024;
constexpr std::int64_t DEFAULT_ROWS = 10;

class RangeTable {
   public:
    explicit RangeTable(std::int64_t rows) : rows_(rows) {}

    dstd::Vector<dstd::String> names() const {
        dstd::Vector<dstd::String> names;
        names.emplace_back("i");
        names.emplace_back("square");
        return names;
    }

    void schema(SchemaBuilder& out) const {
        out.add("i", DFTU_TYPE_INT64, false);
        out.add("square", DFTU_TYPE_INT64, false);
    }

    struct Cursor {
        std::int64_t rows = 0;
        /// The requested projection as an ordered list, repeats kept; empty
        /// means every column in schema order.
        dstd::Vector<dstd::String> wanted;
        std::int64_t at = 0;

        dstd::Result<dstd::Optional<OwnedDataFrame>> next(
            std::int64_t max_rows) {
            if (at >= rows) return dstd::Optional<OwnedDataFrame>();
            std::int64_t take = rows - at;
            if (max_rows > 0 && max_rows < take) take = max_rows;
            if (take > BATCH_ROWS) take = BATCH_ROWS;

            FrameBuilder builder;
            if (wanted.empty()) {
                add_column(builder, "i", take);
                add_column(builder, "square", take);
            } else {
                // Emit in the requested order, so a repeat gives the column
                // twice, as the projection contract requires.
                for (const dstd::String& name : wanted)
                    add_column(builder, name, take);
            }
            auto out = builder.build();
            if (!out)
                return dstd::Unexpected(
                    invalid_argument("range_provider: cannot build a frame"));
            at += take;
            return dstd::Optional<OwnedDataFrame>(std::move(*out));
        }

       private:
        void add_column(FrameBuilder& builder, dstd::StringView name,
                        std::int64_t take) {
            const bool squared = name == "square";
            dstd::Vector<std::int64_t> values(static_cast<std::size_t>(take));
            for (std::int64_t i = 0; i < take; ++i) {
                const std::int64_t value = at + i;
                values[static_cast<std::size_t>(i)] =
                    squared ? value * value : value;
            }
            builder.add(name, Column::from_int64(std::move(values)));
        }
    };

    dstd::Result<ScanResult<Cursor>> scan(const ScanView& view,
                                          const Host&) const {
        Cursor cursor;
        cursor.rows = rows_;
        for (const dstd::StringView& wanted : view.projection) {
            if (wanted == "i" || wanted == "square") {
                cursor.wanted.emplace_back(wanted);
            } else {
                dstd::String message("range_provider: no column ");
                message += wanted;
                return dstd::Unexpected(
                    invalid_argument(dstd::StringView(message)));
            }
        }

        ScanResult<Cursor> result;
        result.filters.assign(view.filters.size(), DFTU_PUSHED_NO);
        result.cursor = dstd::UniquePtr<Cursor>(
            dstd::make_unique<Cursor>(std::move(cursor)));
        return result;
    }

   private:
    std::int64_t rows_;
};

}  // namespace

DFTU_PLUGIN("range_provider", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                const std::int64_t rows = config.get_int("rows", DEFAULT_ROWS);
                p.source<RangeTable>("range_provider.numbers",
                                     dstd::make_unique<RangeTable>(rows));
                return {};
            })
