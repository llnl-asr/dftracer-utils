// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/coro/async_event.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/channel.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string_intern.h>
#include <dftracer/utils/fileio/file_helpers.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/trace/index/build/file_admission.h>
#include <dftracer/utils/trace/index/build/file_group.h>
#include <dftracer/utils/trace/index/build/index_batch_writer.h>
#include <dftracer/utils/trace/index/build/index_fold_driver.h>
#include <dftracer/utils/trace/index/extensions/bloom_fold.h>
#include <dftracer/utils/trace/index/extensions/extension_registry.h>
#include <dftracer/utils/trace/index/extensions/rowset_fold.h>
#include <dftracer/utils/trace/index/index.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/schemas/dft/agg/agg_store.h>
#include <dftracer/utils/trace/index/source.h>
#include <dftracer/utils/trace/index/tables.h>
#include <dftracer/utils/trace/index/write_feed.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>

namespace dftracer::utils::trace::index {

namespace {

using common::ErrorCode;

constexpr dstd::StringView DFTRACER_SCHEMA = "dftracer";

std::uint64_t count_lines(dstd::StringView text) {
    std::uint64_t lines = 0;
    const char* at = text.data();
    const char* end = at + text.size();
    while (at < end) {
        const void* nl =
            std::memchr(at, '\n', static_cast<std::size_t>(end - at));
        if (nl == nullptr) break;
        at = static_cast<const char*>(nl) + 1;
        ++lines;
    }
    return lines;
}

}  // namespace

// What the writer hands to a file's stage: text of the member being fed, the
// end of a member, or the end of the file. Text holds a share of the stage's
// byte budget until the stage has copied it out.
struct StageMessage {
    enum class Kind : std::uint8_t { Lines, EndMember, Finish };
    Kind kind = Kind::Lines;
    dstd::String text;
    dstd::Optional<coro::AsyncSemaphore::Permit> held;
};

using StageChannel = coro::UnboundedChannel<dstd::UniquePtr<StageMessage>>;
using StageProducer = decltype(std::declval<StageChannel&>().producer());

// One file's index state. The writer thread feeds it through a channel and a
// task of the writer's scope, which owns the driver while the file is fed, so
// the parse and the fold steps run beside the compression that follows.
class WriteFeed::File : public std::enable_shared_from_this<WriteFeed::File> {
   public:
    dstd::String final_path;
    dstd::String logical_path;
    dstd::String index_root;
    dstd::UniquePtr<dftracer::utils::dstd::StringIntern> intern;
    dstd::UniquePtr<extensions::BloomFold> bloom;
    dstd::UniquePtr<extensions::RowSetFold> rowset;
    dstd::UniquePtr<extensions::ExtensionBuilders> builders;
    dstd::Optional<build::IndexFoldDriver> driver;
    dstd::Vector<std::uint64_t> fed_bytes;
    dstd::Vector<std::uint64_t> fed_lines;
    std::uint64_t member_bytes = 0;
    std::uint64_t member_lines = 0;
    bool finished = false;
    bool is_table = false;
    gzip::GzipBuildArtifacts artifacts;

    dstd::SharedPtr<coro::AsyncSemaphore> budget;
    dstd::SharedPtr<StageChannel> channel;
    dstd::Optional<StageProducer> producer;
    coro::AsyncEvent done;
    std::uint64_t drain_every = 0;
    std::atomic<bool> failed{false};
    dstd::Optional<dstd::Error> error;
};

struct WriteFeed::Impl {
    IndexerOptions options;
    build::ChunkIndexerConfig config;
    const RecordSchema* schema = nullptr;
    build::FoldBufferPool buffers;
    build::FileAdmission admission;
    dstd::Vector<dstd::SharedPtr<File>> files;
    // The text a stage may hold, shared by every file, and the text fed since
    // the last drain at which a stage waits for its parse tasks, so the text
    // in flight stays a share of the budget.
    dstd::SharedPtr<coro::AsyncSemaphore> budget;
    std::uint64_t drain_every = 0;

    Impl(IndexerOptions o, build::ChunkIndexerConfig c, const RecordSchema* s)
        : options(std::move(o)),
          config(std::move(c)),
          schema(s),
          admission(memory::resolve_spill_budget(options.memory_budget),
                    std::max<std::size_t>(default_runtime().threads(), 1), 0,
                    1) {
        drain_every = std::max<std::uint64_t>(
            memory::resolve_spill_budget(options.memory_budget) / 32,
            std::uint64_t{1} << 20);
        budget = dstd::make_shared<coro::AsyncSemaphore>(drain_every);
    }

