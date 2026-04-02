#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <yyjson.h>
#endif

namespace {

using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;

int64_t json_to_int64(yyjson_val *value) {
    if (yyjson_is_int(value)) return yyjson_get_sint(value);
    return static_cast<int64_t>(yyjson_get_uint(value));
}

CoroTask<void> produce_lines(std::shared_ptr<IteratorState> state,
                             TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->content);
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

CoroTask<void> produce_raw(std::shared_ptr<IteratorState> state,
                           TraceReaderConfig cfg, ReadConfig rc) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_raw(rc);
        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;
            std::string item(opt->data(), opt->size());
            {
                std::unique_lock<std::mutex> lock(sp->mtx);
                sp->cv_producer.wait(lock, [sp] {
                    return sp->queue.size() < sp->max_queue_size ||
                           sp->cancelled.load(std::memory_order_acquire);
                });
                if (sp->cancelled.load(std::memory_order_acquire)) break;
                sp->queue.push(std::move(item));
            }
            sp->cv_consumer.notify_one();
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

using dftracer::utils::utilities::common::arrow::ColumnType;
using dftracer::utils::utilities::common::arrow::RecordBatchBuilder;

// Bump arena for string_views that must survive until builder.finish().
struct StringArena {
    static constexpr std::size_t BLOCK_SIZE = 64 * 1024;
    std::vector<std::vector<char>> blocks;
    std::size_t pos = 0;

    StringArena() { blocks.emplace_back(BLOCK_SIZE); }

    std::string_view push(const char *data, std::size_t len) {
        if (pos + len > blocks.back().size()) {
            blocks.emplace_back(std::max(BLOCK_SIZE, len));
            pos = 0;
        }
        char *dst = blocks.back().data() + pos;
        std::memcpy(dst, data, len);
        pos += len;
        return {dst, len};
    }

    void clear() {
        if (blocks.size() > 1) blocks.resize(1);
        pos = 0;
    }
};

// --- Row type constants (must match Python TYPE_* constants) ---
enum RowType : int8_t {
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
enum IOCat : int8_t {
    IO_READ = 1,
    IO_WRITE = 2,
    IO_METADATA = 3,
    IO_PCTL = 4,
    IO_IPC = 5,
    IO_OTHER = 6,
    IO_SYNC = 7,
};

static int8_t get_io_cat(std::string_view func) {
    // READ
    if (func == "fread" || func == "pread" || func == "preadv" ||
        func == "read" || func == "readv")
        return IO_READ;
    // WRITE
    if (func == "fwrite" || func == "pwrite" || func == "pwritev" ||
        func == "write" || func == "writev")
        return IO_WRITE;
    // SYNC
    if (func == "fsync" || func == "fdatasync" || func == "msync" ||
        func == "sync")
        return IO_SYNC;
    // PCTL
    if (func == "exec" || func == "exit" || func == "fork" || func == "kill" ||
        func == "pipe" || func == "wait")
        return IO_PCTL;
    // IPC
    if (func == "msgctl" || func == "msgget" || func == "msgrcv" ||
        func == "msgsnd" || func == "semctl" || func == "semget" ||
        func == "semop" || func == "shmat" || func == "shmctl" ||
        func == "shmdt" || func == "shmget")
        return IO_IPC;
    // METADATA
    if (func == "__fxstat" || func == "__fxstat64" || func == "__lxstat" ||
        func == "__lxstat64" || func == "__xstat" || func == "__xstat64" ||
        func == "access" || func == "close" || func == "closedir" ||
        func == "fclose" || func == "fcntl" || func == "fopen" ||
        func == "fopen64" || func == "fseek" || func == "fstat" ||
        func == "fstatat" || func == "ftell" || func == "ftruncate" ||
        func == "link" || func == "lseek" || func == "lseek64" ||
        func == "mkdir" || func == "open" || func == "open64" ||
        func == "opendir" || func == "readdir" || func == "readlink" ||
        func == "remove" || func == "rename" || func == "rmdir" ||
        func == "seek" || func == "stat" || func == "unlink")
        return IO_METADATA;
    return IO_OTHER;
}

static bool str_iequal(std::string_view a, const char *b) {
    std::size_t len = std::strlen(b);
    if (a.size() != len) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            static_cast<unsigned char>(b[i]))
            return false;
    }
    return true;
}

