// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/abi_error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/coro/async_generator.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/task.h>
#include <dftracer/utils/coro/task_abi.h>
#include <dftracer/utils/dstd/byte_view.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/binary_file_reader_utility.h>
#include <dftracer/utils/fileio/file_compressor_utility.h>
#include <dftracer/utils/fileio/file_decompressor_utility.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/fileio/gzip_line_writer.h>
#include <dftracer/utils/fileio/lines/line_types.h>
#include <dftracer/utils/fileio/lines/sources/async_streaming_gz_line_generator.h>
#include <dftracer/utils/fileio/lines/sources/async_streaming_plain_line_generator.h>
#include <dftracer/utils/fileio/parallel/merge.h>
#include <dftracer/utils/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/plugins/host/fold_adapter.h>
#include <dftracer/utils/plugins/host/fold_adapter/ext.h>
#include <dftracer/utils/runtime/runtime.h>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace dftracer::utils::plugins::host {

namespace {

constexpr std::uint32_t DEFAULT_MAX_LINES = 1024;
constexpr std::size_t DEFAULT_CHUNK_SIZE = 64 * 1024;
constexpr std::uint8_t GZIP_MAGIC_0 = 0x1f;
constexpr std::uint8_t GZIP_MAGIC_1 = 0x8b;

using LineGenerator = coro::AsyncGenerator<dstd::Result<fileio::lines::Line>>;
using ChunkGenerator = coro::AsyncGenerator<dstd::Result<dstd::ByteView>>;

PluginFold& fold_of(void* h) { return *static_cast<PluginFold*>(h); }

template <class Make>
dftu_status start_task(::dftu_ctx* ctx, ::dftu_task** out, Make&& make) {
    if (out == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "the task slot is NULL");
    *out = coro::make_abi_task(std::forward<Make>(make));
    return DFTU_OK;
}

dstd::String op_message(const char* op, const dstd::Error& error) {
    return dstd::fmt<"{}: {}">(dstd::StringView(op), error.message());
}

dftu_status fail_error(::dftu_ctx* ctx, const char* op,
                       const dstd::Error& error) {
    return host_fail(ctx, common::to_status(error.condition()),
                     op_message(op, error));
}

void fail_task(coro::AbiTask* task, const char* op, const dstd::Error& error) {
    task->fail(common::to_status(error.condition()), op_message(op, error));
}

dftu_status invalid(::dftu_ctx* ctx, const char* op, const char* what) {
    return host_fail(
        ctx, DFTU_E_INVALID,
        dstd::fmt<"{}: {}">(dstd::StringView(op), dstd::StringView(what)));
}

coro::CoroTask<void> merge_shards_body(coro::AbiTask* task, dstd::String target,
                                       dstd::Vector<dstd::String> shards) {
    const int rc = co_await fileio::parallel::merge_shards(target, shards);
    if (rc != 0) {
        task->fail(DFTU_E_IO,
                   dstd::fmt<"merge_shards: could not merge into '{}'">(
                       dstd::StringView(target)));
    }
}

dftu_status host_merge_shards(::dftu_ctx* ctx, void*, const char* target,
                              const char* const* shards, std::uint32_t n,
                              ::dftu_task** out) {
    if (target == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "merge_shards: target is NULL");
    dstd::Vector<dstd::String> names;
    names.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (shards[i] == nullptr)
            return host_fail(ctx, DFTU_E_INVALID,
                             "merge_shards: a shard path is NULL");
        names.emplace_back(shards[i]);
    }
    return start_task(ctx, out,
                      [path = dstd::String(target),
                       names = std::move(names)](coro::AbiTask* task) mutable {
                          return merge_shards_body(task, std::move(path),
                                                   std::move(names));
                      });
}

// Host-owned parallel writer handed to the plugin as an opaque dftu_writer*;
// worker_count and gzip are captured at writer_create for a later writer_open.
struct WriterHandle : PluginHandle {
    dstd::UniquePtr<fileio::parallel::ParallelWriter> writer;
    dstd::String path;
    std::size_t worker_count = 1;
    bool gzip = false;
};

