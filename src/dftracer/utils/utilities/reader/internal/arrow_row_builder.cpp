#include <dftracer/utils/utilities/reader/internal/arrow_row_builder.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/schema.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dftracer::utils::utilities::reader::internal {

using common::arrow::ColumnType;
using common::arrow::RecordBatchBuilder;
using json::JsonParser;
using json::JsonValueHelper;

namespace {

// --- Row type constants (must match Python TYPE_* constants) ---
enum RowType : std::int8_t {
    ROW_EVENT = 0,
    ROW_FILE_HASH = 1,
    ROW_HOST_HASH = 2,
    ROW_STRING_HASH = 3,
    ROW_METADATA = 4,
    ROW_PROC_METADATA = 5,
    ROW_PROFILE = 6,
    ROW_SYSTEM = 7,
};

// --- IO category constants (must match Python IOCategory values) ---
enum IOCat : std::int8_t {
    IO_READ = 1,
    IO_WRITE = 2,
    IO_METADATA = 3,
    IO_PCTL = 4,
    IO_IPC = 5,
    IO_OTHER = 6,
    IO_SYNC = 7,
};

std::int8_t get_io_cat(std::string_view func) {
    using namespace dftracer::utils::trace::internal;
    // Op sets are disjoint, so a single flat lookup preserves the original
    // first-match semantics. Keys view the constexpr op arrays (static
    // storage).
    static const ankerl::unordered_dense::map<std::string_view, std::int8_t>
        op_to_cat = [] {
            ankerl::unordered_dense::map<std::string_view, std::int8_t> m;
            for (auto op : posix_ops::READ) m.emplace(op, IO_READ);
            for (auto op : posix_ops::WRITE) m.emplace(op, IO_WRITE);
            for (auto op : posix_ops::SYNC) m.emplace(op, IO_SYNC);
            for (auto op : posix_ops::PCTL) m.emplace(op, IO_PCTL);
            for (auto op : posix_ops::IPC) m.emplace(op, IO_IPC);
            for (auto op : posix_ops::METADATA) m.emplace(op, IO_METADATA);
            return m;
        }();
    auto it = op_to_cat.find(func);
    return it != op_to_cat.end() ? it->second
                                 : static_cast<std::int8_t>(IO_OTHER);
}

bool str_iequal(std::string_view a, const char *b) {
    std::size_t len = std::strlen(b);
    if (a.size() != len) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            static_cast<unsigned char>(b[i]))
            return false;
    }
    return true;
}

// Fields extracted from a row's "args" object in a single pass.
struct ParsedArgs {
    std::optional<std::string_view> name, value, hhash, fhash;
    std::optional<std::int64_t> epoch, step, size_sum, ret;
    std::optional<std::int64_t> offset, image_idx, image_size;
    // Other int/float args, kept for profile/sys columns.
    std::unordered_map<std::string, std::int64_t> int_map;
    std::unordered_map<std::string, double> float_map;
};

// Parse a row's "args" object (already located during the single top-level
// walk) into the extracted fields. Mirrors the previous per-key dispatch.
ParsedArgs parse_row_args(simdjson::ondemand::value &args_val) {
    using SVH = JsonValueHelper;
    ParsedArgs a;
    auto obj = args_val.get_object();
    if (obj.error()) return a;
    for (auto field : obj.value_unsafe()) {
        if (field.error()) continue;
        auto key_r = field.unescaped_key();
        if (key_r.error()) continue;
        std::string_view key = key_r.value_unsafe();
        auto val_r = field.value();
        if (val_r.error()) continue;
        auto val = val_r.value_unsafe();
        if (key == "name") {
            if (auto s = SVH::get_string(val)) a.name = s;
        } else if (key == "value") {
            if (auto s = SVH::get_string(val)) a.value = s;
        } else if (key == "hhash") {
            if (auto s = SVH::get_string(val)) a.hhash = s;
        } else if (key == "fhash") {
            if (auto s = SVH::get_string(val)) a.fhash = s;
        } else if (key == "epoch") {
            if (auto i = SVH::get_int64(val)) a.epoch = i;
        } else if (key == "step") {
            if (auto i = SVH::get_int64(val)) a.step = i;
        } else if (key == "size_sum") {
            if (auto i = SVH::get_int64(val)) a.size_sum = i;
        } else if (key == "ret") {
            if (auto i = SVH::get_int64(val)) a.ret = i;
        } else if (key == "offset") {
            if (auto i = SVH::get_int64(val)) a.offset = i;
        } else if (key == "image_idx") {
            if (auto i = SVH::get_int64(val)) a.image_idx = i;
        } else if (key == "image_size") {
            if (auto i = SVH::get_int64(val)) a.image_size = i;
        } else {
            if (auto i = SVH::get_int64(val)) {
                a.int_map[std::string(key)] = *i;
            } else if (auto d = SVH::get_double(val)) {
                a.float_map[std::string(key)] = *d;
            }
        }
    }
    return a;
}