static bool str_contains_lower(std::string_view s, const char *needle) {
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

// Normalize a raw JSON row (already parsed into yyjson) into the semantic
// output schema.  Appends one row to `builder` with the full set of output
// columns.  Returns false if the row should be skipped (no valid name).
static bool normalize_row(RecordBatchBuilder &builder, StringArena &arena,
                          yyjson_val *root) {
    // --- Extract top-level fields ---
    yyjson_val *v_ph = yyjson_obj_get(root, "ph");
    yyjson_val *v_name = yyjson_obj_get(root, "name");
    yyjson_val *v_cat = yyjson_obj_get(root, "cat");
    yyjson_val *v_pid = yyjson_obj_get(root, "pid");
    yyjson_val *v_tid = yyjson_obj_get(root, "tid");
    yyjson_val *v_ts = yyjson_obj_get(root, "ts");
    yyjson_val *v_dur = yyjson_obj_get(root, "dur");
    yyjson_val *v_args = yyjson_obj_get(root, "args");

    std::string_view ph =
        v_ph && yyjson_is_str(v_ph)
            ? std::string_view(yyjson_get_str(v_ph), yyjson_get_len(v_ph))
            : std::string_view();
    std::string_view name_sv =
        v_name && yyjson_is_str(v_name)
            ? std::string_view(yyjson_get_str(v_name), yyjson_get_len(v_name))
            : std::string_view();
    std::string_view cat_sv =
        v_cat && yyjson_is_str(v_cat)
            ? std::string_view(yyjson_get_str(v_cat), yyjson_get_len(v_cat))
            : std::string_view();

    // Helper to get args fields
    auto args_str = [&](const char *key) -> std::string_view {
        if (!v_args) return {};
        yyjson_val *v = yyjson_obj_get(v_args, key);
        if (!v) return {};
        if (yyjson_is_str(v)) return {yyjson_get_str(v), yyjson_get_len(v)};
        return {};
    };
    auto args_int = [&](const char *key) -> std::pair<bool, int64_t> {
        if (!v_args) return {false, 0};
        yyjson_val *v = yyjson_obj_get(v_args, key);
        if (!v) return {false, 0};
        if (yyjson_is_int(v)) return {true, yyjson_get_sint(v)};
        if (yyjson_is_uint(v))
            return {true, static_cast<int64_t>(yyjson_get_uint(v))};
        if (yyjson_is_real(v))
            return {true, static_cast<int64_t>(yyjson_get_real(v))};
        return {false, 0};
    };
    auto args_float = [&](const char *key) -> std::pair<bool, double> {
        if (!v_args) return {false, 0.0};
        yyjson_val *v = yyjson_obj_get(v_args, key);
        if (!v) return {false, 0.0};
        if (yyjson_is_real(v)) return {true, yyjson_get_real(v)};
        if (yyjson_is_int(v))
            return {true, static_cast<double>(yyjson_get_sint(v))};
        if (yyjson_is_uint(v))
            return {true, static_cast<double>(yyjson_get_uint(v))};
        return {false, 0.0};
    };

    // --- Type classification ---
    bool is_M = (ph == "M");
    bool is_C = (ph == "C");
    bool is_event = !is_M && !is_C;

    int8_t row_type = ROW_EVENT;
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
    if (is_M) {
        auto an = args_str("name");
        if (!an.empty()) out_name = an;
    }
    if (out_name.empty()) return false;  // skip rows without name

    // --- Declare all output columns (lazy — add_or_get_column handles
    // first-time creation) --- We use a fixed schema so column indices are
    // stable across rows. The builder backfills nulls for columns not touched
    // via end_row().

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
    auto ci_trange = builder.add_or_get_column("trange", ColumnType::INT64);
    auto ci_io_cat = builder.add_or_get_column("io_cat", ColumnType::INT64);
    auto ci_size = builder.add_or_get_column("size", ColumnType::INT64);
    auto ci_offset = builder.add_or_get_column("offset", ColumnType::INT64);
    auto ci_image_id = builder.add_or_get_column("image_id", ColumnType::INT64);

    // --- Populate core columns ---
    builder.append_int64(ci_type, row_type);

    // cat (lowercased) — write into arena
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

    if (v_pid && (yyjson_is_int(v_pid) || yyjson_is_uint(v_pid)))
        builder.append_int64(ci_pid, json_to_int64(v_pid));
    // else: null via end_row backfill

    if (v_tid && (yyjson_is_int(v_tid) || yyjson_is_uint(v_tid)))
        builder.append_int64(ci_tid, json_to_int64(v_tid));

    // hash / value
    auto a_value = args_str("value");
    if (is_hash && !a_value.empty()) builder.append_string(ci_hash, a_value);
    if (row_type == ROW_METADATA && !a_value.empty())
        builder.append_string(ci_value, a_value);

    // host_hash / file_hash
    auto a_hhash = args_str("hhash");
    if (!a_hhash.empty()) builder.append_string(ci_host_hash, a_hhash);
    auto a_fhash = args_str("fhash");
    if (!a_fhash.empty()) builder.append_string(ci_file_hash, a_fhash);

    // epoch / step
    auto [has_epoch, epoch_v] = args_int("epoch");
    if (has_epoch && epoch_v >= 0) builder.append_int64(ci_epoch, epoch_v);
    auto [has_step, step_v] = args_int("step");
    if (has_step && step_v >= 0) builder.append_int64(ci_step, step_v);

    // --- Temporal ---
    bool has_ts = (is_event || is_C) && v_ts &&
                  (yyjson_is_int(v_ts) || yyjson_is_uint(v_ts));
    bool has_dur = v_dur && (yyjson_is_int(v_dur) || yyjson_is_uint(v_dur));
    int64_t ts_val = 0, dur_val = 0;
    if (has_ts) {
        ts_val = json_to_int64(v_ts);
        builder.append_int64(ci_ts, ts_val);
    }
    if (is_event && has_ts && has_dur) {
        dur_val = json_to_int64(v_dur);
        builder.append_int64(ci_dur, dur_val);
        builder.append_int64(ci_te, ts_val + dur_val);
    }

    // --- IO columns (events only) ---
    if (is_event) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        int8_t io_cat = IO_OTHER;

        // size priority: size_sum > POSIX ret > image_size
        auto [has_ss, ss_val] = args_int("size_sum");
        if (has_ss) {
            builder.append_int64(ci_size, ss_val);
            if (is_posix_stdio) io_cat = get_io_cat(name_sv);
        } else if (is_posix_stdio) {
            io_cat = get_io_cat(name_sv);
            auto [has_ret, ret_val] = args_int("ret");
            if (has_ret && ret_val > 0 &&
                (io_cat == IO_READ || io_cat == IO_WRITE))
                builder.append_int64(ci_size, ret_val);
            auto [has_ofs, ofs_val] = args_int("offset");
            if (has_ofs && ofs_val >= 0)
                builder.append_int64(ci_offset, ofs_val);
        } else {
            auto [has_img, img_val] = args_int("image_idx");
            if (has_img && img_val > 0)
                builder.append_int64(ci_image_id, img_val);
            auto [has_ims, ims_val] = args_int("image_size");
            if (has_ims && ims_val > 0 && !str_contains_lower(name_sv, "open"))
                builder.append_int64(ci_size, ims_val);
        }
        builder.append_int64(ci_io_cat, io_cat);
    }

    // --- Profile columns ---
    if (is_profile) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        int8_t io_cat = is_posix_stdio ? get_io_cat(name_sv) : IO_OTHER;
        builder.append_int64(ci_io_cat, io_cat);

        static const char *profile_keys[] = {
            "count",      "count_max",  "count_min",  "count_sum",
            "dft_cnt",    "dur",        "dur_max",    "dur_min",
            "dur_sum",    "epoch",      "flags",      "offset",
            "offset_max", "offset_min", "offset_sum", "ret",
            "ret_max",    "ret_min",    "ret_sum",    "whence",
            "whence_max", "whence_min", "whence_sum", nullptr};
        for (const char **pk = profile_keys; *pk; ++pk) {
            auto [has_v, val] = args_int(*pk);
            if (has_v) {
                auto idx = builder.add_or_get_column(*pk, ColumnType::INT64);
                builder.append_int64(idx, val);
            }
        }
    }

    // --- System columns ---
    if (is_sys) {
        static const char *sys_keys[] = {
            "user_pct", "system_pct",  "iowait_pct",   "idle_pct",
            "irq_pct",  "softirq_pct", "MemAvailable", "MemFree",
            "Cached",   "Dirty",       "Active",       nullptr};
        for (const char **sk = sys_keys; *sk; ++sk) {
            auto [has_v, val] = args_float(*sk);
            if (has_v) {
                auto idx = builder.add_or_get_column(*sk, ColumnType::DOUBLE);
                builder.append_double(idx, val);
            }
        }
    }

    builder.end_row();
    return true;
}

