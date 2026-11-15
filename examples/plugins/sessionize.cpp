// SPDX-License-Identifier: MIT
// Example dftracer-utils plugin: a plan node that numbers sessions.
//
// It registers as the node "sessionize.gaps". A session is a run of rows of one
// pid whose start times follow each other closely: the node walks the rows in
// the order they arrive, keeps the last `ts` it saw for each `pid`, and starts
// a new session when the gap to the previous row of that pid is more than
// GAP_US. It appends one column, `session` (int64), counting from 0 per pid, so
// the input columns pass through unchanged.
//
// The state is per pid and lives in the cursor, so a pid that spans several
// frames of the scan keeps its count. A `call` stage passes no arguments to a
// node, so the threshold is the constant below: one second, because a trace's
// `ts` is in microseconds.
//
// Both inputs are read through a materialized flat view: dftu_series_data
// answers NULL for every other encoding, so a `where` or a `select` above the
// node (which hands it filtered or dictionary columns) would otherwise fail.
// dftu_series_materialize shares the buffers of a column that is already flat,
// so the common case allocates a handle and copies no data. `pid` and `ts`
// may be int64, uint64, int32, uint32, a TIMESTAMP or a DURATION. A row whose
// `pid` or `ts` is null is skipped: it takes no part in the walk, does not
// update a pid's last `ts`, and gets the session -1, so a null never opens,
// extends or breaks a session.
//
// Use: dftracer_view ./traces -q 'select pid, ts | call sessionize.gaps()'
//
// Build: c++ -std=c++20 -shared -fPIC -I<repo>/include -o sessionize.so
// sessionize.cpp

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/node.h>

#include <cstdint>
#include <utility>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

constexpr std::int64_t GAP_US = 1000000;
/// The session of a row whose pid or ts is null: not part of any session.
constexpr std::int64_t NO_SESSION = -1;

/// How the payload of an integer, timestamp or duration column is stored.
enum class IntKind { Int64, Uint64, Int32, Uint32 };

/// The reader for @p type, or nothing for a type that is not an integer.
dstd::Optional<IntKind> int_kind(dataframe::TypeId type) {
    switch (type) {
        case dataframe::TypeId::Int64:
        case dataframe::TypeId::Timestamp:
        case dataframe::TypeId::Duration:
            return IntKind::Int64;
        case dataframe::TypeId::Uint64:
            return IntKind::Uint64;
        case dataframe::TypeId::Int32:
            return IntKind::Int32;
        case dataframe::TypeId::Uint32:
            return IntKind::Uint32;
        default:
            return dstd::Optional<IntKind>();
    }
}

std::int64_t int_at(const void* data, IntKind kind, std::int64_t row) {
    switch (kind) {
        case IntKind::Int64:
            return static_cast<const std::int64_t*>(data)[row];
        case IntKind::Uint64:
            return static_cast<std::int64_t>(
                static_cast<const std::uint64_t*>(data)[row]);
        case IntKind::Int32:
            return static_cast<const std::int32_t*>(data)[row];
        case IntKind::Uint32:
            return static_cast<std::int64_t>(
                static_cast<const std::uint32_t*>(data)[row]);
    }
    return 0;
}

struct LastSeen {
    std::int64_t ts = 0;
    std::int64_t session = 0;
};

class SessionGaps {
   public:
    void output_schema(const SchemaView& in, const OpArgs&,
                       SchemaBuilder& out) const {
        in.copy_all(out);
        out.add("session", DFTU_TYPE_INT64, false);
    }

    struct Cursor {
        InputCursor in;
        dstd::HashMap<std::int64_t, LastSeen> seen;

        dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t rows) {
            DFTRACER_UTILS_TRY(dstd::Optional<OwnedDataFrame> frame,
                               in.next(rows));
            if (!frame) return dstd::Optional<OwnedDataFrame>();

            Column pid(dftu_dataframe_column(frame->get(), "pid"));
            Column ts(dftu_dataframe_column(frame->get(), "ts"));
            if (!pid || !ts)
                return dstd::Unexpected(invalid_argument(
                    "sessionize.gaps needs the columns pid and ts"));
            const dstd::Optional<IntKind> pid_kind = int_kind(pid.dtype());
            const dstd::Optional<IntKind> ts_kind = int_kind(ts.dtype());
            if (!pid_kind || !ts_kind)
                return dstd::Unexpected(invalid_argument(
                    "sessionize.gaps reads pid and ts as integers"));
            // A flat column is shared, not gathered; anything else is.
            Column pid_flat(dftu_series_materialize(pid.get()));
            Column ts_flat(dftu_series_materialize(ts.get()));
            if (!pid_flat || !ts_flat)
                return dstd::Unexpected(invalid_argument(
                    "sessionize.gaps cannot flatten its input"));
            const void* pid_data = dftu_series_data(pid_flat.get());
            const void* ts_data = dftu_series_data(ts_flat.get());
            if (pid_data == nullptr || ts_data == nullptr)
                return dstd::Unexpected(invalid_argument(
                    "sessionize.gaps reads pid and ts as integers"));

            const std::int64_t count = pid_flat.size();
            const bool has_nulls = dftu_series_null_count(pid_flat.get()) > 0 ||
                                   dftu_series_null_count(ts_flat.get()) > 0;
            dstd::Vector<std::int64_t> sessions(static_cast<std::size_t>(count),
                                                NO_SESSION);
            for (std::int64_t row = 0; row < count; ++row) {
                if (has_nulls &&
                    (dftu_series_is_null(pid_flat.get(), row) != 0 ||
                     dftu_series_is_null(ts_flat.get(), row) != 0))
                    continue;
                const std::int64_t key = int_at(pid_data, *pid_kind, row);
                const std::int64_t start = int_at(ts_data, *ts_kind, row);
                auto seen_at = seen.find(key);
                if (seen_at == seen.end()) {
                    seen.emplace(key, LastSeen{start, 0});
                    sessions[static_cast<std::size_t>(row)] = 0;
                    continue;
                }
                if (start - seen_at->second.ts > GAP_US)
                    ++seen_at->second.session;
                seen_at->second.ts = start;
                sessions[static_cast<std::size_t>(row)] =
                    seen_at->second.session;
            }

            FrameBuilder builder;
            const std::int32_t columns =
                dftu_dataframe_num_columns(frame->get());
            for (std::int32_t i = 0; i < columns; ++i) {
                const char* name = dftu_dataframe_column_name(frame->get(), i);
                if (name == nullptr)
                    return dstd::Unexpected(invalid_argument(
                        "sessionize.gaps cannot read its input"));
                builder.add(name,
                            Column(dftu_dataframe_column(frame->get(), name)));
            }
            builder.add("session", Column::from_int64(std::move(sessions)));
            auto out = builder.build();
            if (!out)
                return dstd::Unexpected(
                    invalid_argument("sessionize.gaps cannot build its frame"));
            return dstd::Optional<OwnedDataFrame>(std::move(*out));
        }
    };

    dstd::Result<dstd::UniquePtr<Cursor>> open(InputCursor in, const OpArgs&,
                                               std::uint64_t,
                                               const Host&) const {
        return dstd::make_unique<Cursor>(Cursor{std::move(in), {}});
    }
};

}  // namespace

DFTU_PLUGIN("sessionize", "1.0",
            [](PluginBuilder& p, const Config& config) -> dstd::Result<void> {
                (void)config;
                p.node<SessionGaps>("sessionize.gaps",
                                    dstd::make_unique<SessionGaps>());
                return {};
            })