dftu_status host_writer_create(::dftu_ctx* ctx, void* h, const char* path,
                               std::uint32_t worker_count, std::int32_t gzip,
                               ::dftu_writer** out) {
    if (path == nullptr || out == nullptr) {
        return host_fail(ctx, DFTU_E_INVALID,
                         "writer_create: path and out must be set");
    }
    auto handle = dstd::make_unique<WriterHandle>();
    handle->writer = fileio::parallel::make_sharded_writer();
    handle->path = path;
    handle->worker_count = worker_count ? worker_count : 1;
    handle->gzip = gzip != 0;
    *out = reinterpret_cast<::dftu_writer*>(
        fold_of(h).adopt_handle(std::move(handle)));
    return DFTU_OK;
}

// Runs one writer call; a nonzero result fails the task.
template <class Call>
coro::CoroTask<void> writer_body(coro::AbiTask* task, const char* op,
                                 WriterHandle* writer, Call call) {
    const int rc = co_await call(writer);
    if (rc != 0) {
        task->fail(DFTU_E_IO,
                   dstd::fmt<"{}: could not write '{}'">(
                       dstd::StringView(op), dstd::StringView(writer->path)));
    }
}

template <class Call>
dftu_status writer_start(::dftu_ctx* ctx, void* h, ::dftu_writer* w,
                         ::dftu_task** out, const char* op, Call call) {
    WriterHandle* writer = fold_of(h).find_handle<WriterHandle>(w);
    if (writer == nullptr) return invalid(ctx, op, "the writer is not valid");
    return start_task(ctx, out, [=](coro::AbiTask* task) {
        return writer_body(task, op, writer, call);
    });
}

dftu_status host_writer_open(::dftu_ctx* ctx, void* h, ::dftu_writer* w,
                             ::dftu_task** out) {
    return writer_start(ctx, h, w, out, "writer_open", [](WriterHandle* pw) {
        return pw->writer->open(pw->path, pw->worker_count, pw->gzip, nullptr);
    });
}

dftu_status host_writer_chunk(::dftu_ctx* ctx, void* h, ::dftu_writer* w,
                              std::uint32_t worker, const void* data,
                              std::uint64_t len, ::dftu_task** out) {
    return writer_start(ctx, h, w, out, "writer_chunk", [=](WriterHandle* pw) {
        return pw->writer->write_chunk(
            static_cast<std::size_t>(worker),
            dstd::ByteView(static_cast<const unsigned char*>(data),
                           static_cast<std::size_t>(len)));
    });
}

// A close ends the handle whether or not the close succeeded.
coro::CoroTask<void> writer_close_body(coro::AbiTask* task, PluginFold* fold,
                                       WriterHandle* writer) {
    const int rc = co_await writer->writer->close();
    if (rc != 0) {
        task->fail(DFTU_E_IO, dstd::fmt<"writer_close: could not write '{}'">(
                                  dstd::StringView(writer->path)));
    }
    fold->release_handle(writer);
}

dftu_status host_writer_close(::dftu_ctx* ctx, void* h, ::dftu_writer* w,
                              ::dftu_task** out) {
    PluginFold* fold = &fold_of(h);
    WriterHandle* writer = fold->find_handle<WriterHandle>(w);
    if (writer == nullptr)
        return invalid(ctx, "writer_close", "the writer is not valid");
    return start_task(ctx, out, [=](coro::AbiTask* task) {
        return writer_close_body(task, fold, writer);
    });
}

struct LineReaderHandle : PluginHandle {
    explicit LineReaderHandle(LineGenerator g) : gen(std::move(g)) {}
    LineGenerator gen;
    // A line from the generator is valid only until the next read, so a batch
    // copies its lines side by side into `bytes`.
    dstd::String bytes;
    dstd::Vector<dftu_bytes> views;
    bool ended = false;
};

struct ChunkReaderHandle : PluginHandle {
    explicit ChunkReaderHandle(ChunkGenerator g) : gen(std::move(g)) {}
    ChunkGenerator gen;
    bool ended = false;
};