// Flatten a yyjson object into "prefix.key" columns using native types.
// On type mismatch (same key, different type across rows), appends null.
static void flatten_object_into(RecordBatchBuilder &builder, StringArena &arena,
                                std::string_view prefix, yyjson_val *obj) {
    char key_buf[512];

    yyjson_obj_iter sub_iter;
    yyjson_obj_iter_init(obj, &sub_iter);
    yyjson_val *sub_key;
    while ((sub_key = yyjson_obj_iter_next(&sub_iter))) {
        yyjson_val *sub_val = yyjson_obj_iter_get_val(sub_key);
        const char *sk_str = yyjson_get_str(sub_key);
        std::size_t sk_len = yyjson_get_len(sub_key);

        std::size_t needed = prefix.size() + 1 + sk_len;
        if (needed >= sizeof(key_buf)) continue;
        std::memcpy(key_buf, prefix.data(), prefix.size());
        key_buf[prefix.size()] = '.';
        std::memcpy(key_buf + prefix.size() + 1, sk_str, sk_len);
        std::string_view full_key(key_buf, needed);

        if (yyjson_is_int(sub_val)) {
            auto idx = builder.add_or_get_column(full_key, ColumnType::INT64);
            if (builder.column_type(idx) == ColumnType::INT64)
                builder.append_int64(idx, yyjson_get_sint(sub_val));
            else
                builder.append_null(idx);
        } else if (yyjson_is_uint(sub_val)) {
            auto idx = builder.add_or_get_column(full_key, ColumnType::UINT64);
            if (builder.column_type(idx) == ColumnType::UINT64)
                builder.append_uint64(idx, yyjson_get_uint(sub_val));
            else
                builder.append_null(idx);
        } else if (yyjson_is_real(sub_val)) {
            auto idx = builder.add_or_get_column(full_key, ColumnType::DOUBLE);
            if (builder.column_type(idx) == ColumnType::DOUBLE)
                builder.append_double(idx, yyjson_get_real(sub_val));
            else
                builder.append_null(idx);
        } else if (yyjson_is_bool(sub_val)) {
            auto idx = builder.add_or_get_column(full_key, ColumnType::BOOL);
            if (builder.column_type(idx) == ColumnType::BOOL)
                builder.append_bool(idx, yyjson_get_bool(sub_val));
            else
                builder.append_null(idx);
        } else if (yyjson_is_str(sub_val)) {
            auto idx = builder.add_or_get_column(full_key, ColumnType::STRING);
            if (builder.column_type(idx) == ColumnType::STRING)
                builder.append_string(
                    idx, std::string_view(yyjson_get_str(sub_val),
                                          yyjson_get_len(sub_val)));
            else
                builder.append_null(idx);
        } else if (yyjson_is_null(sub_val)) {
            auto existing = builder.find_column(full_key);
            if (existing) builder.append_null(*existing);
        } else {
            // nested object/array: serialize
            std::size_t json_len;
            char *json_str = yyjson_val_write(sub_val, 0, &json_len);
            auto idx = builder.add_or_get_column(full_key, ColumnType::STRING);
            if (json_str) {
                builder.append_string(idx, arena.push(json_str, json_len));
                free(json_str);
            } else {
                builder.append_null(idx);
            }
        }
    }
}

