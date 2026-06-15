#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <dftracer/utils/core/common/config.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/memory_budget.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/coro/when_all.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/core/utils/string.h>
#include <dftracer/utils/python/arrow_helpers.h>
#include <dftracer/utils/python/batch_byte_size.h>
#include <dftracer/utils/python/json.h>
#include <dftracer/utils/python/py_dict_helpers.h>
#include <dftracer/utils/python/runtime.h>
#include <dftracer/utils/python/trace_reader.h>
#include <dftracer/utils/python/trace_reader_iterator.h>
#include <dftracer/utils/utilities/common/query/query.h>
#include <dftracer/utils/utilities/composites/dft/indexing/chunk_pruner_utility.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/filesystem/pattern_directory_scanner_utility.h>
#include <dftracer/utils/utilities/indexer/index_database.h>
#include <dftracer/utils/utilities/indexer/internal/helpers.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef DFTRACER_UTILS_ENABLE_ARROW
#include <dftracer/utils/python/arrow_stream_capsule.h>
#include <dftracer/utils/utilities/common/arrow/column_builder.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
#include <dftracer/utils/utilities/common/arrow/ipc_writer.h>
#include <dftracer/utils/utilities/common/arrow/partition_writer.h>
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <dftracer/utils/utilities/composites/dft/metadata_collector_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_builder_utility.h>
#include <dftracer/utils/utilities/composites/dft/views/view_definition.h>
#include <dftracer/utils/utilities/composites/dft/views/view_reader_utility.h>
#endif

namespace {

using dftracer::utils::CoroScope;
using dftracer::utils::Runtime;
using dftracer::utils::coro::CoroTask;
using dftracer::utils::coro::when_all;
using dftracer::utils::utilities::filesystem::PatternDirectoryScannerUtility;
using dftracer::utils::utilities::filesystem::
    PatternDirectoryScannerUtilityInput;
using dftracer::utils::utilities::reader::ReadConfig;
using dftracer::utils::utilities::reader::TraceReader;
using dftracer::utils::utilities::reader::TraceReaderConfig;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
using dftracer::utils::utilities::common::arrow::ColumnType;
using dftracer::utils::utilities::common::arrow::RecordBatchBuilder;
using dftracer::utils::utilities::common::json::JsonParser;
using dftracer::utils::utilities::common::json::JsonValueHelper;
#endif
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
using dftracer::utils::utilities::common::arrow::IpcCompression;
using dftracer::utils::utilities::common::arrow::PartitionWriter;
using dftracer::utils::utilities::common::arrow::PartitionWriteStats;
using dftracer::utils::utilities::composites::dft::MetadataCollectorUtility;
using dftracer::utils::utilities::composites::dft::
    MetadataCollectorUtilityInput;
using dftracer::utils::utilities::composites::dft::views::ViewBuilderInput;
using dftracer::utils::utilities::composites::dft::views::ViewBuilderUtility;
using dftracer::utils::utilities::composites::dft::views::ViewDefinition;
using dftracer::utils::utilities::composites::dft::views::ViewReaderInput;
using dftracer::utils::utilities::composites::dft::views::ViewReaderUtility;
#endif

using dftracer::utils::python::MemoryViewBatchData;
using dftracer::utils::python::MemoryViewBatchIteratorState;

CoroTask<void> produce_lines_batched(
    std::shared_ptr<MemoryViewBatchIteratorState> state,
    dftracer::utils::coro::ChannelProducer<MemoryViewBatchData> producer,
    TraceReaderConfig cfg, ReadConfig rc, std::size_t batch_size) {
    auto guard = producer.guard();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        MemoryViewBatchData batch;
        std::size_t count = 0;

        while (auto opt = co_await gen.next()) {
            if (state->cancelled.load(std::memory_order_acquire)) break;
            auto sv = opt->content;
            Py_ssize_t offset = static_cast<Py_ssize_t>(batch.buffer.size());
            batch.buffer.insert(batch.buffer.end(), sv.begin(), sv.end());
            batch.offsets.push_back(offset);
            batch.lengths.push_back(static_cast<Py_ssize_t>(sv.size()));
            ++count;

            if (count >= batch_size) {
                auto batch_bytes = dftracer::utils::python::byte_size(batch);
                state->bytes_in_queue.fetch_add(batch_bytes,
                                                std::memory_order_acq_rel);
                if (!co_await producer.send(std::move(batch))) break;
                batch = MemoryViewBatchData{};
                count = 0;
            }
        }
        if (count > 0 && !state->cancelled.load(std::memory_order_acquire)) {
            auto batch_bytes = dftracer::utils::python::byte_size(batch);
            state->bytes_in_queue.fetch_add(batch_bytes,
                                            std::memory_order_acq_rel);
            co_await producer.send(std::move(batch));
        }
    } catch (...) {
        state->set_error(std::current_exception());
    }
}

CoroTask<void> produce_raw_batched(
    std::shared_ptr<MemoryViewBatchIteratorState> state,
    dftracer::utils::coro::ChannelProducer<MemoryViewBatchData> producer,
    TraceReaderConfig cfg, ReadConfig rc) {
    auto guard = producer.guard();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_raw(rc);
        while (auto opt = co_await gen.next()) {
            if (state->cancelled.load(std::memory_order_acquire)) break;
            MemoryViewBatchData batch;
            batch.buffer.assign(opt->data(), opt->data() + opt->size());
            batch.offsets.push_back(0);
            batch.lengths.push_back(static_cast<Py_ssize_t>(opt->size()));
            auto batch_bytes = dftracer::utils::python::byte_size(batch);
            state->bytes_in_queue.fetch_add(batch_bytes,
                                            std::memory_order_acq_rel);
            if (!co_await producer.send(std::move(batch))) break;
        }
    } catch (...) {
        state->set_error(std::current_exception());
    }
}

using dftracer::utils::utilities::common::json::JsonParser;
using dftracer::utils::utilities::common::json::JsonValueHelper;

static constexpr std::size_t ESTIMATED_BYTES_PER_LINE = 256;
static constexpr std::size_t ESTIMATED_BYTES_PER_RAW_CHUNK = 4 * 1024 * 1024;
static constexpr std::size_t ESTIMATED_BYTES_PER_JSON_EVENT = 512;
static constexpr std::size_t ESTIMATED_BYTES_PER_ARROW_ROW = 1024;

static void insert_simdjson_value(ArgsMap &map, std::string_view key,
                                  simdjson::ondemand::value val) {
    auto type = val.type();
    if (type.error()) return;
    switch (type.value_unsafe()) {
        case simdjson::ondemand::json_type::string: {
            auto r = val.get_string();
            if (!r.error()) map.insert(key, std::string(r.value_unsafe()));
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto ri = val.get_int64();
            if (!ri.error()) {
                auto v = ri.value_unsafe();
                if (v >= 0)
                    map.insert(key, static_cast<std::uint64_t>(v));
                else
                    map.insert(key, v);
            } else {
                auto rd = val.get_double();
                if (!rd.error()) map.insert(key, rd.value_unsafe());
            }
            break;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto r = val.get_bool();
            if (!r.error()) map.insert(key, r.value_unsafe());
            break;
        }
        default:
            break;
    }
}

static void parse_json_to_event(JsonParser &parser, JsonDictEvent &ev) {
    ev.top.set_valid(true);
    parser.for_each_field(
        [&](std::string_view key, simdjson::ondemand::value val) {
            if (key == "args") {
                auto obj = val.get_object();
                if (!obj.error()) {
                    ev.args.set_valid(true);
                    for (auto field : obj.value_unsafe()) {
                        if (field.error()) continue;
                        auto fkey = field.unescaped_key();
                        if (fkey.error()) continue;
                        auto fval = field.value();
                        if (fval.error()) continue;
                        insert_simdjson_value(ev.args, fkey.value_unsafe(),
                                              fval.value_unsafe());
                    }
                }
            } else {
                insert_simdjson_value(ev.top, key, val);
            }
        });
}

CoroTask<void> produce_json_dicts(
    std::shared_ptr<JsonDictIteratorState> state,
    dftracer::utils::coro::ChannelProducer<JsonDictBatch> producer,
    TraceReaderConfig cfg, ReadConfig rc, std::size_t batch_size) {
    auto guard = producer.guard();
    try {
        TraceReader reader(std::move(cfg));
        auto gen = reader.read_json(rc);
        JsonDictBatch batch;
        batch.events.reserve(batch_size);

        while (auto opt = co_await gen.next()) {
            if (state->cancelled.load(std::memory_order_acquire)) break;

            JsonDictEvent ev;
            parse_json_to_event(*opt->parser, ev);
            batch.events.push_back(std::move(ev));

            if (batch.events.size() >= batch_size) {
                auto batch_bytes = dftracer::utils::python::byte_size(batch);
                state->bytes_in_queue.fetch_add(batch_bytes,
                                                std::memory_order_acq_rel);
                if (!co_await producer.send(std::move(batch))) break;
                batch = JsonDictBatch{};
                batch.events.reserve(batch_size);
            }
        }
        if (!batch.events.empty() &&
            !state->cancelled.load(std::memory_order_acquire)) {
            auto batch_bytes = dftracer::utils::python::byte_size(batch);
            state->bytes_in_queue.fetch_add(batch_bytes,
                                            std::memory_order_acq_rel);
            co_await producer.send(std::move(batch));
        }
    } catch (...) {
        state->set_error(std::current_exception());
    }
}

static CoroTask<void> send_files_to_channel(
    std::shared_ptr<dftracer::utils::coro::Channel<std::string>> file_chan,
    const std::vector<std::string> *files, std::atomic<bool> *cancelled) {
    for (const auto &fp : *files) {
        if (cancelled->load(std::memory_order_acquire)) break;
        if (!co_await file_chan->send(fp)) break;
    }
    file_chan->close();
    co_return;
}

static CoroTask<void> json_dict_file_worker(
    std::shared_ptr<dftracer::utils::coro::Channel<std::string>> file_chan,
    dftracer::utils::coro::Channel<JsonDictBatch> *out_chan,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<JsonDictBatch> producer(out_chan);
    auto guard = producer.guard();

    while (auto file_path = co_await file_chan->receive()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;
        TraceReaderConfig cfg;
        cfg.file_path = std::move(*file_path);
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index;

        TraceReader reader(std::move(cfg));
        auto gen = reader.read_json(rc);
        JsonDictBatch batch;
        batch.events.reserve(batch_size);

        while (auto opt = co_await gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            JsonDictEvent ev;
            parse_json_to_event(*opt->parser, ev);
            batch.events.push_back(std::move(ev));
            if (batch.events.size() >= batch_size) {
                if (!co_await producer.send(std::move(batch))) co_return;
                batch = JsonDictBatch{};
                batch.events.reserve(batch_size);
            }
        }
        if (!batch.events.empty()) {
            if (!co_await producer.send(std::move(batch))) co_return;
        }
    }
    co_return;
}

static CoroTask<void> spawn_json_dict_producers(
    CoroScope &child, dftracer::utils::coro::Channel<JsonDictBatch> *out_chan,
    const std::vector<std::string> *files, const std::string *index_dir,
    std::size_t checkpoint_size, bool auto_build_index, const ReadConfig *rc,
    std::size_t batch_size, std::atomic<bool> *cancelled_ptr,
    std::size_t max_workers) {
    std::size_t num_workers = std::min(files->size(), max_workers);
    auto file_chan =
        dftracer::utils::coro::make_channel<std::string>(num_workers);

    for (std::size_t i = 0; i < num_workers; ++i) {
        child.spawn([out_chan, fc = file_chan, idx = *index_dir,
                     checkpoint_size, auto_build_index, r = *rc, batch_size,
                     cancelled_ptr](CoroScope &) {
            return json_dict_file_worker(fc, out_chan, idx, checkpoint_size,
                                         auto_build_index, r, batch_size,
                                         cancelled_ptr);
        });
    }

    child.spawn([fc = file_chan, files, cancelled_ptr](CoroScope &) {
        return send_files_to_channel(fc, files, cancelled_ptr);
    });
    co_return;
}

static CoroTask<void> produce_json_dicts_parallel(
    CoroScope &scope, JsonDictIteratorState *sp, std::string dir_path,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, std::size_t max_workers) {
    try {
        PatternDirectoryScannerUtility scanner;
        auto scan_input = PatternDirectoryScannerUtilityInput(
            dir_path, {".pfw", ".pfw.gz"}, true, false);
        auto entries = co_await scope.spawn(scanner, scan_input);

        std::vector<std::string> files;
        files.reserve(entries.size());
        for (auto &e : entries) files.push_back(e.path.string());
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            sp->channel->close();
            co_return;
        }

        auto *chan_ptr = sp->channel.get();
        auto *cancelled_ptr = &sp->cancelled;

        co_await scope.scope([chan_ptr, &files, &index_dir, checkpoint_size,
                              auto_build_index, &rc, batch_size, cancelled_ptr,
                              max_workers](CoroScope &child) -> CoroTask<void> {
            co_await spawn_json_dict_producers(
                child, chan_ptr, &files, &index_dir, checkpoint_size,
                auto_build_index, &rc, batch_size, cancelled_ptr, max_workers);
        });
    } catch (...) {
        sp->set_error(std::current_exception());
    }
}

static CoroTask<void> lines_file_worker(
    std::shared_ptr<dftracer::utils::coro::Channel<std::string>> file_chan,
    dftracer::utils::coro::Channel<MemoryViewBatchData> *out_chan,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<MemoryViewBatchData> producer(
        out_chan);
    auto guard = producer.guard();

    while (auto file_path = co_await file_chan->receive()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;
        TraceReaderConfig cfg;
        cfg.file_path = std::move(*file_path);
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index;

        TraceReader reader(std::move(cfg));
        auto gen = reader.read_lines(rc);
        MemoryViewBatchData batch;
        std::size_t count = 0;

        while (auto opt = co_await gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            auto sv = opt->content;
            Py_ssize_t offset = static_cast<Py_ssize_t>(batch.buffer.size());
            batch.buffer.insert(batch.buffer.end(), sv.begin(), sv.end());
            batch.offsets.push_back(offset);
            batch.lengths.push_back(static_cast<Py_ssize_t>(sv.size()));
            ++count;
            if (count >= batch_size) {
                if (!co_await producer.send(std::move(batch))) co_return;
                batch = MemoryViewBatchData{};
                count = 0;
            }
        }
        if (count > 0) {
            if (!co_await producer.send(std::move(batch))) co_return;
        }
    }
    co_return;
}

static CoroTask<void> spawn_lines_producers(
    CoroScope &child,
    dftracer::utils::coro::Channel<MemoryViewBatchData> *out_chan,
    const std::vector<std::string> *files, const std::string *index_dir,
    std::size_t checkpoint_size, bool auto_build_index, const ReadConfig *rc,
    std::size_t batch_size, std::atomic<bool> *cancelled_ptr,
    std::size_t max_workers) {
    std::size_t num_workers = std::min(files->size(), max_workers);
    auto file_chan =
        dftracer::utils::coro::make_channel<std::string>(num_workers);

    for (std::size_t i = 0; i < num_workers; ++i) {
        child.spawn([out_chan, fc = file_chan, idx = *index_dir,
                     checkpoint_size, auto_build_index, r = *rc, batch_size,
                     cancelled_ptr](CoroScope &) {
            return lines_file_worker(fc, out_chan, idx, checkpoint_size,
                                     auto_build_index, r, batch_size,
                                     cancelled_ptr);
        });
    }

    child.spawn([fc = file_chan, files, cancelled_ptr](CoroScope &) {
        return send_files_to_channel(fc, files, cancelled_ptr);
    });
    co_return;
}

static CoroTask<void> produce_lines_parallel(
    CoroScope &scope, MemoryViewBatchIteratorState *sp, std::string dir_path,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, std::size_t max_workers) {
    try {
        PatternDirectoryScannerUtility scanner;
        auto scan_input = PatternDirectoryScannerUtilityInput(
            dir_path, {".pfw", ".pfw.gz"}, true, false);
        auto entries = co_await scope.spawn(scanner, scan_input);

        std::vector<std::string> files;
        files.reserve(entries.size());
        for (auto &e : entries) files.push_back(e.path.string());
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            sp->channel->close();
            co_return;
        }

        auto *chan_ptr = sp->channel.get();
        auto *cancelled_ptr = &sp->cancelled;

        co_await scope.scope([chan_ptr, &files, &index_dir, checkpoint_size,
                              auto_build_index, &rc, batch_size, cancelled_ptr,
                              max_workers](CoroScope &child) -> CoroTask<void> {
            co_await spawn_lines_producers(
                child, chan_ptr, &files, &index_dir, checkpoint_size,
                auto_build_index, &rc, batch_size, cancelled_ptr, max_workers);
        });
    } catch (...) {
        sp->set_error(std::current_exception());
    }
}

static CoroTask<void> raw_file_worker(
    std::shared_ptr<dftracer::utils::coro::Channel<std::string>> file_chan,
    dftracer::utils::coro::Channel<MemoryViewBatchData> *out_chan,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<MemoryViewBatchData> producer(
        out_chan);
    auto guard = producer.guard();

    while (auto file_path = co_await file_chan->receive()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;
        TraceReaderConfig cfg;
        cfg.file_path = std::move(*file_path);
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index;

        TraceReader reader(std::move(cfg));
        auto gen = reader.read_raw(rc);
        while (auto opt = co_await gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            MemoryViewBatchData batch;
            batch.buffer.assign(opt->data(), opt->data() + opt->size());
            batch.offsets.push_back(0);
            batch.lengths.push_back(static_cast<Py_ssize_t>(opt->size()));
            if (!co_await producer.send(std::move(batch))) co_return;
        }
    }
    co_return;
}

static CoroTask<void> spawn_raw_producers(
    CoroScope &child,
    dftracer::utils::coro::Channel<MemoryViewBatchData> *out_chan,
    const std::vector<std::string> *files, const std::string *index_dir,
    std::size_t checkpoint_size, bool auto_build_index, const ReadConfig *rc,
    std::atomic<bool> *cancelled_ptr, std::size_t max_workers) {
    std::size_t num_workers = std::min(files->size(), max_workers);
    auto file_chan =
        dftracer::utils::coro::make_channel<std::string>(num_workers);

    for (std::size_t i = 0; i < num_workers; ++i) {
        child.spawn([out_chan, fc = file_chan, idx = *index_dir,
                     checkpoint_size, auto_build_index, r = *rc,
                     cancelled_ptr](CoroScope &) {
            return raw_file_worker(fc, out_chan, idx, checkpoint_size,
                                   auto_build_index, r, cancelled_ptr);
        });
    }

    child.spawn([fc = file_chan, files, cancelled_ptr](CoroScope &) {
        return send_files_to_channel(fc, files, cancelled_ptr);
    });
    co_return;
}

static CoroTask<void> produce_raw_parallel(
    CoroScope &scope, MemoryViewBatchIteratorState *sp, std::string dir_path,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t max_workers) {
    try {
        PatternDirectoryScannerUtility scanner;
        auto scan_input = PatternDirectoryScannerUtilityInput(
            dir_path, {".pfw", ".pfw.gz"}, true, false);
        auto entries = co_await scope.spawn(scanner, scan_input);

        std::vector<std::string> files;
        files.reserve(entries.size());
        for (auto &e : entries) files.push_back(e.path.string());
        std::sort(files.begin(), files.end());

        if (files.empty()) {
            sp->channel->close();
            co_return;
        }

        auto *chan_ptr = sp->channel.get();
        auto *cancelled_ptr = &sp->cancelled;

        co_await scope.scope([chan_ptr, &files, &index_dir, checkpoint_size,
                              auto_build_index, &rc, cancelled_ptr,
                              max_workers](CoroScope &child) -> CoroTask<void> {
            co_await spawn_raw_producers(child, chan_ptr, &files, &index_dir,
                                         checkpoint_size, auto_build_index, &rc,
                                         cancelled_ptr, max_workers);
        });
    } catch (...) {
        sp->set_error(std::current_exception());
    }
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW

using dftracer::utils::utilities::common::arrow::ArrowExportResult;
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
    using namespace dftracer::utils::utilities::composites::dft::internal;
    for (auto op : posix_ops::READ)
        if (op == func) return IO_READ;
    for (auto op : posix_ops::WRITE)
        if (op == func) return IO_WRITE;
    for (auto op : posix_ops::SYNC)
        if (op == func) return IO_SYNC;
    for (auto op : posix_ops::PCTL)
        if (op == func) return IO_PCTL;
    for (auto op : posix_ops::IPC)
        if (op == func) return IO_IPC;
    for (auto op : posix_ops::METADATA)
        if (op == func) return IO_METADATA;
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

// Normalize a raw JSON row (parsed with simdjson) into the semantic
// output schema.  Appends one row to `builder` with the full set of output
// columns.  Returns false if the row should be skipped (no valid name).
static bool normalize_row(RecordBatchBuilder &builder, StringArena &arena,
                          JsonParser &parser) {
    using SVH = JsonValueHelper;

    // --- Extract top-level fields ---
    auto ph = parser.get_string("ph").value_or(std::string_view{});
    auto name_sv = parser.get_string("name").value_or(std::string_view{});
    auto cat_sv = parser.get_string("cat").value_or(std::string_view{});
    auto pid_opt = parser.get_int64("pid");
    auto tid_opt = parser.get_int64("tid");
    auto ts_opt = parser.get_int64("ts");
    auto dur_opt = parser.get_int64("dur");

    // Helper lambdas to access args fields (need to rewind after each access)
    // We'll do a single pass over args instead
    std::optional<std::string_view> args_name, args_value, args_hhash,
        args_fhash;
    std::optional<int64_t> args_epoch, args_step, args_size_sum, args_ret;
    std::optional<int64_t> args_offset, args_image_idx, args_image_size;
    std::unordered_map<std::string, int64_t> args_int_map;
    std::unordered_map<std::string, double> args_float_map;

    parser.rewind();
    parser.for_each_field(
        "args", [&](std::string_view key, simdjson::ondemand::value val) {
            if (key == "name") {
                if (auto s = SVH::get_string(val)) args_name = s;
            } else if (key == "value") {
                if (auto s = SVH::get_string(val)) args_value = s;
            } else if (key == "hhash") {
                if (auto s = SVH::get_string(val)) args_hhash = s;
            } else if (key == "fhash") {
                if (auto s = SVH::get_string(val)) args_fhash = s;
            } else if (key == "epoch") {
                if (auto i = SVH::get_int64(val)) args_epoch = i;
            } else if (key == "step") {
                if (auto i = SVH::get_int64(val)) args_step = i;
            } else if (key == "size_sum") {
                if (auto i = SVH::get_int64(val)) args_size_sum = i;
            } else if (key == "ret") {
                if (auto i = SVH::get_int64(val)) args_ret = i;
            } else if (key == "offset") {
                if (auto i = SVH::get_int64(val)) args_offset = i;
            } else if (key == "image_idx") {
                if (auto i = SVH::get_int64(val)) args_image_idx = i;
            } else if (key == "image_size") {
                if (auto i = SVH::get_int64(val)) args_image_size = i;
            } else {
                // Store other int/float args for profile/sys columns
                if (auto i = SVH::get_int64(val)) {
                    args_int_map[std::string(key)] = *i;
                } else if (auto d = SVH::get_double(val)) {
                    args_float_map[std::string(key)] = *d;
                }
            }
        });

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
    if (is_M && args_name && !args_name->empty()) {
        out_name = *args_name;
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
    if (is_hash && args_value && !args_value->empty())
        builder.append_string(ci_hash, *args_value);
    if (row_type == ROW_METADATA && args_value && !args_value->empty())
        builder.append_string(ci_value, *args_value);

    // host_hash / file_hash
    if (args_hhash && !args_hhash->empty())
        builder.append_string(ci_host_hash, *args_hhash);
    if (args_fhash && !args_fhash->empty())
        builder.append_string(ci_file_hash, *args_fhash);

    // epoch / step
    if (args_epoch && *args_epoch >= 0)
        builder.append_int64(ci_epoch, *args_epoch);
    if (args_step && *args_step >= 0) builder.append_int64(ci_step, *args_step);

    // --- Temporal ---
    bool has_ts = (is_event || is_C) && ts_opt.has_value();
    bool has_dur = dur_opt.has_value();
    int64_t ts_val = 0, dur_val = 0;
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
        int8_t io_cat = IO_OTHER;

        // size priority: size_sum > POSIX ret > image_size
        if (args_size_sum) {
            builder.append_int64(ci_size, *args_size_sum);
            if (is_posix_stdio) io_cat = get_io_cat(out_name);
        } else if (is_posix_stdio) {
            io_cat = get_io_cat(out_name);
            if (args_ret && *args_ret > 0 &&
                (io_cat == IO_READ || io_cat == IO_WRITE))
                builder.append_int64(ci_size, *args_ret);
            if (args_offset && *args_offset >= 0)
                builder.append_int64(ci_offset, *args_offset);
        } else {
            if (args_image_idx && *args_image_idx > 0)
                builder.append_int64(ci_image_id, *args_image_idx);
            if (args_image_size && *args_image_size > 0 &&
                !str_contains_lower(out_name, "open"))
                builder.append_int64(ci_size, *args_image_size);
        }
        builder.append_int64(ci_io_cat, io_cat);
    }

    // --- Profile columns ---
    if (is_profile) {
        bool is_posix_stdio =
            str_iequal(cat_sv, "posix") || str_iequal(cat_sv, "stdio");
        int8_t io_cat = is_posix_stdio ? get_io_cat(out_name) : IO_OTHER;
        builder.append_int64(ci_io_cat, io_cat);

        static const char *profile_keys[] = {
            "count",      "count_max",  "count_min",  "count_sum",
            "dft_cnt",    "dur",        "dur_max",    "dur_min",
            "dur_sum",    "epoch",      "flags",      "offset",
            "offset_max", "offset_min", "offset_sum", "ret",
            "ret_max",    "ret_min",    "ret_sum",    "whence",
            "whence_max", "whence_min", "whence_sum", nullptr};
        for (const char **pk = profile_keys; *pk; ++pk) {
            auto it = args_int_map.find(*pk);
            if (it != args_int_map.end()) {
                auto idx = builder.add_or_get_column(*pk, ColumnType::INT64);
                builder.append_int64(idx, it->second);
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
            auto it = args_float_map.find(*sk);
            if (it != args_float_map.end()) {
                auto idx = builder.add_or_get_column(*sk, ColumnType::DOUBLE);
                builder.append_double(idx, it->second);
            }
        }
    }

    builder.end_row();
    return true;
}

// Flatten a simdjson object into "prefix.key" columns using native types.
// On type mismatch (same key, different type across rows), appends null.
static void flatten_object_into(RecordBatchBuilder &builder, StringArena &arena,
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

static bool build_arrow_row(RecordBatchBuilder &builder, JsonParser &parser,
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

static bool process_json_line(RecordBatchBuilder &builder, JsonParser &parser,
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

static CoroTask<void> produce_arrow_for_file(
    dftracer::utils::coro::Channel<ArrowExportResult> *chan,
    std::string file_path, std::string index_dir, std::size_t checkpoint_size,
    bool auto_build_index, ReadConfig rc, std::size_t batch_size,
    bool normalize, std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<ArrowExportResult> producer(chan);
    auto guard = producer.guard();

    TraceReaderConfig cfg;
    cfg.file_path = std::move(file_path);
    cfg.index_dir = std::move(index_dir);
    cfg.checkpoint_size = checkpoint_size;
    cfg.auto_build_index = auto_build_index;

    TraceReader reader(std::move(cfg));

    // Fast path: non-normalized Arrow build happens inside TraceReader.
    // Normalize still goes through read_json + build_arrow_row for the
    // richer schema derivation.
    if (!normalize) {
        auto batch_gen = reader.read_arrow(rc, batch_size);
        while (auto batch_opt = co_await batch_gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            if (!co_await producer.send(std::move(*batch_opt))) co_return;
        }
        co_return;
    }

    auto gen = reader.read_json(rc);
    RecordBatchBuilder builder;
    builder.reserve(batch_size);
    StringArena arena;

    while (auto opt = co_await gen.next()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;
        if (!build_arrow_row(builder, *opt->parser, arena, normalize)) continue;
        if (builder.num_rows() >= batch_size) {
            auto result = builder.finish();
            arena.clear();
            if (!co_await producer.send(std::move(result))) co_return;
            if (!builder.is_schema_locked()) builder.lock_schema();
            builder.reset(true);
            builder.reserve(batch_size);
        }
    }
    if (builder.num_rows() > 0) {
        co_await producer.send(builder.finish());
    }
    co_return;
}

static CoroTask<void> file_worker(
    std::shared_ptr<dftracer::utils::coro::Channel<std::string>> file_chan,
    dftracer::utils::coro::Channel<ArrowExportResult> *out_chan,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, bool normalize,
    std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<ArrowExportResult> producer(
        out_chan);
    auto guard = producer.guard();

    while (auto file_path = co_await file_chan->receive()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;
        TraceReaderConfig cfg;
        cfg.file_path = std::move(*file_path);
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index;

        TraceReader reader(std::move(cfg));

        if (!normalize) {
            auto batch_gen = reader.read_arrow(rc, batch_size);
            while (auto batch_opt = co_await batch_gen.next()) {
                if (cancelled->load(std::memory_order_acquire)) co_return;
                if (!co_await producer.send(std::move(*batch_opt))) co_return;
            }
            continue;
        }

        auto gen = reader.read_json(rc);
        RecordBatchBuilder builder;
        builder.reserve(batch_size);
        StringArena arena;

        while (auto opt = co_await gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            if (!build_arrow_row(builder, *opt->parser, arena, normalize))
                continue;
            if (builder.num_rows() >= batch_size) {
                auto result = builder.finish();
                arena.clear();
                if (!co_await producer.send(std::move(result))) co_return;
                if (!builder.is_schema_locked()) builder.lock_schema();
                builder.reset(true);
                builder.reserve(batch_size);
            }
        }
        if (builder.num_rows() > 0) {
            if (!co_await producer.send(builder.finish())) co_return;
        }
    }
    co_return;
}

// Extract AND-of-EQ leaves from a Query AST. Returns nullopt if the predicate
// shape is anything else (NE, range ops, IN, NOT, OR), in which case the
// uniform-match shortcut does not apply.
static std::optional<std::vector<std::pair<std::string, std::string>>>
extract_eq_leaves(
    const dftracer::utils::utilities::common::query::QueryNode &node) {
    namespace q_ns = dftracer::utils::utilities::common::query;
    using LeafVec = std::vector<std::pair<std::string, std::string>>;

    auto literal_to_string = [](const q_ns::LiteralNode &lit) -> std::string {
        return std::visit(
            [](auto &&v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>)
                    return v;
                else if constexpr (std::is_same_v<T, bool>)
                    return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, int64_t>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, uint64_t>)
                    return std::to_string(v);
                else if constexpr (std::is_same_v<T, double>)
                    return std::to_string(v);
                else
                    return {};
            },
            lit.value);
    };

    return std::visit(
        [&](const auto &n) -> std::optional<LeafVec> {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, q_ns::CompareNode>) {
                if (n.op != q_ns::CompareOp::EQ) return std::nullopt;
                return LeafVec{{n.field.path, literal_to_string(n.value)}};
            } else if constexpr (std::is_same_v<T, q_ns::AndNode>) {
                auto l = extract_eq_leaves(*n.left);
                if (!l) return std::nullopt;
                auto r = extract_eq_leaves(*n.right);
                if (!r) return std::nullopt;
                l->insert(l->end(), r->begin(), r->end());
                return l;
            } else {
                return std::nullopt;
            }
        },
        node.data);
}

// True iff every checkpoint in `chunk_idxs` has dim_stats min == max == literal
// for every leaf. Empty leaves -> false (no shortcut). Missing dim_stats for
// any (chunk, leaf) -> false (we don't know, play safe).
static bool all_chunks_uniform_match(
    const dftracer::utils::utilities::indexer::IndexDatabase &db, int fid,
    const std::vector<std::pair<std::string, std::string>> &leaves,
    const std::vector<std::uint64_t> &chunk_idxs) {
    if (leaves.empty() || chunk_idxs.empty()) return false;
    namespace indexing = dftracer::utils::utilities::composites::dft::indexing;

    for (const auto &[dim, val] : leaves) {
        auto rows = db.query_chunk_dimension_stats_for_dimension(fid, dim);
        if (rows.empty()) return false;
        std::unordered_map<std::uint64_t,
                           const indexing::ChunkDimensionStatsResult *>
            by_ckpt;
        by_ckpt.reserve(rows.size());
        for (const auto &r : rows) by_ckpt.emplace(r.checkpoint_idx, &r);
        for (auto cidx : chunk_idxs) {
            auto it = by_ckpt.find(cidx);
            if (it == by_ckpt.end()) return false;
            const auto &ds = *it->second;
            if (ds.min_value != val || ds.max_value != val) return false;
        }
    }
    return true;
}

// Byte-range work unit for checkpoint-level parallelism. Each unit covers
// one or more consecutive checkpoints from a single file. Decompression of
// a single gz file is sequential per gzip stream, so splitting at
// checkpoint-aligned byte offsets is what lets multiple workers share the
// decode work for one file.
struct ArrowWorkItem {
    std::string file_path;
    std::size_t start_byte = 0;
    std::size_t end_byte = 0;
    bool start_at_checkpoint = false;
    bool end_at_checkpoint = false;
    // When true, every kept chunk for this byte range is uniform-matching
    // (dim_stats min == max == predicate literal for every AND-of-EQ leaf),
    // so per-event predicate eval is skippable.
    bool chunk_prune_only = false;
    // Line-range work items override byte ranges: the worker passes these
    // down as LINE_RANGE on the read, and the gzip stream resolves them to
    // byte offsets via the checkpoint index. 0 = no line constraint.
    std::size_t start_line = 0;
    std::size_t end_line = 0;
};

static std::vector<ArrowWorkItem> enumerate_work_items(
    const std::vector<std::string> &files, const std::string &index_dir,
    const std::string &query_str, std::size_t max_workers,
    std::size_t clip_start_byte = 0, std::size_t clip_end_byte = 0,
    std::size_t clip_start_line = 0, std::size_t clip_end_line = 0) {
    namespace dft_internal =
        dftracer::utils::utilities::composites::dft::internal;
    namespace indexer_ns = dftracer::utils::utilities::indexer;
    namespace indexing = dftracer::utils::utilities::composites::dft::indexing;

    std::vector<ArrowWorkItem> items;
    items.reserve(files.size() * 4);

    const bool has_line_clip = (clip_start_line > 0 || clip_end_line > 0);
    auto push_unsplit = [&](const std::string &fp) {
        ArrowWorkItem item;
        item.file_path = fp;
        item.start_line = clip_start_line;
        item.end_line = clip_end_line;
        items.push_back(std::move(item));
    };

    // Parse the query once. Pruner input copies a Query, so we keep the
    // parsed form around to feed each ChunkPrunerInput without re-parsing.
    std::optional<dftracer::utils::utilities::common::query::Query> parsed;
    if (!query_str.empty()) {
        auto r = dftracer::utils::utilities::common::query::Query::from_string(
            query_str);
        if (r) parsed = std::move(*r);
    }

    // All files in a directory-mode scan share the same `.dftindex` root.
    // Group files by their resolved index path so we can open the RocksDB
    // once per index and reuse it to prune every file against that handle.
    std::unordered_map<std::string, std::vector<std::size_t>> by_index;
    for (std::size_t i = 0; i < files.size(); ++i) {
        std::string index_path =
            dft_internal::determine_index_path(files[i], index_dir);
        by_index[index_path].push_back(i);
    }

    for (auto &entry : by_index) {
        const auto &index_path = entry.first;
        const auto &file_idxs = entry.second;
        if (!fs::exists(index_path)) {
            for (auto i : file_idxs) push_unsplit(files[i]);
            continue;
        }
        std::unique_ptr<indexer_ns::IndexDatabase> idx_db;
        try {
            idx_db = std::make_unique<indexer_ns::IndexDatabase>(
                index_path,
                dftracer::utils::rocksdb::RocksDatabase::OpenMode::ReadOnly);
        } catch (...) {
            for (auto i : file_idxs) push_unsplit(files[i]);
            continue;
        }

        // Resolve fid + checkpoints per file (cheap queries).
        struct FileCtx {
            std::size_t file_idx;
            int fid;
            std::vector<indexer_ns::IndexerCheckpoint> ckpts;
        };
        std::vector<FileCtx> file_ctxs;
        file_ctxs.reserve(file_idxs.size());
        for (auto i : file_idxs) {
            FileCtx fc;
            fc.file_idx = i;
            fc.fid = idx_db->get_file_info_id(
                indexer_ns::internal::get_logical_path(files[i]));
            if (fc.fid < 0) {
                push_unsplit(files[i]);
                continue;
            }
            fc.ckpts = idx_db->query_checkpoints(fc.fid);
            if (fc.ckpts.empty()) {
                push_unsplit(files[i]);
                continue;
            }
            std::sort(fc.ckpts.begin(), fc.ckpts.end(),
                      [](const auto &a, const auto &b) {
                          return a.first_line_num < b.first_line_num;
                      });
            file_ctxs.push_back(std::move(fc));
        }

        // Batch-prune all files against the shared index: dim_stats and
        // chunk_statistics are loaded in one RocksDB scan each instead of
        // one scan per file.
        std::vector<indexing::ChunkPrunerOutput> pruner_outs(file_ctxs.size());
        if (parsed && !file_ctxs.empty()) {
            indexing::ChunkPrunerBatchInput batch_in;
            batch_in.index_path = index_path;
            batch_in.external_db = idx_db.get();
            batch_in.items.reserve(file_ctxs.size());
            for (auto &fc : file_ctxs) {
                batch_in.items.push_back({files[fc.file_idx], *parsed});
            }
            indexing::ChunkPrunerUtility pruner;
            auto batch_out = pruner.process_batch(batch_in);
            if (batch_out.success) {
                pruner_outs = std::move(batch_out.outputs);
            }
        }

        // For AND-of-EQ predicates, precompute uniform-match leaves once.
        // Per-file pure_match is checked inline below and lets workers skip
        // per-event predicate eval on chunks where dim_stats min == max ==
        // literal for every leaf.
        std::optional<std::vector<std::pair<std::string, std::string>>>
            eq_leaves;
        if (parsed) eq_leaves = extract_eq_leaves(parsed->root());

        for (std::size_t fc_idx = 0; fc_idx < file_ctxs.size(); ++fc_idx) {
            auto &fc = file_ctxs[fc_idx];
            const auto &fp = files[fc.file_idx];

            // Pruner chunk_idx semantics: 0-indexed over uncompressed
            // slices. fc.ckpts holds gzip recovery points; recovery point
            // fc.ckpts[k] sits at the START of pruner chunk (k+1). Pruner
            // chunk 0 has no recovery point at its start (decoded from
            // gzip stream start). Total pruner chunks = fc.ckpts.size()+1.
            const std::size_t total_chunks = fc.ckpts.size() + 1;
            auto chunk_start_byte = [&](std::uint64_t cidx) -> std::size_t {
                if (cidx == 0) return 0;
                return fc.ckpts[cidx - 1].uc_offset;
            };
            auto chunk_end_byte = [&](std::uint64_t cidx) -> std::size_t {
                if (cidx == 0)
                    return fc.ckpts.empty() ? 0 : fc.ckpts[0].uc_offset;
                std::size_t k = cidx - 1;
                return fc.ckpts[k].uc_offset + fc.ckpts[k].uc_size;
            };
            // Line ranges for a chunk. Chunk 0 covers everything before the
            // first recovery point; chunk k>=1 spans recovery point (k-1).
            auto chunk_first_line = [&](std::uint64_t cidx) -> std::size_t {
                if (cidx == 0) return 1;
                return fc.ckpts[cidx - 1].first_line_num;
            };
            auto chunk_last_line = [&](std::uint64_t cidx) -> std::size_t {
                if (cidx == 0) {
                    if (fc.ckpts.empty()) return SIZE_MAX;
                    return fc.ckpts[0].first_line_num > 0
                               ? fc.ckpts[0].first_line_num - 1
                               : 0;
                }
                return fc.ckpts[cidx - 1].last_line_num;
            };

            std::vector<std::uint64_t> keep_chunks;
            keep_chunks.reserve(total_chunks);
            if (parsed) {
                const auto &pr = pruner_outs[fc_idx];
                if (pr.success && !pr.file_may_match) {
                    continue;  // whole file pruned
                }
                if (pr.success && !pr.candidate_checkpoints.empty() &&
                    pr.candidate_checkpoints.size() < pr.total_checkpoints) {
                    for (auto cidx : pr.candidate_checkpoints) {
                        if (cidx < total_chunks) keep_chunks.push_back(cidx);
                    }
                    std::sort(keep_chunks.begin(), keep_chunks.end());
                    keep_chunks.erase(
                        std::unique(keep_chunks.begin(), keep_chunks.end()),
                        keep_chunks.end());
                } else {
                    for (std::uint64_t c = 0; c < total_chunks; ++c)
                        keep_chunks.push_back(c);
                }
            } else {
                for (std::uint64_t c = 0; c < total_chunks; ++c)
                    keep_chunks.push_back(c);
            }

            // Intersect with the user's line range so workers only touch
            // chunks that actually overlap it. Each work item carries the
            // sub-line-range; LINE_RANGE on the read maps it back to bytes
            // via the same checkpoint table the gzip stream uses.
            if (has_line_clip) {
                std::size_t lo = clip_start_line > 0 ? clip_start_line : 1;
                std::size_t hi = clip_end_line > 0 ? clip_end_line : SIZE_MAX;
                std::vector<std::uint64_t> filtered;
                filtered.reserve(keep_chunks.size());
                for (auto c : keep_chunks) {
                    std::size_t cf = chunk_first_line(c);
                    std::size_t cl = chunk_last_line(c);
                    if (cl < lo || cf > hi) continue;
                    filtered.push_back(c);
                }
                keep_chunks = std::move(filtered);
            }

            if (keep_chunks.empty()) continue;

            // All-or-nothing per file: if every kept chunk is uniform-matching
            // for every leaf, every work item from this file gets the
            // chunk_prune_only fast path. Mixed files fall back to per-event
            // eval to stay safe.
            bool file_pure_match = false;
            if (eq_leaves && !eq_leaves->empty() && idx_db) {
                file_pure_match = all_chunks_uniform_match(
                    *idx_db, fc.fid, *eq_leaves, keep_chunks);
            }

            std::size_t target_ranges = std::max<std::size_t>(1, max_workers);
            std::size_t per_range = std::max<std::size_t>(
                1, (keep_chunks.size() + target_ranges - 1) / target_ranges);

            std::size_t group_start = 0;
            while (group_start < keep_chunks.size()) {
                std::size_t group_end = group_start;
                std::size_t emitted = 0;
                while (group_end < keep_chunks.size() && emitted < per_range) {
                    if (group_end > group_start &&
                        keep_chunks[group_end] !=
                            keep_chunks[group_end - 1] + 1) {
                        break;
                    }
                    ++group_end;
                    ++emitted;
                }
                std::uint64_t scidx = keep_chunks[group_start];
                std::uint64_t ecidx = keep_chunks[group_end - 1];
                std::size_t start_byte = chunk_start_byte(scidx);
                std::size_t end_byte = chunk_end_byte(ecidx);
                // start_at_checkpoint: a gzip recovery point sits at
                // start_byte (true for any cidx>=1; false for the implicit
                // chunk 0 which decodes from stream start).
                bool start_at_checkpoint = (scidx >= 1);
                bool end_at_checkpoint = (group_end < keep_chunks.size());
                if (has_line_clip) {
                    std::size_t lo = clip_start_line > 0 ? clip_start_line : 1;
                    std::size_t hi =
                        clip_end_line > 0 ? clip_end_line : SIZE_MAX;
                    std::size_t cluster_first = chunk_first_line(scidx);
                    std::size_t cluster_last = chunk_last_line(ecidx);
                    std::size_t item_start =
                        std::max<std::size_t>(lo, cluster_first);
                    std::size_t item_end =
                        std::min<std::size_t>(hi, cluster_last);
                    if (item_start > item_end) {
                        group_start = group_end;
                        continue;
                    }
                    ArrowWorkItem item;
                    item.file_path = fp;
                    item.chunk_prune_only = file_pure_match;
                    item.start_line = item_start;
                    item.end_line = item_end;
                    items.push_back(std::move(item));
                    group_start = group_end;
                    continue;
                }
                if (clip_end_byte > clip_start_byte) {
                    if (start_byte < clip_start_byte) {
                        start_byte = clip_start_byte;
                        start_at_checkpoint = false;
                    }
                    if (end_byte > clip_end_byte) {
                        end_byte = clip_end_byte;
                        end_at_checkpoint = false;
                    }
                    if (start_byte >= end_byte) {
                        group_start = group_end;
                        continue;
                    }
                }
                items.push_back({fp, start_byte, end_byte, start_at_checkpoint,
                                 end_at_checkpoint, file_pure_match});
                group_start = group_end;
            }
        }
    }
    return items;
}

static CoroTask<void> send_work_items_to_channel(
    std::shared_ptr<dftracer::utils::coro::Channel<ArrowWorkItem>> chan,
    const std::vector<ArrowWorkItem> *items, std::atomic<bool> *cancelled) {
    for (const auto &it : *items) {
        if (cancelled->load(std::memory_order_acquire)) break;
        if (!co_await chan->send(it)) break;
    }
    chan->close();
    co_return;
}

static CoroTask<void> checkpoint_worker(
    std::shared_ptr<dftracer::utils::coro::Channel<ArrowWorkItem>> work_chan,
    dftracer::utils::coro::Channel<ArrowExportResult> *out_chan,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, bool normalize,
    std::atomic<bool> *cancelled) {
    dftracer::utils::coro::ChannelProducer<ArrowExportResult> producer(
        out_chan);
    auto guard = producer.guard();

    // Cache readers keyed by file path so we don't re-probe the same file
    // when successive work items land on it.
    std::unordered_map<std::string, std::shared_ptr<TraceReader>> readers;

    while (auto item = co_await work_chan->receive()) {
        if (cancelled->load(std::memory_order_acquire)) co_return;

        auto &reader_ptr = readers[item->file_path];
        if (!reader_ptr) {
            TraceReaderConfig cfg;
            cfg.file_path = item->file_path;
            cfg.index_dir = index_dir;
            cfg.checkpoint_size = checkpoint_size;
            cfg.auto_build_index = auto_build_index;
            reader_ptr = std::make_shared<TraceReader>(std::move(cfg));
        }

        ReadConfig local_rc = rc;
        if (item->start_line > 0 || item->end_line > 0) {
            // Line-range work items: the read drives off LINE_RANGE; the
            // gzip stream resolves it back to byte offsets via checkpoints.
            local_rc.start_line = item->start_line;
            local_rc.end_line = item->end_line;
            local_rc.start_byte = 0;
            local_rc.end_byte = 0;
            local_rc.start_at_checkpoint = false;
            local_rc.end_at_checkpoint = false;
        } else {
            local_rc.start_byte = item->start_byte;
            local_rc.end_byte = item->end_byte;
            local_rc.start_at_checkpoint = item->start_at_checkpoint;
            local_rc.end_at_checkpoint = item->end_at_checkpoint;
        }
        // Pruning already happened at enumeration time; avoid the per-
        // work-item RocksDB opens that would otherwise dwarf the actual
        // read cost at directory scale (256 files * N ranges).
        local_rc.skip_pruning = true;
        // chunks pre-classified as uniform-matching skip per-event eval.
        if (item->chunk_prune_only) local_rc.chunk_prune_only = true;

        if (!normalize) {
            auto batch_gen = reader_ptr->read_arrow(local_rc, batch_size);
            while (auto batch_opt = co_await batch_gen.next()) {
                if (cancelled->load(std::memory_order_acquire)) co_return;
                if (!co_await producer.send(std::move(*batch_opt))) co_return;
            }
            continue;
        }

        auto gen = reader_ptr->read_json(local_rc);
        RecordBatchBuilder builder;
        builder.reserve(batch_size);
        StringArena arena;

        while (auto opt = co_await gen.next()) {
            if (cancelled->load(std::memory_order_acquire)) co_return;
            if (!build_arrow_row(builder, *opt->parser, arena, normalize))
                continue;
            if (builder.num_rows() >= batch_size) {
                auto result = builder.finish();
                arena.clear();
                if (!co_await producer.send(std::move(result))) co_return;
                if (!builder.is_schema_locked()) builder.lock_schema();
                builder.reset(true);
                builder.reserve(batch_size);
            }
        }
        if (builder.num_rows() > 0) {
            if (!co_await producer.send(builder.finish())) co_return;
        }
    }
    co_return;
}

static CoroTask<void> spawn_arrow_producers(
    CoroScope &child,
    dftracer::utils::coro::Channel<ArrowExportResult> *out_chan,
    const std::vector<ArrowWorkItem> *work_items, const std::string *index_dir,
    std::size_t checkpoint_size, bool auto_build_index, const ReadConfig *rc,
    std::size_t batch_size, bool normalize, std::atomic<bool> *cancelled_ptr,
    std::size_t max_workers) {
    std::size_t num_workers = std::min(work_items->size(), max_workers);
    if (num_workers == 0) num_workers = 1;
    auto work_chan =
        dftracer::utils::coro::make_channel<ArrowWorkItem>(num_workers);

    for (std::size_t i = 0; i < num_workers; ++i) {
        child.spawn([out_chan, wc = work_chan, idx = *index_dir,
                     checkpoint_size, auto_build_index, r = *rc, batch_size,
                     normalize, cancelled_ptr](CoroScope &) {
            return checkpoint_worker(wc, out_chan, idx, checkpoint_size,
                                     auto_build_index, r, batch_size, normalize,
                                     cancelled_ptr);
        });
    }

    child.spawn([wc = work_chan, work_items, cancelled_ptr](CoroScope &) {
        return send_work_items_to_channel(wc, work_items, cancelled_ptr);
    });
    co_return;
}

static CoroTask<void> produce_arrow_batches_for_files(
    CoroScope &scope, ArrowIteratorState *sp, std::vector<std::string> files,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, bool normalize,
    std::size_t max_workers) {
    try {
        if (files.empty()) {
            sp->channel->close();
            co_return;
        }

        auto work_items = enumerate_work_items(
            files, index_dir, rc.query, max_workers, rc.start_byte, rc.end_byte,
            rc.start_line, rc.end_line);
        if (work_items.empty()) {
            sp->channel->close();
            co_return;
        }

        auto *chan_ptr = sp->channel.get();
        auto *cancelled_ptr = &sp->cancelled;

        co_await scope.scope([chan_ptr, &work_items, &index_dir,
                              checkpoint_size, auto_build_index, &rc,
                              batch_size, normalize, cancelled_ptr,
                              max_workers](CoroScope &child) -> CoroTask<void> {
            co_await spawn_arrow_producers(
                child, chan_ptr, &work_items, &index_dir, checkpoint_size,
                auto_build_index, &rc, batch_size, normalize, cancelled_ptr,
                max_workers);
        });
    } catch (...) {
        sp->set_error(std::current_exception());
    }
}

static CoroTask<void> produce_arrow_batches_parallel(
    CoroScope &scope, ArrowIteratorState *sp, std::string dir_path,
    std::string index_dir, std::size_t checkpoint_size, bool auto_build_index,
    ReadConfig rc, std::size_t batch_size, bool normalize,
    std::size_t max_workers) {
    try {
        PatternDirectoryScannerUtility scanner;
        auto scan_input = PatternDirectoryScannerUtilityInput(
            dir_path, {".pfw", ".pfw.gz"}, true, false);
        auto entries = co_await scope.spawn(scanner, scan_input);

        std::vector<std::string> files;
        files.reserve(entries.size());
        for (auto &e : entries) files.push_back(e.path.string());
        std::sort(files.begin(), files.end());

        co_await produce_arrow_batches_for_files(
            scope, sp, std::move(files), std::move(index_dir), checkpoint_size,
            auto_build_index, std::move(rc), batch_size, normalize,
            max_workers);
    } catch (...) {
        sp->set_error(std::current_exception());
    }
}

CoroTask<void> produce_arrow_batches(
    std::shared_ptr<ArrowIteratorState> state,
    dftracer::utils::coro::ChannelProducer<ArrowExportResult> producer,
    TraceReaderConfig cfg, ReadConfig rc, std::size_t batch_size,
    bool flatten_objects = false, bool normalize = false) {
    (void)flatten_objects;

    auto guard = producer.guard();
    try {
        TraceReader reader(std::move(cfg));

        if (!normalize) {
            auto batch_gen = reader.read_arrow(rc, batch_size);
            while (auto batch_opt = co_await batch_gen.next()) {
                if (state->cancelled.load(std::memory_order_acquire)) break;
                auto result_bytes =
                    dftracer::utils::python::byte_size(*batch_opt);
                state->bytes_in_queue.fetch_add(result_bytes,
                                                std::memory_order_acq_rel);
                if (!co_await producer.send(std::move(*batch_opt))) break;
            }
            co_return;
        }

        auto gen = reader.read_json(rc);
        RecordBatchBuilder builder;
        builder.reserve(batch_size);

        StringArena arena;

        while (auto opt = co_await gen.next()) {
            if (state->cancelled.load(std::memory_order_acquire)) break;
            if (!build_arrow_row(builder, *opt->parser, arena, normalize))
                continue;

            if (builder.num_rows() >= batch_size) {
                auto result = builder.finish();
                arena.clear();
                auto result_bytes = dftracer::utils::python::byte_size(result);
                state->bytes_in_queue.fetch_add(result_bytes,
                                                std::memory_order_acq_rel);
                if (!co_await producer.send(std::move(result))) break;
                if (!builder.is_schema_locked()) {
                    builder.lock_schema();
                }
                builder.reset(true);
                builder.reserve(batch_size);
            }
        }

        if (builder.num_rows() > 0 &&
            !state->cancelled.load(std::memory_order_acquire)) {
            auto result = builder.finish();
            auto result_bytes = dftracer::utils::python::byte_size(result);
            state->bytes_in_queue.fetch_add(result_bytes,
                                            std::memory_order_acq_rel);
            co_await producer.send(std::move(result));
        }
    } catch (...) {
        state->set_error(std::current_exception());
    }
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

struct WriteArrowStats {
    std::unordered_map<std::string, PartitionWriteStats> partitions;
    int64_t total_rows = 0;
    int64_t total_uncompressed_bytes = 0;
};

struct WriteArrowResult {
    WriteArrowStats stats;
    std::string error;
    std::uint64_t chunks_scanned = 0;
    std::uint64_t chunks_skipped = 0;
};

CoroTask<WriteArrowResult> write_arrow_pipeline(
    std::string file_path, std::string index_path, std::size_t checkpoint_size,
    std::vector<ViewDefinition> views, std::string output_path,
    int64_t chunk_size_bytes, IpcCompression compression,
    std::size_t event_batch_size) {
    namespace dft_internal =
        dftracer::utils::utilities::composites::dft::internal;
    WriteArrowResult result;

    try {
        if (views.empty()) {
            views.push_back(ViewDefinition().with_name("all"));
        }

        std::string resolved_index =
            index_path.empty()
                ? dft_internal::determine_index_path(file_path, "")
                : index_path;

        auto meta_input = MetadataCollectorUtilityInput::from_file(file_path)
                              .with_checkpoint_size(checkpoint_size)
                              .with_index(resolved_index);
        auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);
        if (!metadata.success) {
            result.error =
                "Failed to collect metadata: " + metadata.error_message;
            co_return result;
        }

        for (const auto &view : views) {
            std::string view_output = output_path;
            if (views.size() > 1 || view.name != "all") {
                view_output = output_path + "/" + view.name;
            }

            PartitionWriter writer;
            int rc_open = co_await writer.open(view_output, chunk_size_bytes,
                                               compression);
            if (rc_open != 0) {
                result.error =
                    "Failed to open partition writer for view: " + view.name;
                co_return result;
            }

            ViewBuilderInput builder_input;
            builder_input.with_view(view)
                .with_file_path(file_path)
                .with_index_path(resolved_index)
                .with_uncompressed_size(metadata.uncompressed_size)
                .with_num_checkpoints(metadata.num_checkpoints);

            auto build_output =
                co_await ViewBuilderUtility{}.process(builder_input);
            if (!build_output.success) {
                result.error = "ViewBuilder failed for view: " + view.name;
                co_return result;
            }

            result.chunks_skipped += build_output.skipped_checkpoints;

            if (!build_output.file_may_match) {
                auto stats = co_await writer.close();
                result.stats.partitions[view.name] = std::move(stats);
                continue;
            }

            RecordBatchBuilder builder;
            bool schema_locked = false;

            for (const auto &candidate : build_output.candidates) {
                ViewReaderInput reader_input;
                reader_input.with_file_path(file_path)
                    .with_index_path(resolved_index)
                    .with_checkpoint_size(checkpoint_size)
                    .with_byte_range(candidate.start_byte, candidate.end_byte)
                    .with_checkpoint_idx(candidate.checkpoint_idx)
                    .with_event_batch_size(event_batch_size)
                    .with_view(view);
                reader_input.query = view.query;

                ViewReaderUtility reader;
                auto gen = reader.process(reader_input);
                while (auto opt = co_await gen.next()) {
                    auto arrow_batch = opt->to_arrow(builder);
                    int rc_write = co_await writer.write_batch(arrow_batch);
                    if (rc_write != 0) {
                        result.error =
                            "Failed to write batch for view: " + view.name;
                        co_return result;
                    }
                    if (!schema_locked) {
                        builder.lock_schema();
                        schema_locked = true;
                    }
                    builder.reset(true);
                }
                result.chunks_scanned++;
            }

            auto stats = co_await writer.close();
            result.stats.partitions[view.name] = std::move(stats);
            result.stats.total_rows +=
                result.stats.partitions[view.name].total_rows;
            result.stats.total_uncompressed_bytes +=
                result.stats.partitions[view.name].total_uncompressed_bytes;
        }
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    co_return result;
}

struct ViewChunkInfo {
    std::uint64_t checkpoint_idx;
    std::size_t start_byte;
    std::size_t end_byte;
};

struct GetViewChunksResult {
    std::vector<ViewChunkInfo> chunks;
    std::uint64_t total_checkpoints = 0;
    std::uint64_t skipped_checkpoints = 0;
    bool file_may_match = false;
    std::string error;
};

CoroTask<GetViewChunksResult> get_view_chunks_pipeline(
    std::string file_path, std::string index_path, std::size_t checkpoint_size,
    ViewDefinition view) {
    namespace dft_internal =
        dftracer::utils::utilities::composites::dft::internal;
    GetViewChunksResult result;

    try {
        std::string resolved_index =
            index_path.empty()
                ? dft_internal::determine_index_path(file_path, "")
                : index_path;

        auto meta_input = MetadataCollectorUtilityInput::from_file(file_path)
                              .with_checkpoint_size(checkpoint_size)
                              .with_index(resolved_index);
        auto metadata = co_await MetadataCollectorUtility{}.process(meta_input);
        if (!metadata.success) {
            result.error =
                "Failed to collect metadata: " + metadata.error_message;
            co_return result;
        }

        ViewBuilderInput builder_input;
        builder_input.with_view(view)
            .with_file_path(file_path)
            .with_index_path(resolved_index)
            .with_uncompressed_size(metadata.uncompressed_size)
            .with_num_checkpoints(metadata.num_checkpoints);

        auto build_output =
            co_await ViewBuilderUtility{}.process(builder_input);
        if (!build_output.success) {
            result.error = "ViewBuilder failed";
            co_return result;
        }

        result.file_may_match = build_output.file_may_match;
        result.total_checkpoints = build_output.total_checkpoints;
        result.skipped_checkpoints = build_output.skipped_checkpoints;

        for (const auto &candidate : build_output.candidates) {
            result.chunks.push_back({candidate.checkpoint_idx,
                                     candidate.start_byte, candidate.end_byte});
        }
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    co_return result;
}

struct WriteViewChunkResult {
    std::string output_file;
    std::uint64_t events_matched = 0;
    std::uint64_t events_scanned = 0;
    int64_t rows_written = 0;
    int64_t bytes_written = 0;
    std::string error;
};

CoroTask<WriteViewChunkResult> write_view_chunk_pipeline(
    std::string file_path, std::string index_path, std::size_t checkpoint_size,
    ViewDefinition view, std::uint64_t checkpoint_idx, std::size_t start_byte,
    std::size_t end_byte, std::string output_file, IpcCompression compression,
    std::size_t event_batch_size) {
    namespace dft_internal =
        dftracer::utils::utilities::composites::dft::internal;
    WriteViewChunkResult result;
    result.output_file = output_file;

    try {
        std::string resolved_index =
            index_path.empty()
                ? dft_internal::determine_index_path(file_path, "")
                : index_path;

        dftracer::utils::utilities::common::arrow::IpcWriter writer;
        int rc_open = co_await writer.open(output_file, compression);
        if (rc_open != 0) {
            result.error = "Failed to open output file";
            co_return result;
        }

        ViewReaderInput reader_input;
        reader_input.with_file_path(file_path)
            .with_index_path(resolved_index)
            .with_checkpoint_size(checkpoint_size)
            .with_byte_range(start_byte, end_byte)
            .with_checkpoint_idx(checkpoint_idx)
            .with_event_batch_size(event_batch_size)
            .with_view(view);
        reader_input.query = view.query;

        RecordBatchBuilder builder;
        bool schema_locked = false;

        ViewReaderUtility reader;
        auto gen = reader.process(reader_input);
        while (auto opt = co_await gen.next()) {
            result.events_matched += opt->events_matched;
            result.events_scanned += opt->events_scanned;
            auto batch = opt->to_arrow(builder);
            if (batch.valid()) {
                result.rows_written += batch.num_rows();
                int rc = co_await writer.write_batch(batch);
                if (rc != 0) {
                    result.error = "Failed to write batch";
                    co_return result;
                }
                if (!schema_locked) {
                    builder.lock_schema();
                    schema_locked = true;
                }
                builder.reset(true);
            }
        }

        int rc = co_await writer.close();
        if (rc != 0) {
            result.error = "Failed to close output file";
        }
    } catch (const std::exception &e) {
        result.error = e.what();
    }
    co_return result;
}

struct ChunkDescriptor {
    std::uint64_t checkpoint_idx;
    std::size_t start_byte;
    std::size_t end_byte;
    std::string output_file;
};

struct WriteViewChunksResult {
    std::vector<WriteViewChunkResult> results;
    int64_t total_rows = 0;
    int64_t total_events_matched = 0;
};

CoroTask<WriteViewChunksResult> write_view_chunks_pipeline(
    std::string file_path, std::string index_path, std::size_t checkpoint_size,
    ViewDefinition view, std::vector<ChunkDescriptor> chunks,
    IpcCompression compression, std::size_t event_batch_size) {
    WriteViewChunksResult result;

    if (chunks.empty()) {
        co_return result;
    }

    std::vector<CoroTask<WriteViewChunkResult>> tasks;
    tasks.reserve(chunks.size());

    for (const auto &chunk : chunks) {
        tasks.push_back(write_view_chunk_pipeline(
            file_path, index_path, checkpoint_size, view, chunk.checkpoint_idx,
            chunk.start_byte, chunk.end_byte, chunk.output_file, compression,
            event_batch_size));
    }

    result.results = co_await when_all(std::move(tasks));

    for (const auto &r : result.results) {
        result.total_rows += r.rows_written;
        result.total_events_matched += r.events_matched;
    }

    co_return result;
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC

TraceReaderConfig build_config(TraceReaderObject *self) {
    TraceReaderConfig cfg;
    cfg.file_path = PyUnicode_AsUTF8(self->file_path);
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx) cfg.index_dir = idx;
    cfg.checkpoint_size = self->checkpoint_size;
    cfg.auto_build_index = self->auto_build_index != 0;
    return cfg;
}

static Runtime *get_runtime(TraceReaderObject *self) {
    if (self->runtime_obj) {
        return ((RuntimeObject *)self->runtime_obj)->runtime.get();
    }
    return get_default_runtime();
}

static TraceReaderIteratorObject *make_memoryview_iterator(
    std::shared_ptr<MemoryViewBatchIteratorState> state) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->batch_state)
        std::shared_ptr<MemoryViewBatchIteratorState>(std::move(state));
    it->current_batch = NULL;
    it->batch_index = 0;
    new (&it->json_dict_state) std::shared_ptr<JsonDictIteratorState>();
    new (&it->json_dict_current_batch) std::shared_ptr<JsonDictBatch>();
    it->json_dict_index = 0;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    new (&it->arrow_state) std::shared_ptr<ArrowIteratorState>();
#endif
    it->mode = IteratorMode::MEMORYVIEW;
    return it;
}

static TraceReaderIteratorObject *make_json_dict_iterator(
    std::shared_ptr<JsonDictIteratorState> state) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->batch_state) std::shared_ptr<MemoryViewBatchIteratorState>();
    it->current_batch = NULL;
    it->batch_index = 0;
    new (&it->json_dict_state)
        std::shared_ptr<JsonDictIteratorState>(std::move(state));
    new (&it->json_dict_current_batch) std::shared_ptr<JsonDictBatch>();
    it->json_dict_index = 0;
#ifdef DFTRACER_UTILS_ENABLE_ARROW
    new (&it->arrow_state) std::shared_ptr<ArrowIteratorState>();
#endif
    it->mode = IteratorMode::JSON_DICT;
    return it;
}

#ifdef DFTRACER_UTILS_ENABLE_ARROW
static TraceReaderIteratorObject *make_arrow_iterator(
    std::shared_ptr<ArrowIteratorState> state) {
    TraceReaderIteratorObject *it =
        (TraceReaderIteratorObject *)TraceReaderIteratorType.tp_alloc(
            &TraceReaderIteratorType, 0);
    if (!it) return NULL;
    new (&it->batch_state) std::shared_ptr<MemoryViewBatchIteratorState>();
    it->current_batch = NULL;
    it->batch_index = 0;
    new (&it->json_dict_state) std::shared_ptr<JsonDictIteratorState>();
    new (&it->json_dict_current_batch) std::shared_ptr<JsonDictBatch>();
    it->json_dict_index = 0;
    new (&it->arrow_state)
        std::shared_ptr<ArrowIteratorState>(std::move(state));
    it->mode = IteratorMode::ARROW;
    return it;
}
#endif

}  // namespace

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
        self->has_index = 0;
        self->runtime_obj = NULL;
    }
    return (PyObject *)self;
}

static int TraceReader_init(TraceReaderObject *self, PyObject *args,
                            PyObject *kwds) {
    static const char *kwlist[] = {
        "path",    "index_dir", "checkpoint_size", "auto_build_index",
        "runtime", NULL};

    const char *file_path;
    const char *index_dir = "";
    std::size_t checkpoint_size = 32 * 1024 * 1024;
    int auto_build_index = 0;
    PyObject *runtime_arg = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|snpO", (char **)kwlist,
                                     &file_path, &index_dir, &checkpoint_size,
                                     &auto_build_index, &runtime_arg)) {
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

    try {
        TraceReaderConfig cfg;
        cfg.file_path = file_path;
        cfg.index_dir = index_dir;
        cfg.checkpoint_size = checkpoint_size;
        cfg.auto_build_index = auto_build_index != 0;
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
    static const char *kwlist[] = {"start_line",    "end_line",    "start_byte",
                                   "end_byte",      "buffer_size", "query",
                                   "memory_budget", NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;
    Py_ssize_t memory_budget = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnzn", (char **)kwlist, &start_line, &end_line,
            &start_byte, &end_byte, &buffer_size, &query_str, &memory_budget)) {
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

    auto state = std::make_shared<MemoryViewBatchIteratorState>();
    state->memory_budget_bytes = dftracer::utils::compute_memory_budget(
        static_cast<std::size_t>(memory_budget));

    Runtime *rt = get_runtime(self);
    std::size_t max_workers = rt->threads();
    constexpr std::size_t LINE_BATCH_SIZE = 1024;
    std::size_t capacity = dftracer::utils::compute_channel_capacity(
        state->memory_budget_bytes, LINE_BATCH_SIZE * ESTIMATED_BYTES_PER_LINE,
        max_workers);
    state->channel =
        dftracer::utils::coro::make_channel<MemoryViewBatchData>(capacity);
    auto *sp = state.get();

    try {
        bool is_dir = fs::is_directory(cfg.file_path);
        if (is_dir) {
            auto handle = rt->scope(
                "iter_lines_parallel",
                [sp, dir_path = cfg.file_path, index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc,
                 max_workers](CoroScope &scope) -> CoroTask<void> {
                    co_await produce_lines_parallel(
                        scope, sp, dir_path, index_dir, checkpoint_size,
                        auto_build_index, rc, LINE_BATCH_SIZE, max_workers);
                });
            state->task_future = handle.future;
        } else {
            auto handle = rt->submit(
                produce_lines_batched(state, state->channel->producer(), cfg,
                                      rc, LINE_BATCH_SIZE),
                "iter_lines");
            state->task_future = handle.future;
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_memoryview_iterator(std::move(state));
    return (PyObject *)it;
}

static PyObject *TraceReader_iter_raw(TraceReaderObject *self, PyObject *args,
                                      PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",    "start_byte",
                                   "end_byte",   "buffer_size", "line_aligned",
                                   "multi_line", "query",       "memory_budget",
                                   NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    int line_aligned = 1;
    int multi_line = 1;
    const char *query_str = NULL;
    Py_ssize_t memory_budget = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnppzn", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &line_aligned,
                                     &multi_line, &query_str, &memory_budget)) {
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

    auto state = std::make_shared<MemoryViewBatchIteratorState>();
    state->memory_budget_bytes = dftracer::utils::compute_memory_budget(
        static_cast<std::size_t>(memory_budget));

    Runtime *rt = get_runtime(self);
    std::size_t max_workers = rt->threads();
    std::size_t capacity = dftracer::utils::compute_channel_capacity(
        state->memory_budget_bytes, ESTIMATED_BYTES_PER_RAW_CHUNK, max_workers);
    state->channel =
        dftracer::utils::coro::make_channel<MemoryViewBatchData>(capacity);
    auto *sp = state.get();

    try {
        bool is_dir = fs::is_directory(cfg.file_path);
        if (is_dir) {
            auto handle = rt->scope(
                "iter_raw_parallel",
                [sp, dir_path = cfg.file_path, index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc,
                 max_workers](CoroScope &scope) -> CoroTask<void> {
                    co_await produce_raw_parallel(
                        scope, sp, dir_path, index_dir, checkpoint_size,
                        auto_build_index, rc, max_workers);
                });
            state->task_future = handle.future;
        } else {
            auto handle = rt->submit(
                produce_raw_batched(state, state->channel->producer(), cfg, rc),
                "iter_raw");
            state->task_future = handle.future;
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_memoryview_iterator(std::move(state));
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

static PyObject *TraceReader_iter_json(TraceReaderObject *self, PyObject *args,
                                       PyObject *kwds) {
    static const char *kwlist[] = {"start_line", "end_line",      "start_byte",
                                   "end_byte",   "buffer_size",   "query",
                                   "batch_size", "memory_budget", NULL};
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;
    Py_ssize_t batch_size = 1024;
    Py_ssize_t memory_budget = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nnnnnznn", (char **)kwlist,
                                     &start_line, &end_line, &start_byte,
                                     &end_byte, &buffer_size, &query_str,
                                     &batch_size, &memory_budget)) {
        return NULL;
    }

    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0 || batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError,
                        "range arguments must be >= 0; buffer_size and "
                        "batch_size must be > 0");
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

    auto state = std::make_shared<JsonDictIteratorState>();
    state->memory_budget_bytes = dftracer::utils::compute_memory_budget(
        static_cast<std::size_t>(memory_budget));

    Runtime *rt = get_runtime(self);
    std::size_t max_workers = rt->threads();
    auto bs = static_cast<std::size_t>(batch_size);
    std::size_t capacity = dftracer::utils::compute_channel_capacity(
        state->memory_budget_bytes, bs * ESTIMATED_BYTES_PER_JSON_EVENT,
        max_workers);
    state->channel =
        dftracer::utils::coro::make_channel<JsonDictBatch>(capacity);
    auto *sp = state.get();

    try {
        bool is_dir = fs::is_directory(cfg.file_path);
        if (is_dir) {
            auto handle = rt->scope(
                "iter_json_parallel",
                [sp, dir_path = cfg.file_path, index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc, bs,
                 max_workers](CoroScope &scope) -> CoroTask<void> {
                    co_await produce_json_dicts_parallel(
                        scope, sp, dir_path, index_dir, checkpoint_size,
                        auto_build_index, rc, bs, max_workers);
                });
            state->task_future = handle.future;
        } else {
            auto handle =
                rt->submit(produce_json_dicts(state, state->channel->producer(),
                                              cfg, rc, bs),
                           "iter_json");
            state->task_future = handle.future;
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_json_dict_iterator(std::move(state));
    return (PyObject *)it;
}

static PyObject *TraceReader_read_json_py(TraceReaderObject *self,
                                          PyObject *args, PyObject *kwds) {
    PyObject *iter = TraceReader_iter_json(self, args, kwds);
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

#ifdef DFTRACER_UTILS_ENABLE_ARROW

static PyObject *TraceReader_iter_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    static const char *kwlist[] = {
        "batch_size", "start_line",    "end_line", "start_byte",
        "end_byte",   "buffer_size",   "query",    "flatten_objects",
        "normalize",  "memory_budget", NULL};
    Py_ssize_t batch_size = 10000;
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;
    int flatten_objects = 1;  // default: expand top-level objects
    int normalize = 0;
    Py_ssize_t memory_budget = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnnzppn", (char **)kwlist, &batch_size,
            &start_line, &end_line, &start_byte, &end_byte, &buffer_size,
            &query_str, &flatten_objects, &normalize, &memory_budget)) {
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
    rc.flatten_objects = flatten_objects != 0;
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<ArrowIteratorState>();
    state->memory_budget_bytes = dftracer::utils::compute_memory_budget(
        static_cast<std::size_t>(memory_budget));

    Runtime *rt = get_runtime(self);
    std::size_t max_workers = rt->threads();
    auto bs = static_cast<std::size_t>(batch_size);
    std::size_t capacity = dftracer::utils::compute_channel_capacity(
        state->memory_budget_bytes, bs * ESTIMATED_BYTES_PER_ARROW_ROW,
        max_workers);
    state->channel =
        dftracer::utils::coro::make_channel<ArrowIteratorState::BatchType>(
            capacity);
    auto *sp = state.get();

    try {
        bool is_dir = fs::is_directory(cfg.file_path);
        if (is_dir) {
            auto handle = rt->scope(
                "iter_arrow_parallel",
                [sp, dir_path = cfg.file_path, index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc, bs,
                 norm = normalize != 0,
                 max_workers](CoroScope &scope) -> CoroTask<void> {
                    co_await produce_arrow_batches_parallel(
                        scope, sp, dir_path, index_dir, checkpoint_size,
                        auto_build_index, rc, bs, norm, max_workers);
                });
            state->task_future = handle.future;
        } else if (normalize) {
            auto handle = rt->submit(
                produce_arrow_batches(state, state->channel->producer(), cfg,
                                      rc, static_cast<std::size_t>(batch_size),
                                      flatten_objects != 0, normalize != 0),
                "iter_arrow");
            state->task_future = handle.future;
        } else {
            std::vector<std::string> files_vec{cfg.file_path};
            auto handle = rt->scope(
                "iter_arrow_parallel",
                [sp, files = std::move(files_vec), index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc, bs,
                 norm = normalize != 0,
                 max_workers](CoroScope &scope) mutable -> CoroTask<void> {
                    co_await produce_arrow_batches_for_files(
                        scope, sp, std::move(files), index_dir, checkpoint_size,
                        auto_build_index, rc, bs, norm, max_workers);
                });
            state->task_future = handle.future;
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return NULL;
    }

    TraceReaderIteratorObject *it = make_arrow_iterator(std::move(state));
    return (PyObject *)it;
}

// Build ArrowIteratorState + spawn the producer task. Same plumbing as
// TraceReader_iter_arrow but returns the state so callers can wrap it as
// either a per-batch iterator or an ArrowArrayStream.
static std::shared_ptr<ArrowIteratorState> spawn_arrow_producer(
    TraceReaderObject *self, PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {
        "batch_size", "start_line",    "end_line", "start_byte",
        "end_byte",   "buffer_size",   "query",    "flatten_objects",
        "normalize",  "memory_budget", NULL};
    Py_ssize_t batch_size = 10000;
    Py_ssize_t start_line = 0, end_line = 0;
    Py_ssize_t start_byte = 0, end_byte = 0;
    Py_ssize_t buffer_size = 4 * 1024 * 1024;
    const char *query_str = NULL;
    int flatten_objects = 1;  // default: expand top-level objects
    int normalize = 0;
    Py_ssize_t memory_budget = 0;

    if (!PyArg_ParseTupleAndKeywords(
            args, kwds, "|nnnnnnzppn", (char **)kwlist, &batch_size,
            &start_line, &end_line, &start_byte, &end_byte, &buffer_size,
            &query_str, &flatten_objects, &normalize, &memory_budget)) {
        return nullptr;
    }

    if (batch_size <= 0) {
        PyErr_SetString(PyExc_ValueError, "batch_size must be > 0");
        return nullptr;
    }
    if (start_line < 0 || end_line < 0 || start_byte < 0 || end_byte < 0 ||
        buffer_size <= 0) {
        PyErr_SetString(
            PyExc_ValueError,
            "range arguments must be >= 0; buffer_size must be > 0");
        return nullptr;
    }

    TraceReaderConfig cfg;
    try {
        cfg = build_config(self);
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }

    ReadConfig rc;
    rc.start_line = static_cast<std::size_t>(start_line);
    rc.end_line = static_cast<std::size_t>(end_line);
    rc.start_byte = static_cast<std::size_t>(start_byte);
    rc.end_byte = static_cast<std::size_t>(end_byte);
    rc.buffer_size = static_cast<std::size_t>(buffer_size);
    rc.flatten_objects = flatten_objects != 0;
    if (query_str) rc.query = query_str;

    auto state = std::make_shared<ArrowIteratorState>();
    state->memory_budget_bytes = dftracer::utils::compute_memory_budget(
        static_cast<std::size_t>(memory_budget));

    Runtime *rt = get_runtime(self);
    std::size_t max_workers = rt->threads();
    auto bs = static_cast<std::size_t>(batch_size);
    std::size_t capacity = dftracer::utils::compute_channel_capacity(
        state->memory_budget_bytes, bs * ESTIMATED_BYTES_PER_ARROW_ROW,
        max_workers);
    state->channel =
        dftracer::utils::coro::make_channel<ArrowIteratorState::BatchType>(
            capacity);
    auto *sp = state.get();

    try {
        bool is_dir = fs::is_directory(cfg.file_path);
        if (is_dir) {
            auto handle = rt->scope(
                "iter_arrow_parallel",
                [sp, dir_path = cfg.file_path, index_dir = cfg.index_dir,
                 checkpoint_size = cfg.checkpoint_size,
                 auto_build_index = cfg.auto_build_index, rc, bs,
                 norm = normalize != 0,
                 max_workers](CoroScope &scope) -> CoroTask<void> {
                    co_await produce_arrow_batches_parallel(
                        scope, sp, dir_path, index_dir, checkpoint_size,
                        auto_build_index, rc, bs, norm, max_workers);
                });
            state->task_future = handle.future;
        } else {
            auto handle = rt->submit(
                produce_arrow_batches(state, state->channel->producer(), cfg,
                                      rc, static_cast<std::size_t>(batch_size),
                                      flatten_objects != 0, normalize != 0),
                "iter_arrow");
            state->task_future = handle.future;
        }
    } catch (const std::exception &e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }

    return state;
}

static PyObject *TraceReader_iter_arrow_stream(TraceReaderObject *self,
                                               PyObject *args, PyObject *kwds) {
    auto state = spawn_arrow_producer(self, args, kwds);
    if (!state) return NULL;
    return make_arrow_batch_stream(std::move(state));
}

static PyObject *TraceReader_read_arrow(TraceReaderObject *self, PyObject *args,
                                        PyObject *kwds) {
    auto state = spawn_arrow_producer(self, args, kwds);
    if (!state) return NULL;
    PyObject *stream = make_arrow_batch_stream(std::move(state));
    if (!stream) return NULL;
    return dftracer::utils::python::wrap_arrow_stream_table(stream);
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW

#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC

static int parse_str_list_trace(PyObject *obj, std::vector<std::string> &out,
                                const char *param_name) {
    if (!obj || obj == Py_None) return 0;
    if (!PyList_Check(obj)) {
        PyErr_Format(PyExc_TypeError, "%s must be a list of str", param_name);
        return -1;
    }
    Py_ssize_t n = PyList_Size(obj);
    for (Py_ssize_t i = 0; i < n; i++) {
        const char *s = PyUnicode_AsUTF8(PyList_GetItem(obj, i));
        if (!s) return -1;
        out.emplace_back(s);
    }
    return 0;
}

static PyObject *TraceReader_write_arrow(TraceReaderObject *self,
                                         PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"path",        "views",      "chunk_size_mb",
                                   "compression", "batch_size", NULL};
    const char *path = NULL;
    PyObject *views_obj = Py_None;
    int chunk_size_mb = 32;
    const char *compression_str = "zstd";
    Py_ssize_t batch_size = 10000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "s|Oisn", (char **)kwlist,
                                     &path, &views_obj, &chunk_size_mb,
                                     &compression_str, &batch_size)) {
        return NULL;
    }

    if (chunk_size_mb < 0) {
        PyErr_SetString(PyExc_ValueError, "chunk_size_mb must be >= 0");
        return NULL;
    }

    std::vector<ViewDefinition> views;
    if (views_obj && views_obj != Py_None) {
        if (!PyList_Check(views_obj)) {
            PyErr_SetString(PyExc_TypeError, "views must be a list or None");
            return NULL;
        }
        Py_ssize_t n = PyList_Size(views_obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyList_GetItem(views_obj, i);
            ViewDefinition vd;

            if (PyUnicode_Check(item)) {
                const char *name = PyUnicode_AsUTF8(item);
                if (!name) return NULL;
                std::string name_str(name);
                if (name_str == "io") {
                    vd = ViewDefinition::io_view();
                } else if (name_str == "compute") {
                    vd = ViewDefinition::compute_view();
                } else if (name_str == "dlio") {
                    vd = ViewDefinition::dlio_view();
                } else {
                    vd.with_name(name_str);
                }
            } else if (PyDict_Check(item)) {
                PyObject *name_obj = PyDict_GetItemString(item, "name");
                if (!name_obj || !PyUnicode_Check(name_obj)) {
                    PyErr_SetString(PyExc_ValueError,
                                    "view dict must have 'name' string");
                    return NULL;
                }
                vd.with_name(PyUnicode_AsUTF8(name_obj));

                PyObject *query_obj = PyDict_GetItemString(item, "query");
                if (query_obj && query_obj != Py_None) {
                    if (!PyUnicode_Check(query_obj)) {
                        PyErr_SetString(PyExc_ValueError,
                                        "view 'query' must be a string");
                        return NULL;
                    }
                    vd.with_query(PyUnicode_AsUTF8(query_obj));
                }

                PyObject *meta_obj =
                    PyDict_GetItemString(item, "include_metadata");
                if (meta_obj && meta_obj != Py_None) {
                    vd.with_include_metadata(PyObject_IsTrue(meta_obj));
                }
            } else {
                PyErr_SetString(PyExc_TypeError,
                                "views list must contain strings or dicts");
                return NULL;
            }
            views.push_back(std::move(vd));
        }
    }

    IpcCompression compression = IpcCompression::ZSTD;
    if (compression_str) {
        std::string comp_lower(compression_str);
        for (auto &c : comp_lower) c = std::tolower(c);
        if (comp_lower == "none") {
            compression = IpcCompression::NONE;
        } else if (comp_lower == "zstd") {
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
            compression = IpcCompression::ZSTD;
#else
            PyErr_SetString(
                PyExc_ValueError,
                "ZSTD compression not available (built without ZSTD)");
            return NULL;
#endif
        } else {
            PyErr_Format(PyExc_ValueError,
                         "Unknown compression: %s (use 'none' or 'zstd')",
                         compression_str);
            return NULL;
        }
    }

    int64_t chunk_size_bytes =
        static_cast<int64_t>(chunk_size_mb) * 1024 * 1024;

    std::string file_path = PyUnicode_AsUTF8(self->file_path);
    std::string index_path;
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx && idx[0] != '\0') {
        index_path = idx;
    }
    std::size_t checkpoint_size = self->checkpoint_size;

    std::string output_path(path);
    WriteArrowResult result;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        result =
            rt->submit(write_arrow_pipeline(
                           file_path, index_path, checkpoint_size,
                           std::move(views), output_path, chunk_size_bytes,
                           compression, static_cast<std::size_t>(batch_size)),
                       "write_arrow")
                .get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    if (!result.error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, result.error.c_str());
        return NULL;
    }

    // Build result dict
    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    // Build files list per partition
    PyObject *partitions_dict = PyDict_New();
    if (!partitions_dict) {
        Py_DECREF(dict);
        return NULL;
    }

    for (const auto &[partition_name, partition_stats] :
         result.stats.partitions) {
        PyObject *partition_dict = PyDict_New();
        if (!partition_dict) {
            Py_DECREF(partitions_dict);
            Py_DECREF(dict);
            return NULL;
        }

        PyObject *files_list = PyList_New(0);
        if (!files_list) {
            Py_DECREF(partition_dict);
            Py_DECREF(partitions_dict);
            Py_DECREF(dict);
            return NULL;
        }

        for (const auto &f : partition_stats.files) {
            PyObject *file_str = PyUnicode_FromString(f.c_str());
            if (!file_str || PyList_Append(files_list, file_str) < 0) {
                Py_XDECREF(file_str);
                Py_DECREF(files_list);
                Py_DECREF(partition_dict);
                Py_DECREF(partitions_dict);
                Py_DECREF(dict);
                return NULL;
            }
            Py_DECREF(file_str);
        }

        PyDict_SetItemString(partition_dict, "files", files_list);
        dict_set_steal(partition_dict, "rows",
                       PyLong_FromLongLong(partition_stats.total_rows));
        dict_set_steal(
            partition_dict, "bytes",
            PyLong_FromLongLong(partition_stats.total_uncompressed_bytes));
        Py_DECREF(files_list);

        PyObject *key = partition_name.empty()
                            ? PyUnicode_FromString("_default")
                            : PyUnicode_FromString(partition_name.c_str());
        PyDict_SetItem(partitions_dict, key, partition_dict);
        Py_DECREF(key);
        Py_DECREF(partition_dict);
    }

    PyDict_SetItemString(dict, "partitions", partitions_dict);
    dict_set_steal(dict, "total_rows",
                   PyLong_FromLongLong(result.stats.total_rows));
    dict_set_steal(dict, "total_bytes",
                   PyLong_FromLongLong(result.stats.total_uncompressed_bytes));
    dict_set_steal(dict, "chunks_scanned",
                   PyLong_FromUnsignedLongLong(result.chunks_scanned));
    dict_set_steal(dict, "chunks_skipped",
                   PyLong_FromUnsignedLongLong(result.chunks_skipped));
    Py_DECREF(partitions_dict);

    return dict;
}

static PyObject *TraceReader_get_view_chunks(TraceReaderObject *self,
                                             PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"view", NULL};
    PyObject *view_obj = Py_None;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", (char **)kwlist,
                                     &view_obj)) {
        return NULL;
    }

    ViewDefinition view;
    if (view_obj && view_obj != Py_None) {
        if (PyUnicode_Check(view_obj)) {
            const char *name = PyUnicode_AsUTF8(view_obj);
            if (!name) return NULL;
            std::string name_str(name);
            if (name_str == "io") {
                view = ViewDefinition::io_view();
            } else if (name_str == "compute") {
                view = ViewDefinition::compute_view();
            } else if (name_str == "dlio") {
                view = ViewDefinition::dlio_view();
            } else {
                view.with_name(name_str);
            }
        } else if (PyDict_Check(view_obj)) {
            PyObject *name_obj = PyDict_GetItemString(view_obj, "name");
            if (name_obj && PyUnicode_Check(name_obj)) {
                view.with_name(PyUnicode_AsUTF8(name_obj));
            }
            PyObject *query_obj = PyDict_GetItemString(view_obj, "query");
            if (query_obj && query_obj != Py_None &&
                PyUnicode_Check(query_obj)) {
                view.with_query(PyUnicode_AsUTF8(query_obj));
            }
        } else {
            PyErr_SetString(PyExc_TypeError, "view must be a string or dict");
            return NULL;
        }
    }

    std::string file_path = PyUnicode_AsUTF8(self->file_path);
    std::string index_path;
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx && idx[0] != '\0') {
        index_path = idx;
    }
    std::size_t checkpoint_size = self->checkpoint_size;

    GetViewChunksResult result;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        result = rt->submit(get_view_chunks_pipeline(file_path, index_path,
                                                     checkpoint_size, view),
                            "get_view_chunks")
                     .get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    if (!result.error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, result.error.c_str());
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    PyObject *chunks_list = PyList_New(result.chunks.size());
    if (!chunks_list) {
        Py_DECREF(dict);
        return NULL;
    }

    for (std::size_t i = 0; i < result.chunks.size(); ++i) {
        const auto &chunk = result.chunks[i];
        PyObject *chunk_dict = PyDict_New();
        if (!chunk_dict) {
            Py_DECREF(chunks_list);
            Py_DECREF(dict);
            return NULL;
        }
        dict_set_steal(chunk_dict, "checkpoint_idx",
                       PyLong_FromUnsignedLongLong(chunk.checkpoint_idx));
        dict_set_steal(chunk_dict, "start_byte",
                       PyLong_FromSize_t(chunk.start_byte));
        dict_set_steal(chunk_dict, "end_byte",
                       PyLong_FromSize_t(chunk.end_byte));
        PyList_SetItem(chunks_list, i, chunk_dict);
    }

    PyDict_SetItemString(dict, "chunks", chunks_list);
    dict_set_steal(dict, "total_checkpoints",
                   PyLong_FromUnsignedLongLong(result.total_checkpoints));
    dict_set_steal(dict, "skipped_checkpoints",
                   PyLong_FromUnsignedLongLong(result.skipped_checkpoints));
    dict_set_steal(dict, "file_may_match",
                   PyBool_FromLong(result.file_may_match ? 1 : 0));
    Py_DECREF(chunks_list);

    return dict;
}

static PyObject *TraceReader_write_view_chunk(TraceReaderObject *self,
                                              PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {
        "output_file", "checkpoint_idx", "start_byte", "end_byte",
        "view",        "compression",    "batch_size", NULL};
    const char *output_file = NULL;
    unsigned long long checkpoint_idx = 0;
    Py_ssize_t start_byte = 0;
    Py_ssize_t end_byte = 0;
    PyObject *view_obj = Py_None;
    const char *compression_str = "zstd";
    Py_ssize_t batch_size = 10000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sKnn|Osn", (char **)kwlist,
                                     &output_file, &checkpoint_idx, &start_byte,
                                     &end_byte, &view_obj, &compression_str,
                                     &batch_size)) {
        return NULL;
    }

    IpcCompression compression = IpcCompression::ZSTD;
    if (compression_str) {
        std::string comp_lower(compression_str);
        for (auto &c : comp_lower) c = std::tolower(c);
        if (comp_lower == "none") {
            compression = IpcCompression::NONE;
        } else if (comp_lower == "zstd") {
#ifdef DFTRACER_UTILS_ENABLE_ZSTD
            compression = IpcCompression::ZSTD;
#else
            PyErr_SetString(PyExc_ValueError, "ZSTD compression not available");
            return NULL;
#endif
        }
    }

    ViewDefinition view;
    if (view_obj && view_obj != Py_None) {
        if (PyUnicode_Check(view_obj)) {
            const char *name = PyUnicode_AsUTF8(view_obj);
            if (!name) return NULL;
            std::string name_str(name);
            if (name_str == "io") {
                view = ViewDefinition::io_view();
            } else if (name_str == "compute") {
                view = ViewDefinition::compute_view();
            } else if (name_str == "dlio") {
                view = ViewDefinition::dlio_view();
            } else {
                view.with_name(name_str);
            }
        } else if (PyDict_Check(view_obj)) {
            PyObject *name_obj = PyDict_GetItemString(view_obj, "name");
            if (name_obj && PyUnicode_Check(name_obj)) {
                view.with_name(PyUnicode_AsUTF8(name_obj));
            }
            PyObject *query_obj = PyDict_GetItemString(view_obj, "query");
            if (query_obj && query_obj != Py_None &&
                PyUnicode_Check(query_obj)) {
                view.with_query(PyUnicode_AsUTF8(query_obj));
            }
        }
    }

    std::string file_path = PyUnicode_AsUTF8(self->file_path);
    std::string index_path;
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx && idx[0] != '\0') {
        index_path = idx;
    }
    std::size_t checkpoint_size = self->checkpoint_size;

    WriteViewChunkResult result;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        result =
            rt->submit(write_view_chunk_pipeline(
                           file_path, index_path, checkpoint_size, view,
                           checkpoint_idx, static_cast<std::size_t>(start_byte),
                           static_cast<std::size_t>(end_byte),
                           std::string(output_file), compression,
                           static_cast<std::size_t>(batch_size)),
                       "write_view_chunk")
                .get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    if (!result.error.empty()) {
        PyErr_SetString(PyExc_RuntimeError, result.error.c_str());
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    dict_set_steal(dict, "output_file",
                   PyUnicode_FromString(result.output_file.c_str()));
    dict_set_steal(dict, "events_matched",
                   PyLong_FromUnsignedLongLong(result.events_matched));
    dict_set_steal(dict, "events_scanned",
                   PyLong_FromUnsignedLongLong(result.events_scanned));
    dict_set_steal(dict, "rows_written",
                   PyLong_FromLongLong(result.rows_written));
    dict_set_steal(dict, "bytes_written",
                   PyLong_FromLongLong(result.bytes_written));

    return dict;
}

static PyObject *TraceReader_write_view_chunks(TraceReaderObject *self,
                                               PyObject *args, PyObject *kwds) {
    static const char *kwlist[] = {"chunks",      "output_dir", "view",
                                   "compression", "batch_size", NULL};
    PyObject *chunks_list = NULL;
    const char *output_dir = NULL;
    PyObject *view_obj = Py_None;
    const char *compression_str = "zstd";
    Py_ssize_t batch_size = 10000;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "Os|Osn", (char **)kwlist,
                                     &chunks_list, &output_dir, &view_obj,
                                     &compression_str, &batch_size)) {
        return NULL;
    }

    if (!PyList_Check(chunks_list)) {
        PyErr_SetString(PyExc_TypeError, "chunks must be a list");
        return NULL;
    }

    IpcCompression compression = IpcCompression::ZSTD;
    if (strcmp(compression_str, "none") == 0) {
        compression = IpcCompression::NONE;
    } else if (strcmp(compression_str, "zstd") != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "compression must be 'zstd' or 'none'");
        return NULL;
    }

    ViewDefinition view;
    if (view_obj && view_obj != Py_None) {
        if (PyUnicode_Check(view_obj)) {
            const char *name = PyUnicode_AsUTF8(view_obj);
            if (!name) return NULL;
            std::string name_str(name);
            if (name_str == "io") {
                view = ViewDefinition::io_view();
            } else if (name_str == "compute") {
                view = ViewDefinition::compute_view();
            } else if (name_str == "dlio") {
                view = ViewDefinition::dlio_view();
            } else {
                view.with_name(name_str);
            }
        } else if (PyDict_Check(view_obj)) {
            PyObject *name_obj = PyDict_GetItemString(view_obj, "name");
            if (name_obj && PyUnicode_Check(name_obj)) {
                view.with_name(PyUnicode_AsUTF8(name_obj));
            }
            PyObject *query_obj = PyDict_GetItemString(view_obj, "query");
            if (query_obj && query_obj != Py_None &&
                PyUnicode_Check(query_obj)) {
                view.with_query(PyUnicode_AsUTF8(query_obj));
            }
        }
    }

    std::vector<ChunkDescriptor> chunks;
    Py_ssize_t num_chunks = PyList_Size(chunks_list);
    chunks.reserve(static_cast<std::size_t>(num_chunks));

    for (Py_ssize_t i = 0; i < num_chunks; i++) {
        PyObject *chunk_dict = PyList_GetItem(chunks_list, i);
        if (!PyDict_Check(chunk_dict)) {
            PyErr_SetString(PyExc_TypeError, "each chunk must be a dict");
            return NULL;
        }

        ChunkDescriptor desc;

        PyObject *cp_idx = PyDict_GetItemString(chunk_dict, "checkpoint_idx");
        PyObject *start = PyDict_GetItemString(chunk_dict, "start_byte");
        PyObject *end = PyDict_GetItemString(chunk_dict, "end_byte");

        if (!cp_idx || !start || !end) {
            PyErr_SetString(
                PyExc_KeyError,
                "chunk must have checkpoint_idx, start_byte, end_byte");
            return NULL;
        }

        desc.checkpoint_idx =
            static_cast<std::uint64_t>(PyLong_AsUnsignedLongLong(cp_idx));
        desc.start_byte =
            static_cast<std::size_t>(PyLong_AsUnsignedLongLong(start));
        desc.end_byte =
            static_cast<std::size_t>(PyLong_AsUnsignedLongLong(end));

        char filename[64];
        snprintf(filename, sizeof(filename), "chunk-%05llu.arrow",
                 (unsigned long long)desc.checkpoint_idx);
        desc.output_file = std::string(output_dir) + "/" + filename;

        chunks.push_back(std::move(desc));
    }

    std::string file_path = PyUnicode_AsUTF8(self->file_path);
    std::string index_path;
    const char *idx = PyUnicode_AsUTF8(self->index_dir);
    if (idx && idx[0] != '\0') {
        index_path = idx;
    }
    std::size_t checkpoint_size = self->checkpoint_size;

    WriteViewChunksResult result;
    std::string error_msg;

    Py_BEGIN_ALLOW_THREADS try {
        Runtime *rt = get_runtime(self);
        result = rt->submit(write_view_chunks_pipeline(
                                file_path, index_path, checkpoint_size, view,
                                std::move(chunks), compression,
                                static_cast<std::size_t>(batch_size)),
                            "write_view_chunks")
                     .get();
    } catch (const std::exception &e) {
        error_msg = e.what();
    }
    Py_END_ALLOW_THREADS

        if (!error_msg.empty()) {
        PyErr_SetString(PyExc_RuntimeError, error_msg.c_str());
        return NULL;
    }

    PyObject *dict = PyDict_New();
    if (!dict) return NULL;

    PyObject *results_list =
        PyList_New(static_cast<Py_ssize_t>(result.results.size()));
    if (!results_list) {
        Py_DECREF(dict);
        return NULL;
    }

    for (std::size_t i = 0; i < result.results.size(); i++) {
        const auto &r = result.results[i];
        PyObject *item = PyDict_New();
        if (!item) {
            Py_DECREF(results_list);
            Py_DECREF(dict);
            return NULL;
        }
        dict_set_steal(item, "output_file",
                       PyUnicode_FromString(r.output_file.c_str()));
        dict_set_steal(item, "rows_written",
                       PyLong_FromLongLong(r.rows_written));
        dict_set_steal(item, "events_matched",
                       PyLong_FromUnsignedLongLong(r.events_matched));
        if (!r.error.empty()) {
            dict_set_steal(item, "error",
                           PyUnicode_FromString(r.error.c_str()));
        }
        PyList_SetItem(results_list, static_cast<Py_ssize_t>(i), item);
    }

    PyDict_SetItemString(dict, "results", results_list);
    Py_DECREF(results_list);
    dict_set_steal(dict, "total_rows", PyLong_FromLongLong(result.total_rows));
    dict_set_steal(dict, "total_events_matched",
                   PyLong_FromLongLong(result.total_events_matched));

    return dict;
}

#endif  // DFTRACER_UTILS_ENABLE_ARROW_IPC

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
    {"iter_json", (PyCFunction)TraceReader_iter_json,
     METH_VARARGS | METH_KEYWORDS,
     "Return an iterator over parsed JSON events as Python dicts.\n"
     "\n"
     "Each event is parsed once in C++ (single-pass simdjson ondemand)\n"
     "and yielded as a Python dict. No double-parsing overhead.\n"
     "\n"
     "Args:\n"
     "    start_line (int): First line (0 = beginning).\n"
     "    end_line (int): Last line (0 = end of file).\n"
     "    start_byte (int): First byte offset (0 = beginning).\n"
     "    end_byte (int): Last byte offset (0 = end of file).\n"
     "    buffer_size (int): Internal read buffer size in bytes.\n"
     "    query (str): Optional query filter.\n"
     "    batch_size (int): Events per internal batch (default 1024).\n"},
    {"read_json", (PyCFunction)TraceReader_read_json_py,
     METH_VARARGS | METH_KEYWORDS,
     "Read all events as parsed Python dicts (list).\n"
     "\n"
     "Equivalent to list(iter_json(...)).\n"},
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
    {"iter_arrow_stream", (PyCFunction)TraceReader_iter_arrow_stream,
     METH_VARARGS | METH_KEYWORDS,
     "Return an _ArrowBatchStream that exposes Arrow record batches\n"
     "via the Arrow C Data Interface stream protocol\n"
     "(__arrow_c_stream__). PyArrow can drain the producer channel\n"
     "with a single call, without per-batch Python iteration.\n"},
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
#ifdef DFTRACER_UTILS_ENABLE_ARROW_IPC
    {"write_arrow", (PyCFunction)TraceReader_write_arrow,
     METH_VARARGS | METH_KEYWORDS,
     "Write trace data to partitioned Arrow IPC files.\n"
     "\n"
     "Args:\n"
     "    path (str): Output directory path.\n"
     "    partition_by (list[str] or None): Column names to partition by.\n"
     "    num_buckets (int): Number of hash buckets (0 = no bucketing).\n"
     "    chunk_size_mb (int): Max uncompressed MB per file (default 32).\n"
     "    compression (str): 'zstd' or 'none' (default 'zstd').\n"
     "    batch_size (int): Rows per internal batch (default 10000).\n"
     "    normalize (bool): Use normalized schema (default False).\n"
     "\n"
     "Returns:\n"
     "    dict: Statistics including partitions, total_rows, total_bytes.\n"},
    {"get_view_chunks", (PyCFunction)TraceReader_get_view_chunks,
     METH_VARARGS | METH_KEYWORDS,
     "Get candidate chunks for a view after bloom filter pruning.\n"
     "\n"
     "Args:\n"
     "    view (str or dict): View name ('io', 'compute', 'dlio') or\n"
     "                        dict with 'name' and optional 'query'.\n"
     "\n"
     "Returns:\n"
     "    dict: chunks list, total_checkpoints, skipped_checkpoints.\n"},
    {"write_view_chunk", (PyCFunction)TraceReader_write_view_chunk,
     METH_VARARGS | METH_KEYWORDS,
     "Write a single chunk to an Arrow IPC file.\n"
     "\n"
     "Args:\n"
     "    output_file (str): Path to output Arrow IPC file.\n"
     "    checkpoint_idx (int): Checkpoint index.\n"
     "    start_byte (int): Start byte offset.\n"
     "    end_byte (int): End byte offset.\n"
     "    view (str or dict): View definition.\n"
     "    compression (str): 'zstd' or 'none' (default 'zstd').\n"
     "    batch_size (int): Events per batch (default 10000).\n"
     "\n"
     "Returns:\n"
     "    dict: output_file, events_matched, rows_written, bytes_written.\n"},
    {"write_view_chunks", (PyCFunction)TraceReader_write_view_chunks,
     METH_VARARGS | METH_KEYWORDS,
     "Write multiple chunks to Arrow IPC files in parallel.\n"
     "\n"
     "All chunks are processed concurrently on the Runtime thread pool.\n"
     "\n"
     "Args:\n"
     "    chunks (list): List of dicts with checkpoint_idx, start_byte, "
     "end_byte.\n"
     "    output_dir (str): Directory for output Arrow IPC files.\n"
     "    view (str or dict): View definition.\n"
     "    compression (str): 'zstd' or 'none' (default 'zstd').\n"
     "    batch_size (int): Events per batch (default 10000).\n"
     "\n"
     "Returns:\n"
     "    dict: results list, total_rows, total_events_matched.\n"},
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
     "Exit the runtime context for the with statement.\n"
     "\n"
     "TraceReader does not own the shared RocksDB instance for an index path;\n"
     "any shared DB lifetime remains manager-owned on the native side."},
    {NULL}};

static PyGetSetDef TraceReader_getsetters[] = {
    {"path", (getter)TraceReader_get_file_path, NULL,
     "Path to the trace file or directory", NULL},
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
    "            runtime: Runtime | None = None)\n"
    "--\n"
    "\n"
    "Smart trace file reader that auto-selects sequential or indexed\n"
    "reading based on whether a ``.dftindex`` store exists.\n"
    "\n"
    "Args:\n"
    "    file_path (str): Path to the trace file (.pfw.gz or plain "
    "text).\n"
    "    index_dir (str): Directory to search for ``.dftindex`` "
    "stores.\n"
    "        Empty string (default) searches next to the trace file.\n"
    "    checkpoint_size (int): Checkpoint interval in bytes for index\n"
    "        building (default 32 MB).\n"
    "    auto_build_index (bool): If True, automatically build an "
    "index\n"
    "        when none exists.\n"
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