struct GzHandle : PluginHandle {
    fileio::GzipLineWriter writer;
    bool is_open = false;
};

dstd::Result<bool> has_gzip_magic(const char* path) {
    auto file = dstd::File::open(dstd::PathView(path), {});
    if (!file) return file.error();
    std::uint8_t magic[2] = {0, 0};
    auto read = file->read_at(0, dstd::Span<std::uint8_t>(magic, 2));
    if (!read) return read.error();
    return *read == 2 && magic[0] == GZIP_MAGIC_0 && magic[1] == GZIP_MAGIC_1;
}

dftu_status host_lines_open(::dftu_ctx* ctx, void* h, const char* path,
                            std::uint64_t start_line, std::uint64_t end_line,
                            ::dftu_line_reader** out) {
    if (path == nullptr || out == nullptr)
        return invalid(ctx, "lines_open", "path and out must be set");
    if (end_line != 0 && end_line < start_line)
        return invalid(ctx, "lines_open", "end_line is before start_line");
    const dstd::Result<bool> gzip = has_gzip_magic(path);
    if (!gzip.has_value()) return fail_error(ctx, "lines_open", gzip.error());
    const auto start = static_cast<std::size_t>(start_line);
    const auto end = static_cast<std::size_t>(end_line);
    LineGenerator gen =
        *gzip ? fileio::lines::sources::async_streaming_gz_lines(
                    dstd::String(path), start, end)
              : fileio::lines::sources::async_streaming_plain_lines(
                    dstd::String(path), start, end);
    *out = reinterpret_cast<::dftu_line_reader*>(fold_of(h).adopt_handle(
        dstd::make_unique<LineReaderHandle>(std::move(gen))));
    return DFTU_OK;
}

coro::CoroTask<void> lines_next_body(coro::AbiTask* task, LineReaderHandle* r,
                                     std::uint32_t max_lines,
                                     dftu_line_batch* out) {
    r->bytes.clear();
    r->views.clear();
    dstd::Vector<std::size_t> sizes;
    std::uint64_t first_line = 0;
    while (sizes.size() < max_lines && !r->ended) {
        auto next = co_await r->gen.next();
        if (!next) {
            r->ended = true;
            break;
        }
        if (!*next) {
            r->ended = true;
            fail_task(task, "lines_next", next->error());
            co_return;
        }
        const fileio::lines::Line& line = **next;
        if (sizes.empty()) first_line = line.line_number;
        r->bytes.append(line.content);
        sizes.push_back(line.content.size());
    }
    out->lines = nullptr;
    out->first_line = first_line;
    out->count = static_cast<std::uint32_t>(sizes.size());
    if (sizes.empty()) {
        task->status = DFTU_END;
        co_return;
    }
    std::size_t offset = 0;
    r->views.reserve(sizes.size());
    for (const std::size_t size : sizes) {
        r->views.push_back(
            dftu_bytes{r->bytes.data() + offset, size, nullptr, nullptr});
        offset += size;
    }
    out->lines = r->views.data();
}

dftu_status host_lines_next(::dftu_ctx* ctx, void* h, ::dftu_line_reader* r,
                            std::uint32_t max_lines, dftu_line_batch* out,
                            ::dftu_task** task) {
    LineReaderHandle* reader = fold_of(h).find_handle<LineReaderHandle>(r);
    if (reader == nullptr)
        return invalid(ctx, "lines_next", "the reader is not valid");
    if (out == nullptr)
        return invalid(ctx, "lines_next", "the batch slot is NULL");
    const std::uint32_t max = max_lines != 0 ? max_lines : DEFAULT_MAX_LINES;
    return start_task(ctx, task, [=](coro::AbiTask* t) {
        return lines_next_body(t, reader, max, out);
    });
}

void host_lines_close(void* h, ::dftu_line_reader* r) {
    fold_of(h).release_handle(r);
}