void append_profile_columns(
    RecordBatchBuilder &builder,
    const std::unordered_map<std::string, std::int64_t> &int_map) {
    static const char *profile_keys[] = {
        "count",      "count_max",  "count_min",  "count_sum",  "dftu_cnt",
        "dur",        "dur_max",    "dur_min",    "dur_sum",    "epoch",
        "flags",      "offset",     "offset_max", "offset_min", "offset_sum",
        "ret",        "ret_max",    "ret_min",    "ret_sum",    "whence",
        "whence_max", "whence_min", "whence_sum", nullptr};
    for (const char **pk = profile_keys; *pk; ++pk) {
        auto it = int_map.find(*pk);
        if (it != int_map.end()) {
            auto idx = builder.add_or_get_column(*pk, ColumnType::INT64);
            builder.append_int64(idx, it->second);
        }
    }
}

void append_system_columns(
    RecordBatchBuilder &builder,
    const std::unordered_map<std::string, double> &float_map) {
    static const char *sys_keys[] = {
        "user_pct", "system_pct",  "iowait_pct",   "idle_pct",
        "irq_pct",  "softirq_pct", "MemAvailable", "MemFree",
        "Cached",   "Dirty",       "Active",       nullptr};
    for (const char **sk = sys_keys; *sk; ++sk) {
        auto it = float_map.find(*sk);
        if (it != float_map.end()) {
            auto idx = builder.add_or_get_column(*sk, ColumnType::DOUBLE);
            builder.append_double(idx, it->second);
        }
    }
}