CoroTask<void> produce_arrow_batches(std::shared_ptr<ArrowIteratorState> state,
                                     TraceReaderConfig cfg, ReadConfig rc,
                                     std::size_t batch_size,
                                     bool flatten_objects = false,
                                     bool normalize = false) {
    auto *sp = state.get();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        RecordBatchBuilder builder;
        builder.reserve(batch_size);

        std::vector<yyjson_doc *> held_docs;
        StringArena arena;
        held_docs.reserve(batch_size);

        while (auto opt = co_await gen.next()) {
            if (sp->cancelled.load(std::memory_order_acquire)) break;

            const char *trimmed;
            std::size_t trimmed_length;
            if (!dftracer::utils::json_trim_and_validate(
                    opt->content.data(), opt->content.size(), trimmed,
                    trimmed_length)) {
                continue;
            }

            yyjson_doc *doc = yyjson_read(trimmed, trimmed_length, 0);
            if (!doc) continue;

            yyjson_val *root = yyjson_doc_get_root(doc);
            if (!root || !yyjson_is_obj(root)) {
                yyjson_doc_free(doc);
                continue;
            }

            if (normalize) {
                // Produce the semantic output schema directly.
                // normalize_row calls end_row() internally.
                if (!normalize_row(builder, arena, root)) {
                    yyjson_doc_free(doc);
                    continue;
                }
                held_docs.push_back(doc);
            } else {
                yyjson_obj_iter iter;
                yyjson_obj_iter_init(root, &iter);
                yyjson_val *key;
                while ((key = yyjson_obj_iter_next(&iter))) {
                    yyjson_val *val = yyjson_obj_iter_get_val(key);
                    const char *key_str = yyjson_get_str(key);
                    std::size_t key_len = yyjson_get_len(key);
                    std::string_view key_sv(key_str, key_len);

                    if (yyjson_is_int(val)) {
                        std::size_t idx = builder.add_or_get_column(
                            key_sv, ColumnType::INT64);
                        builder.append_int64(idx, yyjson_get_sint(val));
                    } else if (yyjson_is_uint(val)) {
                        std::size_t idx = builder.add_or_get_column(
                            key_sv, ColumnType::UINT64);
                        builder.append_uint64(idx, yyjson_get_uint(val));
                    } else if (yyjson_is_real(val)) {
                        std::size_t idx = builder.add_or_get_column(
                            key_sv, ColumnType::DOUBLE);
                        builder.append_double(idx, yyjson_get_real(val));
                    } else if (yyjson_is_bool(val)) {
                        std::size_t idx =
                            builder.add_or_get_column(key_sv, ColumnType::BOOL);
                        builder.append_bool(idx, yyjson_get_bool(val));
                    } else if (yyjson_is_str(val)) {
                        std::size_t idx = builder.add_or_get_column(
                            key_sv, ColumnType::STRING);
                        builder.append_string(
                            idx, std::string_view(yyjson_get_str(val),
                                                  yyjson_get_len(val)));
                    } else if (yyjson_is_null(val)) {
                        auto existing = builder.find_column(key_sv);
                        if (existing) builder.append_null(*existing);
                    } else {
                        std::size_t json_len;
                        char *json_str = yyjson_val_write(val, 0, &json_len);
                        std::size_t idx = builder.add_or_get_column(
                            key_sv, ColumnType::STRING);
                        if (json_str) {
                            builder.append_string(
                                idx, arena.push(json_str, json_len));
                            free(json_str);
                        } else {
                            builder.append_null(idx);
                        }
                    }
                }
                builder.end_row();
                held_docs.push_back(doc);
            }  // end else (raw path)

            if (builder.num_rows() >= batch_size) {
                auto result = builder.finish();
                for (auto *d : held_docs) yyjson_doc_free(d);
                held_docs.clear();
                arena.clear();

                {
                    std::unique_lock<std::mutex> lock(sp->mtx);
                    sp->cv_producer.wait(lock, [sp] {
                        return sp->queue.size() < sp->max_queue_size ||
                               sp->cancelled.load(std::memory_order_acquire);
                    });
                    if (sp->cancelled.load(std::memory_order_acquire)) break;
                    sp->queue.push(std::move(result));
                }
                sp->cv_consumer.notify_one();
                builder.reset(false);
                builder.reserve(batch_size);
            }
        }

        if (builder.num_rows() > 0) {
            auto result = builder.finish();
            for (auto *d : held_docs) yyjson_doc_free(d);
            held_docs.clear();
            arena.clear();
            {
                std::lock_guard<std::mutex> lock(sp->mtx);
                sp->queue.push(std::move(result));
            }
            sp->cv_consumer.notify_one();
        } else {
            for (auto *d : held_docs) yyjson_doc_free(d);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->error = std::current_exception();
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
        sp->cv_consumer.notify_one();
        co_return;
    }
    {
        std::lock_guard<std::mutex> lock(sp->mtx);
        sp->queue.push(std::nullopt);
        sp->done.store(true, std::memory_order_release);
    }
    sp->cv_consumer.notify_one();
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

TraceReaderConfig build_config(TraceReaderObject *self) {
    TraceReaderConfig cfg;
    cfg.file_path = PyUnicode_AsUTF8(self->file_path);
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx) cfg.index_dir = idx;
    cfg.checkpoint_size = self->checkpoint_size;
    cfg.auto_build_index = self->auto_build_index != 0;
    cfg.index_threshold = self->index_threshold;
    return cfg;
}

