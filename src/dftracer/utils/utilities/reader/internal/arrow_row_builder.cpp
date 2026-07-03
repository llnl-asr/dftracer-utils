#include <dftracer/utils/utilities/reader/internal/arrow_row_builder.h>

#ifdef DFTRACER_UTILS_ENABLE_ARROW

#include <ankerl/unordered_dense.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>

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
using common::json::JsonParser;
using common::json::JsonValueHelper;

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
    using namespace dftracer::utils::utilities::composites::dft::internal;
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

bool str_contains_lower(std::string_view s, const char *needle) {
    std::size_t nlen = std::strlen(needle);
    if (s.size() < nlen) return false;
    for (std::size_t i = 0; i <= s.size() - nlen; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < nlen; ++j) {
            if (std::tolower(static_cast<unsigned char>(s[i + j])) !=
                static_cast<unsigned char>(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
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
        "count",      "count_max",  "count_min",  "count_sum",  "dft_cnt",
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
                   JsonParser &parser) {
    using SVH = JsonValueHelper;
    // --- Single-pass extraction: capture top-level fields and args in one
    // member walk (dispatch on key, any field order). ---
    std::string_view ph, name_sv, cat_sv;
    std::optional<std::int64_t> pid_opt, tid_opt, ts_opt, dur_opt;
    ParsedArgs args;
    parser.for_each_field(
        [&](std::string_view key, simdjson::ondemand::value val) {
            if (key == "ph") {
                if (auto s = SVH::get_string(val)) ph = *s;
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
    bool is_M = (ph == "M");
    bool is_C = (ph == "C");
    bool is_event = !is_M && !is_C;

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
    if (has_ts) {
        ts_val = *ts_opt;
        builder.append_int64(ci_ts, ts_val);
    }
    if (is_event && has_ts && has_dur) {
        dur_val = *dur_opt;
        builder.append_int64(ci_dur, dur_val);
        builder.append_int64(ci_te, ts_val + dur_val);
    }

    // --- IO columns (events only) ---
    if (is_event) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        std::int8_t io_cat = IO_OTHER;

        // size priority: size_sum > POSIX ret > image_size
        if (args.size_sum) {
            builder.append_int64(ci_size, *args.size_sum);
            if (is_posix_stdio) io_cat = get_io_cat(out_name);
        } else if (is_posix_stdio) {
            io_cat = get_io_cat(out_name);
            if (args.ret && *args.ret > 0 &&
                (io_cat == IO_READ || io_cat == IO_WRITE))
                builder.append_int64(ci_size, *args.ret);
            if (args.offset && *args.offset >= 0)
                builder.append_int64(ci_offset, *args.offset);
        } else {
            if (args.image_idx && *args.image_idx > 0)
                builder.append_int64(ci_image_id, *args.image_idx);
            if (args.image_size && *args.image_size > 0 &&
                !str_contains_lower(out_name, "open"))
                builder.append_int64(ci_size, *args.image_size);
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

// Flatten a simdjson object into "prefix.key" columns using native types.
// On type mismatch (same key, different type across rows), appends null.
void flatten_object_into(RecordBatchBuilder &builder, StringArena &arena,
                         std::string_view prefix,
                         simdjson::ondemand::object obj) {
    using SVH = JsonValueHelper;
    char key_buf[512];

    for (auto field : obj) {
        if (field.error()) continue;

        auto key_result = field.unescaped_key();
        if (key_result.error()) continue;
        std::string_view sk = key_result.value_unsafe();

        auto val_result = field.value();
        if (val_result.error()) continue;
        auto sub_val = val_result.value_unsafe();

        std::size_t needed = prefix.size() + 1 + sk.size();
        if (needed >= sizeof(key_buf)) continue;
        std::memcpy(key_buf, prefix.data(), prefix.size());
        key_buf[prefix.size()] = '.';
        std::memcpy(key_buf + prefix.size() + 1, sk.data(), sk.size());
        std::string_view full_key(key_buf, needed);

        auto type_result = sub_val.type();
        if (type_result.error()) continue;
        auto json_type = type_result.value_unsafe();

        switch (json_type) {
            case simdjson::ondemand::json_type::number: {
                auto num_result = sub_val.get_number();
                if (num_result.error()) break;
                auto num = num_result.value_unsafe();
                if (num.is_int64()) {
                    auto idx =
                        builder.add_or_get_column(full_key, ColumnType::INT64);
                    if (builder.column_type(idx) == ColumnType::INT64)
                        builder.append_int64(idx, num.get_int64());
                    else
                        builder.append_null(idx);
                } else if (num.is_uint64()) {
                    auto idx =
                        builder.add_or_get_column(full_key, ColumnType::UINT64);
                    if (builder.column_type(idx) == ColumnType::UINT64)
                        builder.append_uint64(idx, num.get_uint64());
                    else
                        builder.append_null(idx);
                } else {
                    auto idx =
                        builder.add_or_get_column(full_key, ColumnType::DOUBLE);
                    if (builder.column_type(idx) == ColumnType::DOUBLE)
                        builder.append_double(idx, num.get_double());
                    else
                        builder.append_null(idx);
                }
                break;
            }
            case simdjson::ondemand::json_type::string: {
                auto str_result = sub_val.get_string();
                if (str_result.error()) break;
                auto str = str_result.value_unsafe();
                auto idx =
                    builder.add_or_get_column(full_key, ColumnType::STRING);
                if (builder.column_type(idx) == ColumnType::STRING)
                    builder.append_string(idx, str);
                else
                    builder.append_null(idx);
                break;
            }
            case simdjson::ondemand::json_type::boolean: {
                auto bool_result = sub_val.get_bool();
                if (bool_result.error()) break;
                auto b = bool_result.value_unsafe();
                auto idx =
                    builder.add_or_get_column(full_key, ColumnType::BOOL);
                if (builder.column_type(idx) == ColumnType::BOOL)
                    builder.append_bool(idx, b);
                else
                    builder.append_null(idx);
                break;
            }
            case simdjson::ondemand::json_type::null: {
                auto existing = builder.find_column(full_key);
                if (existing) builder.append_null(*existing);
                break;
            }
            case simdjson::ondemand::json_type::object:
            case simdjson::ondemand::json_type::array: {
                // Serialize nested object/array to JSON string
                auto json_str = SVH::to_json_string(sub_val);
                auto idx =
                    builder.add_or_get_column(full_key, ColumnType::STRING);
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
    }
}

bool build_arrow_row(RecordBatchBuilder &builder, JsonParser &parser,
                     StringArena &arena, bool normalize) {
    if (normalize) return normalize_row(builder, arena, parser);

    using SVH = JsonValueHelper;
    parser.for_each_field([&](std::string_view key_sv,
                              simdjson::ondemand::value val) {
        auto type_result = val.type();
        if (type_result.error()) return;
        auto json_type = type_result.value_unsafe();
        switch (json_type) {
            case simdjson::ondemand::json_type::number: {
                auto num_result = val.get_number();
                if (num_result.error()) break;
                auto num = num_result.value_unsafe();
                if (num.is_int64()) {
                    std::size_t idx =
                        builder.add_or_get_column(key_sv, ColumnType::INT64);
                    builder.append_int64(idx, num.get_int64());
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
                       bool normalize) {
    const char *trimmed;
    std::size_t trimmed_length;
    if (!dftracer::utils::json_trim_and_validate_with_comma(
            content.data(), content.size(), trimmed, trimmed_length))
        return false;
    if (!parser.parse(std::string_view(trimmed, trimmed_length))) return false;
    return build_arrow_row(builder, parser, arena, normalize);
}

}  // namespace dftracer::utils::utilities::reader::internal

#endif  // DFTRACER_UTILS_ENABLE_ARROW