    // A stage that is still waiting for text ends when its channel closes,
    // so a write that fails leaves no task of the scope parked.
    ~Impl() {
        for (const auto& f : files) f->producer.reset();
    }
};

WriteFeed::WriteFeed(dstd::UniquePtr<Impl> impl) : impl_(std::move(impl)) {}
WriteFeed::WriteFeed(WriteFeed&&) noexcept = default;
WriteFeed& WriteFeed::operator=(WriteFeed&&) noexcept = default;
WriteFeed::~WriteFeed() = default;

coro::CoroTask<void> WriteFeed::abandon() {
    for (const auto& f : impl_->files) {
        if (!f->channel) continue;
        f->producer.reset();
        auto waiting = f->done.wait();
        co_await waiting;
        f->done.reset();
        f->channel.reset();
    }
}

dstd::Result<WriteFeed> WriteFeed::open(IndexerOptions options) {
    auto config = index_chunk_config(options);
    if (!config) return config.error();
    auto schema =
        get_schema(options.schema.empty() ? DFTRACER_SCHEMA
                                          : dstd::StringView(options.schema));
    if (!schema) return schema.error();
    return WriteFeed(dstd::make_unique<Impl>(std::move(options),
                                             std::move(*config), *schema));
}

dstd::Result<WriteFeed::File*> WriteFeed::add_file(
    dstd::StringView final_path) {
    Impl& s = *impl_;
    auto file = dstd::make_shared<File>();
    file->budget = s.budget;
    file->drain_every = s.drain_every;
    file->final_path = dstd::String(final_path);
    file->logical_path = fileio::get_logical_path(file->final_path);
    file->index_root = trace::internal::determine_index_path(
        file->final_path, s.options.index_dir);
    file->intern = dstd::make_unique<dftracer::utils::dstd::StringIntern>();
    dstd::Vector<trace::views::detail::Fold*> folds;
    if (s.options.bloom) {
        file->bloom = dstd::make_unique<extensions::BloomFold>(
            *file->intern, build::for_schema(s.config, *s.schema));
        folds.push_back(file->bloom.get());
    }
    file->rowset = dstd::make_unique<extensions::RowSetFold>(
        *file->intern, indexed_rowsets(*s.schema));
    folds.push_back(file->rowset.get());
    file->driver.emplace(*file->intern, folds, file->final_path,
                         file->index_root);
    file->driver->set_record_schema(*s.schema);
    file->driver->share_buffers(s.buffers);
    file->driver->set_parse_gate(s.admission.gate());
    if (s.options.bloom) {
        auto registered = extensions::registered_extensions();
        if (!registered.empty()) {
            file->builders = dstd::make_unique<extensions::ExtensionBuilders>(
                std::move(registered));
            file->driver->set_plugin_builders(file->builders.get());
        }
    }
    s.admission.gate().file_in();
    s.files.push_back(std::move(file));
    return s.files.back().get();
}

namespace {

// The stage of one file: steps the driver with what the writer sends, in the
// order it sent it, until the file is finished or the channel closes. After
// an error it keeps receiving, so the writer never waits on a stage that
// stopped, and the budget of the text it drops goes back.
//
// The frame owns a StageEnd, which sets `done` when the frame dies, so a stage
// that never ran (the scope or the executor was shutting down) does not leave
// a drain or an abandon waiting.
struct StageEnd {
    dstd::SharedPtr<WriteFeed::File> file;
    explicit StageEnd(dstd::SharedPtr<WriteFeed::File> f)
        : file(std::move(f)) {}
    StageEnd(StageEnd&&) noexcept = default;
    StageEnd& operator=(StageEnd&&) = delete;
    ~StageEnd() {
        if (file) file->done.set();
    }
};

coro::CoroTask<void> run_stage(StageEnd end) {
    const dstd::SharedPtr<WriteFeed::File>& file = end.file;
    auto consumer = file->channel->consumer();
    std::uint64_t undrained = 0;
    bool finished = false;
    for (;;) {
        auto received = co_await consumer.receive();
        if (!received) break;
        StageMessage& m = **received;
        if (file->failed.load(std::memory_order_relaxed)) continue;
        dstd::Result<void> step{};
        if (m.kind == StageMessage::Kind::Lines) {
            file->member_bytes += m.text.size();
            file->member_lines += count_lines(m.text);
            auto feeding =
                file->driver->on_chunk(m.text, file->fed_bytes.size());
            step = co_await std::move(feeding);
            undrained += m.text.size();
            if (step && undrained >= file->drain_every) {
                undrained = 0;
                auto flushing = file->driver->flush();
                step = co_await std::move(flushing);
            }
        } else if (m.kind == StageMessage::Kind::EndMember) {
            // The driver tags each piece with its member, so the members of a
            // file parse at once; only the end of the file waits for them.
            auto closing = file->driver->on_checkpoint(file->fed_bytes.size());
            step = co_await std::move(closing);
            file->fed_bytes.push_back(file->member_bytes);
            file->fed_lines.push_back(file->member_lines);
            file->member_bytes = 0;
            file->member_lines = 0;
        } else {
            auto flushing = file->driver->flush();
            step = co_await std::move(flushing);
        }
        if (!step) {
            file->error = step.error();
            file->failed.store(true, std::memory_order_release);
        }
        if (m.kind == StageMessage::Kind::Finish) {
            finished = true;
            break;
        }
    }
    // A stage that was not finished waits for the parse tasks it started,
    // which read the feed's pool and gate.
    if (!finished) {
        auto draining = file->driver->flush();
        (void)co_await std::move(draining);
    }
    file->done.set();
}

}  // namespace

namespace {

// Starts the stage of `file` in `scope` when it has none: the first call, or
// the first after a drain.
void ensure_stage(WriteFeed::File& file, coro::Scope& scope) {
    if (file.producer) return;
    file.channel =
        coro::make_unbounded_channel<dstd::UniquePtr<StageMessage>>();
    file.producer.emplace(file.channel->producer());
    file.driver->bind_scope(&scope);
    scope.spawn(run_stage(StageEnd(file.shared_from_this())));
}

}  // namespace

coro::CoroTask<dstd::Result<void>> WriteFeed::lines(File& file,
                                                    dstd::StringView text,
                                                    coro::Scope& scope) {
    if (text.empty()) co_return dstd::Result<void>{};
    if (file.failed.load(std::memory_order_acquire)) co_return *file.error;
    ensure_stage(file, scope);
    auto permitting = file.budget->permit(text.size());
    auto held = co_await permitting;
    auto message = dstd::make_unique<StageMessage>();
    message->text = dstd::String(text);
    message->held.emplace(std::move(held));
    auto sending = file.producer->send(std::move(message));
    co_await sending;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> WriteFeed::end_member(File& file,
                                                         coro::Scope& scope) {
    if (file.failed.load(std::memory_order_acquire)) co_return *file.error;
    ensure_stage(file, scope);
    auto message = dstd::make_unique<StageMessage>();
    message->kind = StageMessage::Kind::EndMember;
    auto sending = file.producer->send(std::move(message));
    co_await sending;
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> WriteFeed::drain(File& file) {
    if (file.producer) {
        auto message = dstd::make_unique<StageMessage>();
        message->kind = StageMessage::Kind::Finish;
        auto sending = file.producer->send(std::move(message));
        co_await sending;
        file.producer.reset();
        auto waiting = file.done.wait();
        co_await waiting;
        file.done.reset();
        file.channel.reset();
    }
    if (file.failed.load(std::memory_order_acquire)) co_return *file.error;
    co_return dstd::Result<void>{};
}

dstd::Result<void> WriteFeed::finish(File& file,
                                     dstd::Vector<FedMember> members,
                                     std::int64_t table_rows) {
    if (members.size() != file.fed_bytes.size())
        return common::make_error<"{} has {} members, the index was fed {}">(
            ErrorCode::Internal, dstd::StringView(file.final_path),
            members.size(), file.fed_bytes.size());
    gzip::GzipBuildArtifacts a;
    a.checkpoint_size = impl_->options.checkpoint_size;
    std::uint64_t compressed = 0;
    for (std::size_t i = 0; i < members.size(); ++i) {
        const FedMember& m = members[i];
        if (m.uc_size != file.fed_bytes[i] || m.lines != file.fed_lines[i])
            return common::make_error<
                "member {} of {} holds {} bytes in {} lines, the index was "
                "fed {} bytes in {} lines">(
                ErrorCode::Internal, i, dstd::StringView(file.final_path),
                m.uc_size, m.lines, file.fed_bytes[i], file.fed_lines[i]);
        gzip::GzipMemberRecord r;
        r.member_idx = i;
        r.c_offset = m.c_offset;
        r.c_size = m.c_size;
        r.uc_offset = a.total_uc_size;
        r.uc_size = m.uc_size;
        r.first_line_num = a.total_lines + 1;
        r.last_line_num = a.total_lines + m.lines;
        a.members.push_back(r);
        a.total_lines += m.lines;
        a.total_uc_size += m.uc_size;
        compressed += m.c_size;
    }
    file.driver->seal();
    a.table_rows_complete =
        file.driver->table_rows_complete(a.total_lines, table_rows);
    if (file.bloom) file.bloom->set_file_bytes(compressed);
    file.artifacts = std::move(a);
    file.is_table = table_rows >= 0;
    file.finished = true;
    impl_->admission.gate().file_out();
    return {};
}

coro::CoroTask<dstd::Result<void>> WriteFeed::commit() {
    Impl& s = *impl_;
    dstd::Map<dstd::String, dstd::Vector<File*>> by_root;
    for (const auto& f : s.files)
        if (f->finished) by_root[f->index_root].push_back(f.get());
    for (auto& [root, files] : by_root) {
        auto index = Index::open(root);
        if (!index) co_return index.error();
        auto snapshot = index->snapshot();
        if (!snapshot) co_return snapshot.error();
        // A table's rows are not events, so its aggregation tier is empty; it
        // is marked built so that the index reads as current. The tier of an
        // events file is left for a build that reads the committed file.
        auto serving = schemas::dft::agg::tier::serving_config(
            *snapshot, s.options.aggregation);
        if (!serving) co_return serving.error();
        const std::uint64_t agg_params = serving->params_hash();
        build::FileGroup group;
        std::size_t tag = 0;
        for (File* f : files) {
            struct stat st{};
            if (::stat(f->final_path.c_str(), &st) != 0)
                co_return common::make_error<"cannot stat {}">(
                    ErrorCode::Io, dstd::StringView(f->final_path));
            const auto size = static_cast<std::uint64_t>(st.st_size);
            dstd::OpenOptions read;
            auto opened = dstd::File::open(dstd::PathView(f->final_path), read);
            if (!opened)
                co_return common::make_error<"cannot open {}: {}">(
                    ErrorCode::Io, dstd::StringView(f->final_path),
                    opened.error().message());
            const SourceStat stat{static_cast<std::uint64_t>(st.st_mtime), size,
                                  fileio::calculate_file_hash(*opened, size)};
            const bool fresh = !snapshot->file(f->logical_path).has_value();
            auto started = index->build(f->logical_path, stat);
            if (!started) co_return started.error();
            build::ParsedIndexJob job;
            job.fresh = fresh;
            job.file_path = f->final_path;
            job.artifacts = std::move(f->artifacts);
            job.intern = std::move(f->intern);
            job.bloom_fold = std::move(f->bloom);
            job.rowset_fold = std::move(f->rowset);
            job.plugin_builders = std::move(f->builders);
            job.schema = s.schema;
            build::FileAdmission::Ticket ticket;
            if (!build::fill_index_job(*started, job, ticket))
                co_return common::make_error<"index of {}: {}">(
                    ErrorCode::Io, dstd::StringView(f->final_path),
                    dstd::StringView(job.error_message));
            if (f->is_table)
                started->finish(store::IndexExtension::Agg, agg_params);
            auto adding =
                group.add(std::move(*started), tag++, f->logical_path);
            const dstd::String staged = co_await std::move(adding);
            if (!staged.empty())
                co_return common::make_error<"index of {}: {}">(
                    ErrorCode::Io, dstd::StringView(f->final_path),
                    dstd::StringView(staged));
        }
        auto committing = group.commit();
        auto result = co_await std::move(committing);
        if (!result.failures.empty())
            co_return common::make_error<"index of {}: {}">(
                ErrorCode::Io, dstd::StringView(result.failures.front().path),
                dstd::StringView(result.failures.front().message));
    }
    co_return dstd::Result<void>{};
}

}  // namespace dftracer::utils::trace::index