static Runtime *get_runtime(TraceReaderObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return get_default_runtime();
}

static TraceReaderIteratorObject *make_iterator(
    std::shared_ptr<IteratorState> state, IteratorMode mode) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->state) std::shared_ptr<IteratorState>(std::move(state));
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    new (&it->arrow_state) std::shared_ptr<ArrowIteratorState>();
#endif
    it->mode = mode;
    return it;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
static TraceReaderIteratorObject *make_arrow_iterator(
    std::shared_ptr<ArrowIteratorState> state) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->state) std::shared_ptr<IteratorState>();
    new (&it->arrow_state)
        std::shared_ptr<ArrowIteratorState>(std::move(state));
    it->mode = IteratorMode::ARROW;
    return it;
}
#endif

}  // namespace

using dftracer::utils::python::wrap_arrow_table;

static void TraceReader_dealloc(TraceReaderObject *self) {
    Py_XDECREF(self->file_path);
    Py_XDECREF(self->index_dir);
    Py_XDECREF(self->runtime_obj);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *TraceReader_new(PyTypeObject *type, PyObject *args,
                                 PyObject *kwds) {
    TraceReaderObject *self = (TraceReaderObject *)type->tp_alloc(type, 0);
    if (self) {
        self->file_path = NULL;
        self->index_dir = NULL;
        self->checkpoint_size = 32 * 1024 * 1024;
        self->auto_build_index = 0;
        self->index_threshold =
            dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
        self->has_index = 0;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int TraceReader_init(TraceReaderObject *self, PyObject *args,
                            PyObject *kwds) {
    static const char *kwlist[] = {"file_path",
                                   "index_dir",
                                   "checkpoint_size",
                                   "auto_build_index",
                                   "index_threshold",
                                   "runtime",
                                   NULL};

    const char *file_path;
    const char *index_dir = "";
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    int auto_build_index = 0;
    std::size_t index_threshold =
        dftracer::utils::constants::indexer::DEFAULT_INDEX_SIZE_THRESHOLD;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snpnO", (char **)kwlist,
                                     &file_path, &index_dir, &checkpoint_size,
                                     &auto_build_index, &index_threshold,
                                     &runtime_arg)) {
        return -1;
    }

    if (runtime_arg && runtime_arg != Py_None) {
        if (PyObject_TypeCheck(runtime_arg, &RuntimeType)) {
            // Direct C++ Runtime object
            Py_INCREF(runtime_arg);
            self->runtime_obj = runtime_arg;
        } else {
            // Python wrapper, extract _native attribute
            PyObject *native = PyObject_GetAttrString(runtime_arg, "_native");
            if (native && PyObject_TypeCheck(native, &RuntimeType)) {
                self->runtime_obj = native;  // already incref'd by GetAttr
            } else {
                Py_XDECREF(native);
                PyErr_SetString(PyExc_TypeError,
                                "runtime must be a Runtime instance or None");
                return -1;
            }
        }
    }

    self->file_path = PyUnicode_FromString(file_path);
    if (!self->file_path) return -1;

    self->index_dir = PyUnicode_FromString(index_dir);
    if (!self->index_dir) {
        Py_DECREF(self->file_path);
        self->file_path = NULL;
        return -1;
    }

    self->checkpoint_size = checkpoint_size;
    self->auto_build_index = auto_build_index;
    self->index_threshold = index_threshold;

    try {
        TraceReaderConfig cfg;
        cfg.file_path = file_path;
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index != 0;
        cfg.index_threshold = index_threshold;
        TraceReader probe(std::move(cfg));
        self->has_index = probe.has_index() ? 1 : 0;
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        Py_DECREF(self->file_path);
        Py_DECREF(self->index_dir);
        self->file_path = NULL;
        self->index_dir = NULL;
        return -1;
    }

    return 0;
}

static PyObject *TraceReader_iter_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "query",
                                   NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_lines(state, cfg, rc), "iter_lines");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::LINES);
    return (PyObject *)it;
}

static PyObject *TraceReader_iter_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "line_aligned",
                                   "multi_line", "query",       NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    int line_aligned = 1;
    int multi_line = 1;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnppz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &line_aligned,
                                     &multi_line, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    rc.line_aligned = line_aligned != 0;
    rc.multi_line = multi_line != 0;
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_raw(state, cfg, rc), "iter_raw");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::RAW);
    return (PyObject *)it;
}

static PyObject *TraceReader_read_lines(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    PyObject *iter = TraceReader_iter_lines(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_read_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    PyObject *iter = TraceReader_iter_raw(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

static PyObject *TraceReader_iter_lines_json(TraceReaderObject *self,
                                             PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "query",
                                   NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnz", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &query_str)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<IteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_lines(state, cfg, rc), "iter_lines_json");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_iterator(state, IteratorMode::JSON);
    return (PyObject *)it;
}

static PyObject *TraceReader_read_lines_json(TraceReaderObject *self,
                                             PyObject *args, PyObject *kwds) {
    PyObject *iter = TraceReader_iter_lines_json(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    return list;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

static PyObject *TraceReader_iter_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {
        "batch_size", "start_line",  "end_line", "start_byte",
        "end_byte",   "buffer_size", "query",    "flatten_objects",
        "normalize",  NULL};
    Py_ssize_t batch_size = 10000;
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;
    int flatten_objects = 0;
    int normalize = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnnzpp", (char **)kwlist, &batch_size, &start_line,
            &end_line, &start_byte, &end_byte, &buffer_size, &query_str,
            &flatten_objects, &normalize)) {
        return NULL;
    }

    if (batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "batch_size must be > 0");
        return NULL;
    }
    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return NULL;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<ArrowIteratorState>();

    Runtime *rt = get_runtime(self);
    try {
        rt->submit(produce_arrow_batches(state, cfg, rc,
                                         static_cast<std::size_t>(batch_size),
                                         flatten_objects != 0, normalize != 0),
                   "iter_arrow");
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_arrow_iterator(std::move(state));
    return (PyObject *)it;
}

static PyObject *TraceReader_read_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    PyObject *iter = TraceReader_iter_arrow(self, args, kwds);
    if (!iter) return NULL;
    PyObject *list = PySequence_List(iter);
    Py_DECREF(iter);
    if (!list) return NULL;

    return wrap_arrow_table(list);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

static PyObject *TraceReader_enter(TraceReaderObject *self,
                                   PyObject *Py_UNUSED(ignored)) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *TraceReader_exit(TraceReaderObject *self, PyObject *args) {
    Py_RETURN_NONE;
}

static PyObject *TraceReader_get_file_path(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->file_path);
    return self->file_path;
}

static PyObject *TraceReader_get_index_dir(TraceReaderObject *self,
                                           void *closure) {
    Py_INCREF(self->index_dir);
    return self->index_dir;
}

static PyObject *TraceReader_get_has_index(TraceReaderObject *self,
                                           void *closure) {
    return PyBool_FromLong(self->has_index);
}

static PyObject *TraceReader_get_num_lines_prop(TraceReaderObject *self,
                                                void *closure) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        std::size_t n = reader.get_num_lines();
        if (n > 0) return PyLong_FromSize_t(n);
    } catch (...) {
    }
    PyObject *empty_args = PyTuple_New(0);
    if (!empty_args) return NULL;
    PyObject *list = TraceReader_read_lines(self, empty_args, NULL);
    Py_DECREF(empty_args);
    if (!list) return NULL;
    Py_ssize_t n = PyList_GET_SIZE(list);
    Py_DECREF(list);
    return PyLong_FromSsize_t(n);
}

static PyObject *TraceReader_get_max_bytes(TraceReaderObject *self,
                                           PyObject *Py_UNUSED(ignored)) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        return PyLong_FromSize_t(reader.get_max_bytes());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyObject *TraceReader_get_num_lines(TraceReaderObject *self,
                                           PyObject *Py_UNUSED(ignored)) {
    try {
        TraceReaderConfig cfg = build_config(self);
        TraceReader reader(std::move(cfg));
        return PyLong_FromSize_t(reader.get_num_lines());
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }
}

static PyMethodDef TraceReader_methods[] = {
    {"iter_lines", (PyCFunction)TraceReader_iter_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over decoded lines.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"iter_raw", (PyCFunction)TraceReader_iter_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over raw byte chunks.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"
     "    line_aligned (bool): Align chunks to line boundaries.\n"
     "    multi_line (bool): Allow multiple lines per chunk.\n"},
    {"read_lines", (PyCFunction)TraceReader_read_lines,
     METH_VARARGS | METH_KEYWORDS,
     "Read all lines and return as list.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_raw", (PyCFunction)TraceReader_read_raw,
     METH_VARARGS | METH_KEYWORDS,
     "Read all raw chunks and return as list.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"
     "    line_aligned (bool): Align chunks to line boundaries.\n"
     "    multi_line (bool): Allow multiple lines per chunk.\n"},
    {"iter_lines_json", (PyCFunction)TraceReader_iter_lines_json,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over parsed JSON objects.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_lines_json", (PyCFunction)TraceReader_read_lines_json,
     METH_VARARGS | METH_KEYWORDS,
     "Read all lines as parsed JSON objects.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    {"iter_arrow", (PyCFunction)TraceReader_iter_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over Arrow record batches.\n"
     "\n"
     "Args:\n"
     "    batch_size (int): Maximum rows per Arrow batch.\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
    {"read_arrow", (PyCFunction)TraceReader_read_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "Read all events as a materialized ArrowTable.\n"
     "\n"
     "Args:\n"
     "    batch_size (int): Maximum rows per Arrow batch.\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"},
#endif
    {"get_max_bytes", (PyCFunction)TraceReader_get_max_bytes, METH_NOARGS,
     "Get the maximum byte position (0 if unknown for compressed\n"
     "files without index)."},
    {"get_num_lines", (PyCFunction)TraceReader_get_num_lines, METH_NOARGS,
     "Get the total number of lines (0 if unknown for files without\n"
     "index)."},
    {"__enter__", (PyCFunction)TraceReader_enter, METH_NOARGS,
     "Enter the runtime context for the with statement."},
    {"__exit__", (PyCFunction)TraceReader_exit, METH_VARARGS,
     "Exit the runtime context for the with statement."},
    {NULL}};

static PyGetSetDef TraceReader_getsetters[] = {
    {"file_path", (getter)TraceReader_get_file_path, NULL,
     "Path to the trace file", NULL},
    {"index_dir", (getter)TraceReader_get_index_dir, NULL,
     "Directory for index files", NULL},
    {"has_index", (getter)TraceReader_get_has_index, NULL,
     "True if a checkpoint index was found", NULL},
    {"num_lines", (getter)TraceReader_get_num_lines_prop, NULL,
     "Total line count (reads all lines if needed)", NULL},
    {NULL}};

PyTypeObject TraceReaderType = {
    PyVarObject_HEAD_INIT(NULL, 0) "dftracer_utils_ext.TraceReader",
    sizeof(TraceReaderObject),                /* tp_basicsize */
    0,                                        /* tp_itemsize */
    (destructor)TraceReader_dealloc,          /* tp_dealloc */
    0,                                        /* tp_vectorcall_offset */
    0,                                        /* tp_getattr */
    0,                                        /* tp_setattr */
    0,                                        /* tp_as_async */
    0,                                        /* tp_repr */
    0,                                        /* tp_as_number */
    0,                                        /* tp_as_sequence */
    0,                                        /* tp_as_mapping */
    0,                                        /* tp_hash */
    0,                                        /* tp_call */
    0,                                        /* tp_str */
    0,                                        /* tp_getattro */
    0,                                        /* tp_setattro */
    0,                                        /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "TraceReader(file_path: str, index_dir: str = '',\n"
    "            checkpoint_size: int = 33554432,\n"
    "            auto_build_index: bool = False,\n"
    "            index_threshold: int = 1048576,\n"
    "            runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Smart trace file reader that auto-selects sequential or indexed\n"
    "reading based on whether an ``.idx`` sidecar exists.\n"
    "\n"
    "Args:\n"
    "    file_path (str): Path to the trace file (.pfw.gz or plain "
    "text).\n"
    "    index_dir (str): Directory to search for ``.idx`` sidecar "
    "files.\n"
    "        Empty string (default) searches next to the trace file.\n"
    "    checkpoint_size (int): Checkpoint interval in bytes for index\n"
    "        building (default 32 MB).\n"
    "    auto_build_index (bool): If True, automatically build an "
    "index\n"
    "        when none exists and the file exceeds *index_threshold*.\n"
    "    index_threshold (int): Minimum file size in bytes before\n"
    "        auto-indexing is triggered (default 8 MB).\n"
    "    runtime (Runtime or None): Runtime instance for thread pool "
    "control.\n"
    "        If None, uses the default global Runtime.\n"
    "\n"
    "Raises:\n"
    "    RuntimeError: If *file_path* does not exist or cannot be "
    "opened.\n",                /* tp_doc */
    0,                          /* tp_traverse */
    0,                          /* tp_clear */
    0,                          /* tp_richcompare */
    0,                          /* tp_weaklistoffset */
    0,                          /* tp_iter */
    0,                          /* tp_iternext */
    TraceReader_methods,        /* tp_methods */
    0,                          /* tp_members */
    TraceReader_getsetters,     /* tp_getset */
    0,                          /* tp_base */
    0,                          /* tp_dict */
    0,                          /* tp_descr_get */
    0,                          /* tp_descr_set */
    0,                          /* tp_dictoffset */
    (initproc)TraceReader_init, /* tp_init */
    0,                          /* tp_alloc */
    TraceReader_new,            /* tp_new */
};

int init_trace_reader(PyObject *m) {
    if (PyType_Ready(&TraceReaderType) < 0) return -1;

    Py_INCREF(&TraceReaderType);
    if (PyModule_AddObject(m, "TraceReader", (PyObject *)&TraceReaderType) <
        0) {
        Py_DECREF(&TraceReaderType);
        Py_DECREF(m);
        return -1;
    }

    return 0;
}