// Normalize a raw JSON row (parsed with simdjson) into the semantic
// output schema.  Appends one row to `builder` with the full set of output
// columns.  Returns false if the row should be skipped (no valid name).
bool normalize_row(RecordBatchBuilder &builder, StringArena &arena,
                   JsonParser &parser, TimeScaleState &time_scale) {
    using SVH = JsonValueHelper;
    // --- Single-pass extraction: capture top-level fields and args in one
    // member walk (dispatch on key, any field order). ---
    dftracer::utils::trace::RecordPhase phase =
        dftracer::utils::trace::RecordPhase::UNKNOWN;
    std::string_view name_sv, cat_sv;
    std::optional<std::int64_t> pid_opt, tid_opt, ts_opt, dur_opt;
    ParsedArgs args;
    parser.for_each_field(
        [&](std::string_view key, simdjson::ondemand::value val) {
            if (key == "ph") {
                phase = dftracer::utils::trace::read_phase(val);
            } else if (key == "name") {
                if (auto s = SVH::get_string(val)) name_sv = *s;
            } else if (key == "cat") {
                if (auto s = SVH::get_string(val)) cat_sv = *s;
            } else if (key == "pid") {
                pid_opt = SVH::get_int64(val);
            } else if (key == "tid") {
                tid_opt = SVH::get_int64(val);
            } else if (key == "ts") {
                ts_opt = SVH::get_int64(val);
            } else if (key == "dur") {
                dur_opt = SVH::get_int64(val);
            } else if (key == "args") {
                args = parse_row_args(val);
            }
        });

    // --- Type classification ---
    bool is_M = (phase == dftracer::utils::trace::RecordPhase::METADATA);
    bool is_C = (phase == dftracer::utils::trace::RecordPhase::COUNTER);
    bool is_event = !is_M && !is_C;

    if (is_M && name_sv == "CM" && args.name && *args.name == "time_metric" &&
        args.value) {
        time_scale.metric = trace::parse_time_metric(*args.value);
    }

    std::int8_t row_type = ROW_EVENT;
    if (is_M) {
        if (name_sv == "FH")
            row_type = ROW_FILE_HASH;
        else if (name_sv == "HH")
            row_type = ROW_HOST_HASH;
        else if (name_sv == "SH")
            row_type = ROW_STRING_HASH;
        else if (name_sv == "PR")
            row_type = ROW_PROC_METADATA;
        else
            row_type = ROW_METADATA;
    } else if (is_C) {
        row_type = str_iequal(cat_sv, "sys") ? ROW_SYSTEM : ROW_PROFILE;
    }
    bool is_hash = (row_type >= ROW_FILE_HASH && row_type <= ROW_STRING_HASH) ||
                   row_type == ROW_PROC_METADATA;
    bool is_profile = (row_type == ROW_PROFILE);
    bool is_sys = (row_type == ROW_SYSTEM);

    // Name: metadata rows use args.name if available
    std::string_view out_name = name_sv;
    if (is_M && args.name && !args.name->empty()) {
        out_name = *args.name;
    }
    if (out_name.empty()) return false;  // skip rows without name

    // --- Declare all output columns ---
    auto ci_type = builder.add_or_get_column("type", ColumnType::INT64);
    auto ci_cat = builder.add_or_get_column("cat", ColumnType::STRING);
    auto ci_name = builder.add_or_get_column("name", ColumnType::STRING);
    auto ci_pid = builder.add_or_get_column("pid", ColumnType::INT64);
    auto ci_tid = builder.add_or_get_column("tid", ColumnType::INT64);
    auto ci_hash = builder.add_or_get_column("hash", ColumnType::STRING);
    auto ci_value = builder.add_or_get_column("value", ColumnType::STRING);
    auto ci_host_hash =
        builder.add_or_get_column("host_hash", ColumnType::STRING);
    auto ci_file_hash =
        builder.add_or_get_column("file_hash", ColumnType::STRING);
    auto ci_epoch = builder.add_or_get_column("epoch", ColumnType::INT64);
    auto ci_step = builder.add_or_get_column("step", ColumnType::INT64);
    auto ci_ts = builder.add_or_get_column("ts", ColumnType::INT64);
    auto ci_dur = builder.add_or_get_column("dur", ColumnType::INT64);
    auto ci_te = builder.add_or_get_column("te", ColumnType::INT64);
    [[maybe_unused]] auto ci_trange =
        builder.add_or_get_column("trange", ColumnType::INT64);
    auto ci_io_cat = builder.add_or_get_column("io_cat", ColumnType::INT64);
    auto ci_size = builder.add_or_get_column("size", ColumnType::INT64);
    auto ci_offset = builder.add_or_get_column("offset", ColumnType::INT64);
    auto ci_image_id = builder.add_or_get_column("image_id", ColumnType::INT64);

    // --- Populate core columns ---
    builder.append_int64(ci_type, row_type);

    // cat (lowercased) - write into arena
    if (!cat_sv.empty()) {
        char lbuf[256];
        std::size_t clen = std::min(cat_sv.size(), sizeof(lbuf));
        for (std::size_t i = 0; i < clen; ++i)
            lbuf[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(cat_sv[i])));
        builder.append_string(ci_cat, arena.push(lbuf, clen));
    } else {
        builder.append_null(ci_cat);
    }

    builder.append_string(ci_name, out_name);

    if (pid_opt) builder.append_int64(ci_pid, *pid_opt);
    if (tid_opt) builder.append_int64(ci_tid, *tid_opt);

    // hash / value
    if (is_hash && args.value && !args.value->empty())
        builder.append_string(ci_hash, *args.value);
    if (row_type == ROW_METADATA && args.value && !args.value->empty())
        builder.append_string(ci_value, *args.value);

    // host_hash / file_hash
    if (args.hhash && !args.hhash->empty())
        builder.append_string(ci_host_hash, *args.hhash);
    if (args.fhash && !args.fhash->empty())
        builder.append_string(ci_file_hash, *args.fhash);

    // epoch / step
    if (args.epoch && *args.epoch >= 0)
        builder.append_int64(ci_epoch, *args.epoch);
    if (args.step && *args.step >= 0) builder.append_int64(ci_step, *args.step);

    // --- Temporal ---
    bool has_ts = (is_event || is_C) && ts_opt.has_value();
    bool has_dur = dur_opt.has_value();
    std::int64_t ts_val = 0, dur_val = 0;
    const bool scale_time =
        time_scale.target && *time_scale.target != time_scale.metric;
    auto scaled = [&](std::int64_t v) {
        return static_cast<std::int64_t>(
            trace::scale_between(time_scale.metric, *time_scale.target,
                                 static_cast<std::uint64_t>(v)));
    };
    if (has_ts) {
        ts_val = *ts_opt;
        if (scale_time) ts_val = scaled(ts_val);
        builder.append_int64(ci_ts, ts_val);
    }
    if (is_event && has_ts && has_dur) {
        dur_val = *dur_opt;
        if (scale_time) dur_val = scaled(dur_val);
        builder.append_int64(ci_dur, dur_val);
        builder.append_int64(ci_te, ts_val + dur_val);
    }

    // --- IO columns (events only) ---
    if (is_event) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        std::int8_t io_cat = is_posix_stdio
                                 ? get_io_cat(out_name)
                                 : static_cast<std::int8_t>(IO_OTHER);
        // Size uses the shared io-cat rule (size_sum > posix/stdio read|write
        // ret > image_size) so the reader and the View aggregator's "size"
        // cannot drift.
        if (auto sz = dftracer::utils::trace::internal::derive_io_size(
                cat_sv, out_name, args.size_sum, args.ret, args.image_size))
            builder.append_int64(ci_size, *sz);
        // Offset / image-id side outputs stay on their original branches (not
        // part of the size rule).
        if (!args.size_sum) {
            if (is_posix_stdio) {
                if (args.offset && *args.offset >= 0)
                    builder.append_int64(ci_offset, *args.offset);
            } else if (args.image_idx && *args.image_idx > 0) {
                builder.append_int64(ci_image_id, *args.image_idx);
            }
        }
        builder.append_int64(ci_io_cat, io_cat);
    }

    // --- Profile columns ---
    if (is_profile) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        std::int8_t io_cat = is_posix_stdio
                                 ? get_io_cat(out_name)
                                 : static_cast<std::int8_t>(IO_OTHER);
        builder.append_int64(ci_io_cat, io_cat);
        append_profile_columns(builder, args.int_map);
    }

    // --- System columns ---
    if (is_sys) {
        append_system_columns(builder, args.float_map);
    }

    builder.end_row();
    return true;
}

}  // namespace

