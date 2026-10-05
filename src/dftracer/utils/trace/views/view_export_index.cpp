#include <dftracer/utils/core/common/constants.h>
#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/string_intern.h>
#include <dftracer/utils/core/coro/channel.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/build/batch_builder.h>
#include <dftracer/utils/index/build/chunk_indexer.h>
#include <dftracer/utils/index/extensions/bloom_fold.h>
#include <dftracer/utils/index/gzip/gzip_member_record.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/internal/helpers.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/internal/utils.h>
#include <dftracer/utils/trace/views/coverage.h>
#include <dftracer/utils/trace/views/fold.h>
#include <dftracer/utils/trace/views/fold_event.h>
#include <dftracer/utils/trace/views/view_definition.h>
#include <dftracer/utils/trace/views/view_executor.h>
#include <dftracer/utils/trace/views/view_scan.h>
#include <dftracer/utils/trace/views/view_scanner_utility.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <simdjson.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

// Fused export + index: write the View's events as a new multi-member trace AND
// build its member+bloom+stats index in one pass (no re-inflate of the output).
// Scan stays parallel (producer coroutines over a channel); one consumer cuts
// members into a GzipLineWriter, whose compress workers parse each member into
// their own BloomFold slice, so member_idx == checkpoint_idx per part.
namespace dftracer::utils::trace::views::detail {

namespace {

// Per-part index data, accumulated while writing and persisted together at the
// end so all parts land in one index transaction sharing one index_path.
struct PartData {
    std::string path;
    std::vector<dftracer::utils::index::gzip::GzipMemberRecord> members;
    std::uint64_t total_uc = 0;
    std::uint64_t total_lines = 0;
};

}  // namespace

coro::CoroTask<ExportStats> run_export_trace_indexed(
    const ViewPlan& plan, const TraceWriteOptions& opts,
    const ProgressFn* progress) {
    // Member size defaults to the checkpoint granularity (a member == a chunk).
    constexpr std::size_t DEFAULT_FLUSH_BYTES =
        constants::indexer::DEFAULT_CHECKPOINT_SIZE;
    constexpr std::size_t MAX_PRODUCERS = 16;

    ViewDefinition vdef = make_vdef(plan, /*for_aggregation=*/false);
    std::uint64_t skipped = 0;
    auto units = co_await gather_units(plan, vdef, skipped);

    const std::size_t member_size =
        opts.member_size ? opts.member_size : DEFAULT_FLUSH_BYTES;
    const std::size_t part_size = opts.part_size;  // 0 = single file
    const bool multi = part_size > 0;

    const std::size_t num_producers = std::max<std::size_t>(
        1, std::min<std::size_t>(std::max<std::size_t>(units.size(), 1),
                                 MAX_PRODUCERS));

    // Index folds key their state per file_path, so one fold set spans all
    // parts; finalize (after the members are committed) writes the bloom.
    const std::string index_path = trace::internal::determine_index_path(
        opts.output_path, opts.index_path);
    const auto& schema = plan_record_schema(plan);
    dftracer::utils::StringIntern intern;
    dftracer::utils::index::extensions::BloomFold bloom(
        intern, dftracer::utils::index::build::for_schema({}, schema));
    std::array<Fold*, 1> folds{&bloom};

    std::vector<PartData> parts;
    std::atomic<std::uint64_t> matched{0}, scanned{0}, units_done{0};
    std::atomic<std::uint64_t> invalid{0}, unconverted{0};
    bool ok = true;
    // Set when the consumer fails, so producers stop scanning.
    std::atomic<bool> failed{false};

    // Each fold call takes a slice (parser, scratch and BloomFold slice), so
    // workers share no state; all slices merge into `bloom` after the writer
    // closes.
    struct FoldSlice {
        std::unique_ptr<Fold> bloom;
        dftracer::utils::json::RecordParser parser;
        std::string parse_buf;
        std::vector<FoldEvent> events;
    };
    std::mutex pool_mu;
    std::vector<std::unique_ptr<FoldSlice>> pool;

    auto fold_member = [&](const utilities::fileio::MemberRef& ref,
                           std::string_view data) {
        std::unique_ptr<FoldSlice> sl;
        {
            std::lock_guard<std::mutex> lk(pool_mu);
            if (!pool.empty()) {
                sl = std::move(pool.back());
                pool.pop_back();
            }
        }
        if (!sl) {
            sl = std::make_unique<FoldSlice>();
            sl->bloom = bloom.slice();
        }
        sl->events.clear();
        for (std::size_t ls = 0, i = 0; i < data.size(); ++i) {
            if (data[i] != '\n') continue;
            std::string_view line = data.substr(ls, i - ls);
            ls = i + 1;
            if (line.empty()) continue;
            sl->parse_buf.assign(line);
            sl->parse_buf.resize(line.size() + simdjson::SIMDJSON_PADDING,
                                 '\0');
            auto doc =
                sl->parser.parse(sl->parse_buf.data(), line.size(), false);
            if (doc.error()) continue;
            sl->events.push_back(
                vdef.by_path ? decode_record(doc.value_unsafe(), intern,
                                             /*capture_schema=*/true, nullptr,
                                             vdef.record_schema, nullptr,
                                             INDEX_MAX_CHILDREN)
                             : extract_fold_event(doc.value_unsafe(), intern,
                                                  /*needs_args=*/true,
                                                  /*extra_fields=*/nullptr,
                                                  /*capture_schema=*/true));
        }
        ScanUnit unit;
        unit.file_path = utilities::fileio::gzip_part_path(
            opts.output_path, static_cast<int>(ref.part), multi);
        unit.index_path = index_path;
        unit.checkpoint_idx = ref.part_index;
        FoldBatch fb{std::span<const FoldEvent>(sl->events), unit};
        sl->bloom->step(fb);
        sl->bloom->seal_unit(unit);
        std::lock_guard<std::mutex> lk(pool_mu);
        pool.push_back(std::move(sl));
    };

    auto part_at = [&](std::size_t p) -> PartData& {
        if (parts.size() <= p) parts.resize(p + 1);
        return parts[p];
    };

    utilities::fileio::GzipWriterOptions wopts;
    wopts.member_size = member_size;
    wopts.level = opts.level;
    wopts.workers = opts.num_workers;
    wopts.part_size = part_size;
    wopts.compress = opts.compress;
    wopts.part_name = [&](std::size_t i) {
        return utilities::fileio::gzip_part_path(opts.output_path,
                                                 static_cast<int>(i), multi);
    };
    wopts.on_member = [&](const utilities::fileio::MemberInfo& m) {
        dftracer::utils::index::gzip::GzipMemberRecord rec;
        auto& pd = part_at(m.part);
        rec.member_idx = pd.members.size();
        rec.c_offset = m.c_offset;
        rec.c_size = m.c_size;
        rec.uc_offset = m.uc_offset;
        rec.uc_size = m.uc_size;
        rec.first_line_num = m.first_line;
        rec.last_line_num = rec.first_line_num + (m.lines ? m.lines - 1 : 0);
        pd.members.push_back(rec);
    };
    wopts.on_part = [&](std::size_t part, const std::string& path,
                        std::uint64_t uc_bytes, std::uint64_t lines) {
        auto& pd = part_at(part);
        pd.path = path;
        pd.total_uc = uc_bytes;
        pd.total_lines = lines;
    };
    wopts.fold = fold_member;
    auto opened = co_await utilities::fileio::GzipLineWriter::open(
        opts.output_path, std::move(wopts));
    if (!opened) {
        throw DFTUtilsException(
            ErrorCode::IO, "export_trace failed writing " + opts.output_path);
    }
    auto writer = std::move(*opened);

    auto ch = coro::make_channel<std::string>(2 * num_producers + 1);

    co_await run_coro_scope([&](CoroScope& scope) -> coro::CoroTask<void> {
        co_await scope.scope([&](CoroScope& child) -> coro::CoroTask<void> {
            // Consumer: append batches in arrival order; the writer cuts
            // members and names their parts.
            child.spawn([&](CoroScope&) -> coro::CoroTask<void> {
                auto cons = ch->consumer();
                while (auto item = co_await cons.receive()) {
                    if (co_await writer.append(*item)) continue;
                    ok = false;
                    failed.store(true, std::memory_order_relaxed);
                    // Drain, so producers blocked on the channel finish.
                    while (co_await cons.receive()) {
                    }
                    co_return;
                }
                co_return;
            });

            // One shared producer, captured by reference (GCC rejects a
            // move-only init-capture in a coroutine lambda) and released when
            // this sub-scope joins, so the channel closes and the consumer
            // drains before close().
            {
                auto prod = ch->producer();
                co_await child.scope([&](CoroScope& workers)
                                         -> coro::CoroTask<void> {
                    for (std::size_t w = 0; w < num_producers; ++w) {
                        workers.spawn([&,
                                       w](CoroScope&) -> coro::CoroTask<void> {
                            std::string batch;
                            for (std::size_t i = w; i < units.size();
                                 i += num_producers) {
                                if (is_cancelled(plan) ||
                                    failed.load(std::memory_order_relaxed))
                                    co_return;
                                ViewScannerInput sin = make_scanner_input(
                                    units[i], vdef, vdef.query);
                                ViewScannerUtility scanner;
                                auto gen = scanner(sin);
                                while (auto b = co_await gen.next()) {
                                    if (is_cancelled(plan) ||
                                        failed.load(std::memory_order_relaxed))
                                        co_return;
                                    matched.fetch_add(
                                        b->events_matched,
                                        std::memory_order_relaxed);
                                    scanned.fetch_add(
                                        b->events_scanned,
                                        std::memory_order_relaxed);
                                    invalid.fetch_add(
                                        b->lines_invalid,
                                        std::memory_order_relaxed);
                                    unconverted.fetch_add(
                                        b->values_unconverted,
                                        std::memory_order_relaxed);
                                    if (b->events.empty()) continue;
                                    batch.clear();
                                    for (const auto& ev : b->events) {
                                        batch.append(ev);
                                        batch.push_back('\n');
                                    }
                                    co_await prod.send(batch);
                                }
                                if (progress)
                                    (*progress)(
                                        units_done.fetch_add(
                                            1, std::memory_order_relaxed) +
                                            1,
                                        units.size());
                            }
                            co_return;
                        });
                    }
                    co_return;
                });
            }
            co_return;
        });
    });

    if (ok && !co_await writer.close()) ok = false;
    if (!ok || parts.empty()) {
        throw DFTUtilsException(
            ErrorCode::IO, "export_trace failed writing " + opts.output_path);
    }

    for (auto& sl : pool) bloom.merge(*sl->bloom);

    // Persist the index for every part in one transaction. Registering each
    // with its exact mtime/size makes the resolver accept it as fresh; any
    // later edit trips the stale check and rebuilds.
    try {
        dftracer::utils::index::store::IndexDatabase db(index_path);
        auto w = db.begin_write();
        // BloomFold::finalize below writes the dft.index data; until then
        // the parts have members but no bloom tier, which a later scan fills.
        namespace records = dftracer::utils::index::store::records;
        using dftracer::utils::index::store::IndexExtension;
        for (const auto& part : parts) {
            auto logical =
                dftracer::utils::index::store::internal::get_logical_path(
                    part.path);
            const auto hash =
                dftracer::utils::index::store::internal::calculate_file_hash(
                    part.path);
            const auto mtime = static_cast<std::uint64_t>(
                dftracer::utils::index::store::internal::
                    get_file_modification_time(part.path));
            const auto bytes =
                dftracer::utils::index::store::internal::file_size_bytes(
                    part.path);
            const int fid = w->file_id_for(logical);
            records::put_file_record(
                *w, logical,
                {static_cast<std::uint32_t>(fid), mtime, hash, bytes});
            for (auto ext : {IndexExtension::ZONEMAP, IndexExtension::BLOOM,
                             IndexExtension::COUNTS, IndexExtension::POSTINGS,
                             IndexExtension::STATS})
                records::clear_file(*w, ext, fid);
            records::clear_file(*w, IndexExtension::MEMBERS, fid);
            for (const auto& m : part.members)
                records::put_gzip_member(*w, fid, m);
            records::put_file_metadata(*w, fid, member_size, part.total_lines,
                                       part.total_uc, false);
            records::put_manifest(*w, fid, IndexExtension::MEMBERS, 0);
            records::put_profile(*w, fid, schema.id, schema.params_hash());
        }
        w->commit();
    } catch (const std::exception& e) {
        throw DFTUtilsException(ErrorCode::IO,
                                std::string("fused index build failed for ") +
                                    opts.output_path + ": " + e.what());
    }

    // Members are committed, so the folds' finalize can open the index, resolve
    // each file, and write the bloom as a second transaction.
    CoverageSet whole_file;
    for (const auto& part : parts) whole_file.add_file(part.path);
    for (auto* f : folds) co_await f->finalize(whole_file);

    ExportStats st;
    st.chunks_skipped = skipped;
    st.chunks_scanned = units.size();
    st.events_matched = matched.load(std::memory_order_relaxed);
    st.events_scanned = scanned.load(std::memory_order_relaxed);
    st.lines_invalid = invalid.load(std::memory_order_relaxed);
    st.values_unconverted = unconverted.load(std::memory_order_relaxed);
    co_return st;
}

}  // namespace dftracer::utils::trace::views::detail