dftu_status host_chunks_open(::dftu_ctx* ctx, void* h, const char* path,
                             std::uint64_t chunk_size,
                             ::dftu_chunk_reader** out) {
    if (path == nullptr || out == nullptr)
        return invalid(ctx, "chunks_open", "path and out must be set");
    const std::size_t size = chunk_size != 0
                                 ? static_cast<std::size_t>(chunk_size)
                                 : DEFAULT_CHUNK_SIZE;
    ChunkGenerator gen = fileio::read_binary_file(dstd::Path(path), size);
    *out = reinterpret_cast<::dftu_chunk_reader*>(fold_of(h).adopt_handle(
        dstd::make_unique<ChunkReaderHandle>(std::move(gen))));
    return DFTU_OK;
}

coro::CoroTask<void> chunks_next_body(coro::AbiTask* task, ChunkReaderHandle* r,
                                      dftu_bytes* out) {
    *out = dftu_bytes{nullptr, 0, nullptr, nullptr};
    if (r->ended) {
        task->status = DFTU_END;
        co_return;
    }
    auto next = co_await r->gen.next();
    if (!next) {
        r->ended = true;
        task->status = DFTU_END;
        co_return;
    }
    if (!*next) {
        r->ended = true;
        fail_task(task, "chunks_next", next->error());
        co_return;
    }
    out->data = (*next)->data();
    out->len = (*next)->size();
}

dftu_status host_chunks_next(::dftu_ctx* ctx, void* h, ::dftu_chunk_reader* r,
                             dftu_bytes* out, ::dftu_task** task) {
    ChunkReaderHandle* reader = fold_of(h).find_handle<ChunkReaderHandle>(r);
    if (reader == nullptr)
        return invalid(ctx, "chunks_next", "the reader is not valid");
    if (out == nullptr)
        return invalid(ctx, "chunks_next", "the chunk slot is NULL");
    return start_task(ctx, task, [=](coro::AbiTask* t) {
        return chunks_next_body(t, reader, out);
    });
}

void host_chunks_close(void* h, ::dftu_chunk_reader* r) {
    fold_of(h).release_handle(r);
}

coro::CoroTask<void> gz_open_body(coro::AbiTask* task, GzHandle* g,
                                  dstd::String path,
                                  fileio::GzipWriterOptions options) {
    auto opened = co_await fileio::GzipLineWriter::open(std::move(path),
                                                        std::move(options));
    if (!opened) {
        fail_task(task, "gz_open", opened.error());
        co_return;
    }
    g->writer = std::move(*opened);
    g->is_open = true;
}

dftu_status host_gz_open(::dftu_ctx* ctx, void* h, const char* path,
                         const dftu_gz_options* opts, ::dftu_gz_writer** out,
                         ::dftu_task** task) {
    if (path == nullptr || out == nullptr)
        return invalid(ctx, "gz_open", "path and out must be set");
    fileio::GzipWriterOptions options;
    if (opts != nullptr) {
        dftu_gz_options given{sizeof(dftu_gz_options), 0, 0, 0};
        if (!common::copy_struct(&given, sizeof(given), opts, opts->struct_size,
                                 offsetof(dftu_gz_options, level))) {
            return invalid(
                ctx, "gz_open",
                "options struct_size does not reach the first field");
        }
        if (given.level != 0) options.level = given.level;
        if (given.member_size != 0)
            options.member_size = static_cast<std::size_t>(given.member_size);
        if (given.workers != 0) options.workers = given.workers;
    }
    if (task == nullptr)
        return host_fail(ctx, DFTU_E_INVALID, "the task slot is NULL");
    auto handle = dstd::make_unique<GzHandle>();
    GzHandle* g = handle.get();
    *out = reinterpret_cast<::dftu_gz_writer*>(
        fold_of(h).adopt_handle(std::move(handle)));
    return start_task(ctx, task,
                      [g, p = dstd::String(path),
                       options = std::move(options)](coro::AbiTask* t) mutable {
                          return gz_open_body(t, g, std::move(p),
                                              std::move(options));
                      });
}

dftu_status host_gz_append(::dftu_ctx* ctx, void* h, ::dftu_gz_writer* w,
                           const void* data, std::uint64_t len) {
    GzHandle* g = fold_of(h).find_handle<GzHandle>(w);
    if (g == nullptr || !g->is_open)
        return invalid(ctx, "gz_append", "the writer is not open");
    if (data == nullptr && len != 0)
        return invalid(ctx, "gz_append", "data is NULL");
    const dstd::StringView lines(static_cast<const char*>(data),
                                 static_cast<std::size_t>(len));
    const auto ran = default_runtime().block_on(coro::named(
        "dft-plugin-gz-append", [&]() -> coro::CoroTask<dstd::Result<void>> {
            co_return co_await g->writer.append(lines);
        }()));
    if (!ran) return fail_error(ctx, "gz_append", ran.error());
    return DFTU_OK;
}

// A close ends the handle whether or not the close succeeded.
coro::CoroTask<void> gz_close_body(coro::AbiTask* task, PluginFold* fold,
                                   GzHandle* g, dftu_gz_summary* out) {
    g->is_open = false;
    auto summary = co_await g->writer.close();
    if (summary) {
        *out = dftu_gz_summary{summary->parts, summary->members, summary->lines,
                               summary->uc_bytes, summary->c_bytes};
    } else {
        fail_task(task, "gz_close", summary.error());
    }
    fold->release_handle(g);
}

dftu_status host_gz_close(::dftu_ctx* ctx, void* h, ::dftu_gz_writer* w,
                          dftu_gz_summary* out, ::dftu_task** task) {
    PluginFold* fold = &fold_of(h);
    GzHandle* g = fold->find_handle<GzHandle>(w);
    if (g == nullptr || !g->is_open)
        return invalid(ctx, "gz_close", "the writer is not open");
    if (out == nullptr)
        return invalid(ctx, "gz_close", "the summary slot is NULL");
    return start_task(ctx, task, [=](coro::AbiTask* t) {
        return gz_close_body(t, fold, g, out);
    });
}

coro::CoroTask<void> compress_body(coro::AbiTask* task, dstd::String in,
                                   dstd::String out_path, std::int32_t level,
                                   std::uint64_t member_size,
                                   std::uint64_t* out_size) {
    auto input = fileio::FileCompressionUtilityInput::from_file(in);
    input.with_output(out_path);
    if (level != 0) input.with_compression_level(level);
    if (member_size != 0)
        input.with_member_size(static_cast<std::size_t>(member_size));
    const fileio::FileCompressorUtility compressor;
    auto done = co_await compressor(input);
    if (!done) {
        fail_task(task, "compress", done.error());
        co_return;
    }
    if (out_size != nullptr)
        *out_size = static_cast<std::uint64_t>(done->compressed_size);
}

dftu_status host_compress(::dftu_ctx* ctx, void*, const char* in_path,
                          const char* out_path, std::int32_t level,
                          std::uint64_t member_size, std::uint64_t* out_size,
                          ::dftu_task** task) {
    if (in_path == nullptr || out_path == nullptr)
        return invalid(ctx, "compress", "the paths must be set");
    return start_task(ctx, task,
                      [=, in = dstd::String(in_path),
                       out = dstd::String(out_path)](coro::AbiTask* t) mutable {
                          return compress_body(t, std::move(in), std::move(out),
                                               level, member_size, out_size);
                      });
}

coro::CoroTask<void> decompress_body(coro::AbiTask* task, dstd::String in,
                                     dstd::String out_path,
                                     std::uint64_t* out_size) {
    auto input = fileio::FileDecompressionUtilityInput::from_file(in);
    input.with_output(out_path);
    const fileio::FileDecompressorUtility decompressor;
    auto done = co_await decompressor(input);
    if (!done) {
        fail_task(task, "decompress", done.error());
        co_return;
    }
    if (out_size != nullptr)
        *out_size = static_cast<std::uint64_t>(done->decompressed_size);
}

dftu_status host_decompress(::dftu_ctx* ctx, void*, const char* in_path,
                            const char* out_path, std::uint64_t* out_size,
                            ::dftu_task** task) {
    if (in_path == nullptr || out_path == nullptr)
        return invalid(ctx, "decompress", "the paths must be set");
    return start_task(ctx, task,
                      [=, in = dstd::String(in_path),
                       out = dstd::String(out_path)](coro::AbiTask* t) mutable {
                          return decompress_body(t, std::move(in),
                                                 std::move(out), out_size);
                      });
}

// The text of logical_path, kept until the plugin frees its dftu_bytes.
struct OwnedText : dstd::HeapObject {
    dstd::String text;
};

void free_owned_text(void*, void* ud) {
    dstd::UniquePtr<OwnedText>(static_cast<OwnedText*>(ud)).reset();
}

// The file helpers share one rule: a path that cannot be stat'ed is not found.
dftu_status stat_for(::dftu_ctx* ctx, const char* op, const char* path,
                     common::InputFileStat* st) {
    if (path == nullptr) return invalid(ctx, op, "path is NULL");
    if (!common::stat_input(dstd::CStringView(path), *st)) {
        return host_fail(ctx, DFTU_E_NOT_FOUND,
                         dstd::fmt<"{}: cannot stat '{}'">(
                             dstd::StringView(op), dstd::StringView(path)));
    }
    return DFTU_OK;
}

dftu_status host_logical_path(::dftu_ctx* ctx, void*, const char* path,
                              dftu_bytes* out) {
    if (out == nullptr) return invalid(ctx, "logical_path", "out is NULL");
    common::InputFileStat st;
    const dftu_status found = stat_for(ctx, "logical_path", path, &st);
    if (found < 0) return found;
    auto owned = dstd::make_unique<OwnedText>();
    owned->text = fileio::get_logical_path(path);
    const dstd::StringView text = owned->text;
    *out =
        dftu_bytes{text.data(), text.size(), &free_owned_text, owned.release()};
    return DFTU_OK;
}

dftu_status host_mtime(::dftu_ctx* ctx, void*, const char* path,
                       std::int64_t* out_seconds) {
    if (out_seconds == nullptr) return invalid(ctx, "mtime", "out is NULL");
    common::InputFileStat st;
    const dftu_status found = stat_for(ctx, "mtime", path, &st);
    if (found < 0) return found;
    *out_seconds = static_cast<std::int64_t>(
        fileio::get_file_modification_time(dstd::CStringView(path)));
    return DFTU_OK;
}

dftu_status host_size(::dftu_ctx* ctx, void*, const char* path,
                      std::uint64_t* out) {
    if (out == nullptr) return invalid(ctx, "size", "out is NULL");
    common::InputFileStat st;
    const dftu_status found = stat_for(ctx, "size", path, &st);
    if (found < 0) return found;
    *out = fileio::file_size_bytes(dstd::CStringView(path));
    return DFTU_OK;
}

dftu_status host_sampled_hash(::dftu_ctx* ctx, void*, const char* path,
                              std::uint64_t* out) {
    if (out == nullptr) return invalid(ctx, "sampled_hash", "out is NULL");
    common::InputFileStat st;
    const dftu_status found = stat_for(ctx, "sampled_hash", path, &st);
    if (found < 0) return found;
    *out = fileio::calculate_file_hash(dstd::CStringView(path));
    return DFTU_OK;
}

const dftu_svc_fileio g_fileio = {sizeof(dftu_svc_fileio),
                                  host_merge_shards,
                                  host_writer_create,
                                  host_writer_open,
                                  host_writer_chunk,
                                  host_writer_close,
                                  host_lines_open,
                                  host_lines_next,
                                  host_lines_close,
                                  host_chunks_open,
                                  host_chunks_next,
                                  host_chunks_close,
                                  host_gz_open,
                                  host_gz_append,
                                  host_gz_close,
                                  host_compress,
                                  host_decompress,
                                  host_logical_path,
                                  host_mtime,
                                  host_size,
                                  host_sampled_hash};

}  // namespace

const void* detail::fileio_ext_vtable() { return &g_fileio; }

}  // namespace dftracer::utils::plugins::host