// Membership test for the projection (linear over a handful of kept keys, so
// no per-field allocation).
static bool keep_key(const RowBuildOptions &opts, std::string_view key) {
    if (!opts.keep || opts.keep->empty()) return true;
    for (const auto &k : *opts.keep)
        if (k == key) return true;
    return false;
}

bool build_arrow_row(RecordBatchBuilder &builder, JsonParser &parser,
                     StringArena &arena, bool normalize,
                     TimeScaleState &time_scale, const RowBuildOptions &opts) {
    if (normalize) return normalize_row(builder, arena, parser, time_scale);

    using SVH = JsonValueHelper;
    parser.for_each_field([&](std::string_view key_sv,
                              simdjson::ondemand::value val) {
        if (!keep_key(opts, key_sv)) return;
        auto type_result = val.type();
        if (type_result.error()) return;
        auto json_type = type_result.value_unsafe();
        switch (json_type) {
            case simdjson::ondemand::json_type::number: {
                auto num_result = val.get_number();
                if (num_result.error()) break;
                auto num = num_result.value_unsafe();
                const bool time_key =
                    opts.time_scale != 1.0 &&
                    (key_sv == "ts" || key_sv == "dur" || key_sv == "te");
                if (num.is_int64()) {
                    std::int64_t v = num.get_int64();
                    if (time_key)
                        v = static_cast<std::int64_t>(static_cast<double>(v) *
                                                      opts.time_scale);
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::INT64);
                    builder.append_int64(idx, v);
                } else if (num.is_uint64()) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::UINT64);
                    builder.append_uint64(idx, num.get_uint64());
                } else {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::DOUBLE);
                    builder.append_double(idx, num.get_double());
                }
                break;
            }
            case simdjson::ondemand::json_type::string: {
                auto str_result = val.get_string();
                if (str_result.error()) break;
                auto str = str_result.value_unsafe();
                if (opts.dict_strings) {
                    std::size_t idx = builder.add_or_get_column(
                        key_sv, ColumnType::DICT_STRING);
                    builder.append_dict_string(idx, str);
                    break;
                }
                std::size_t idx =
                    builder.add_or_get_column(key_sv, ColumnType::STRING);
                builder.append_string(idx, str);
                break;
            }
            case simdjson::ondemand::json_type::boolean: {
                auto bool_result = val.get_bool();
                if (bool_result.error()) break;
                auto b = bool_result.value_unsafe();
                std::size_t idx =
                    builder.add_or_get_column(key_sv, ColumnType::BOOL);
                builder.append_bool(idx, b);
                break;
            }
            case simdjson::ondemand::json_type::null: {
                auto existing = builder.find_column(key_sv);
                if (existing) builder.append_null(*existing);
                break;
            }
            case simdjson::ondemand::json_type::object:
            case simdjson::ondemand::json_type::array: {
                auto json_str = SVH::to_json_string(val);
                std::size_t idx =
                    builder.add_or_get_column(key_sv, ColumnType::STRING);
                if (json_str) {
                    builder.append_string(
                        idx, arena.push(json_str->data(), json_str->size()));
                } else {
                    builder.append_null(idx);
                }
                break;
            }
            default:
                break;
        }
    });
    builder.end_row();
    return true;
}

bool process_json_line(RecordBatchBuilder &builder, JsonParser &parser,
                       StringArena &arena, std::string_view content,
                       bool normalize, TimeScaleState &time_scale,
                       const RowBuildOptions &opts) {
    const char *trimmed;
    std::size_t trimmed_length;
    if (!dftracer::utils::json::trim_and_validate_with_comma(
            content.data(), content.size(), trimmed, trimmed_length))
        return false;
    if (!parser.parse(std::string_view(trimmed, trimmed_length))) return false;
    return build_arrow_row(builder, parser, arena, normalize, time_scale, opts);
}

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW
