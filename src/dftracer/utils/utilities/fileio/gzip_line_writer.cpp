// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/platform_compat.h>
#include <dftracer/utils/coro/async_mutex.h>
#include <dftracer/utils/coro/async_semaphore.h>
#include <dftracer/utils/coro/named.h>
#include <dftracer/utils/coro/scope.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/budget.h>
#include <dftracer/utils/runtime/runtime.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/fileio/long_lived_release.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <utility>

namespace dftracer::utils::utilities::fileio {

dstd::String gzip_part_path(dstd::StringView base, int idx, bool multi) {
    if (!multi) return dstd::String(base);
    const std::size_t pos = base.rfind(".pfw");
    if (pos == dstd::StringView::npos)
        return dstd::String(base) + "-" + dstd::to_string(idx);
    return dstd::String(base.substr(0, pos)) + "-" + dstd::to_string(idx) +
           dstd::String(base.substr(pos));
}

namespace {

constexpr std::size_t BUFFER_HEADROOM_BYTES = 1 * 1024 * 1024;
constexpr std::size_t EXTRA_IN_FLIGHT = 2;

struct Job {
    std::uint64_t seq;
    dstd::String data;
    dstd::UniquePtr<char[]> raw;  ///< when set, the bytes instead of `data`
    std::size_t raw_size = 0;
    std::size_t part = 0;
    std::uint64_t part_index = 0;
    std::size_t units = 0;  ///< reservation held by this job
    bool ends_part = false;

    dstd::StringView bytes() const {
        return raw ? dstd::StringView(raw.get(), raw_size)
                   : dstd::StringView(data);
    }
};

struct Done {
    dstd::Vector<std::uint8_t> comp;
    dstd::String plain;
    std::uint64_t uc_size = 0;
    std::uint64_t lines = 0;
    std::size_t part = 0;
    std::size_t units = 0;
    bool ends_part = false;
};

constexpr std::size_t UNIT_BYTES = 1024;

}  // namespace

struct GzipLineWriter::Impl {
    Impl(dstd::String base_path, GzipWriterOptions o, std::size_t w)
        : base(std::move(base_path)),
          opts(std::move(o)),
          workers(w),
          compute(w) {
        for (std::size_t i = w; i-- > 0;) free_slots.push_back(i);
        const std::uint64_t budget =
            memory::resolve_spill_budget(opts.memory_budget);
        auto c = take_compressor();
        const std::size_t m = opts.member_size;
        const std::size_t job_b =
            m + (opts.compress ? c->bound(m) : std::size_t{0});
        give_compressor(std::move(c));
        member_units = units_of(m);
        const std::uint64_t total = std::min<std::uint64_t>(
            budget / UNIT_BYTES, std::uint64_t{1} << 40);
        const std::size_t job_unit = units_of(job_b);
        job_cap = std::max<std::size_t>(job_unit, total / 2);
        prod_cap = std::max<std::size_t>(2 * member_units, total - job_cap);
        const std::size_t max_jobs =
            std::min<std::size_t>(w + EXTRA_IN_FLIGHT, job_cap / job_unit);
        slots = dstd::make_unique<coro::AsyncSemaphore>(
            std::max<std::size_t>(1, max_jobs));
        prod_mem =
            dstd::make_unique<coro::AsyncSemaphore>(prod_cap - member_units);
        job_mem = dstd::make_unique<coro::AsyncSemaphore>(job_cap);
        // The shared buffer (ordered mode, unordered append) is one member.
        held.store(member_units, std::memory_order_relaxed);
        peak_held.store(member_units, std::memory_order_relaxed);
    }

    static std::size_t units_of(std::size_t bytes) {
        return (bytes + UNIT_BYTES - 1) / UNIT_BYTES;
    }

    std::size_t member_units = 0, job_cap = 0, prod_cap = 0;
    dstd::UniquePtr<coro::AsyncSemaphore> prod_mem, job_mem;
    std::atomic<std::uint64_t> held{0}, peak_held{0}, prod_held{0};

    void note_held(std::int64_t delta) {
        const std::uint64_t now =
            held.fetch_add(static_cast<std::uint64_t>(delta),
                           std::memory_order_relaxed) +
            static_cast<std::uint64_t>(delta);
        std::uint64_t p = peak_held.load(std::memory_order_relaxed);
        while (now > p && !peak_held.compare_exchange_weak(p, now)) {
        }
    }

    // Waits for `units` (clamped to the pool) and returns what it took.
    coro::CoroTask<std::size_t> reserve(coro::AsyncSemaphore& sem,
                                        std::size_t cap, std::size_t units) {
        units = std::min(units, cap);
        if (units == 0) co_return 0;
        co_await sem.acquire(units);
        if (&sem == prod_mem.get())
            prod_held.fetch_add(units, std::memory_order_relaxed);
        note_held(static_cast<std::int64_t>(units));
        co_return units;
    }

    void unreserve(coro::AsyncSemaphore& sem, std::size_t units) {
        if (units == 0) return;
        if (&sem == prod_mem.get())
            prod_held.fetch_sub(units, std::memory_order_relaxed);
        note_held(-static_cast<std::int64_t>(units));
        sem.release(units);
    }

    dstd::String base;
    GzipWriterOptions opts;
    std::size_t workers;
    coro::Scope scope;
    dstd::UniquePtr<coro::AsyncSemaphore> slots;
    coro::AsyncSemaphore compute;
    coro::AsyncMutex drain_mu;

    std::mutex buf_mu;
    dstd::String buf;
    std::atomic<std::uint64_t> next_seq{0};
    // Submit decides every member's part (ordered mode only).
    std::size_t sub_part = 0;
    std::uint64_t sub_index = 0, sub_uc = 0;
    dstd::Vector<std::size_t> free_slots;

    std::mutex mu;
    dstd::HashMap<std::uint64_t, Done> ready;
    std::uint64_t next_write = 0;
    dstd::Optional<dstd::Error> error;
    dstd::Vector<dstd::UniquePtr<compress::GzipMemberCompressor>> pool;

    std::atomic<std::size_t> in_flight{0};
    std::atomic<std::size_t> peak{0};

    // Writer step state; touched only under drain_mu, or after join.
    dstd::UniquePtr<parallel::ParallelWriter> writer;
    parallel::FileLayout layout = parallel::FileLayout::Striped;
    dstd::String cur_path;
    std::size_t part = 0;
    std::uint64_t member_idx = 0;
    std::uint64_t part_c = 0;
    std::uint64_t part_uc = 0;
    std::uint64_t part_lines = 0;
    dstd::Vector<dstd::String> created;
    WriteSummary summary;
    bool cleaned = false;
    bool closed = false;

    bool settled() const { return cleaned || closed; }

    void abandon() {
        set_error(common::ErrorCode::Internal,
                  "writer destroyed without close");
    }

    coro::CoroTask<void> join_and_clean() {
        (void)co_await fail();
        co_return;
    }

    bool multi() const { return opts.part_size > 0; }
    bool unordered() const { return !opts.ordered; }

    void set_error(common::ErrorCode code, dstd::String msg) {
        std::lock_guard<std::mutex> lk(mu);
        if (!error) error = dstd::Error{code, msg};
    }

    dstd::Error get_error() {
        std::lock_guard<std::mutex> lk(mu);
        return *error;
    }

    bool failed() {
        std::lock_guard<std::mutex> lk(mu);
        return error.has_value();
    }

    coro::CoroTask<bool> open_part() {
        cur_path = opts.part_name
                       ? opts.part_name(part)
                       : gzip_part_path(base, static_cast<int>(part), multi());
        created.push_back(cur_path);
        auto cw = parallel::make_writer_for_path(
            {cur_path, unordered() ? workers : 1, opts.member_size,
             BUFFER_HEADROOM_BYTES, opts.compress});
        writer = std::move(cw.writer);
        layout = cw.layout.layout;
        part_c = part_uc = part_lines = 0;
        const auto open_rc = co_await writer->open(
            cur_path, unordered() ? workers : 1, opts.compress, &scope);
        if (open_rc != 0) {
            set_error(common::ErrorCode::Io, "cannot open " + cur_path);
            co_return false;
        }
        for (auto& p : writer->output_paths()) created.push_back(p);
        co_return co_await write_extra(opts.part_header, true);
    }

    coro::CoroTask<bool> write_extra(const dstd::String& bytes, bool header) {
        if (bytes.empty()) co_return true;
        dstd::Vector<std::uint8_t> comp;
        if (opts.compress) {
            auto c = take_compressor();
            const bool ok = c->valid() && c->compress_member_into(
                                              comp, bytes.data(), bytes.size());
            give_compressor(std::move(c));
            if (!ok) {
                set_error(common::ErrorCode::Compression,
                          "member compression failed");
                co_return false;
            }
        }
        dstd::ByteView chunk = opts.compress
                                   ? dstd::ByteView(comp.data(), comp.size())
                                   : dstd::ByteView(bytes.data(), bytes.size());
        if (unordered()) {
            const int rc = header ? co_await writer->write_header(chunk)
                                  : co_await writer->write_footer(chunk);
            if (rc != 0) {
                set_error(common::ErrorCode::Io,
                          "write failed for " + cur_path);
                co_return false;
            }
            summary.c_bytes += chunk.size();
            co_return true;
        }
        const auto write_chunk_rc = co_await writer->write_chunk(0, chunk);
        if (write_chunk_rc != 0) {
            set_error(common::ErrorCode::Io, "write failed for " + cur_path);
            co_return false;
        }
        auto span = writer->last_member(0);
        part_c = span ? span->offset + span->length : part_c + chunk.size();
        summary.c_bytes += span ? span->length : chunk.size();
        co_return true;
    }

    coro::CoroTask<bool> close_part() {
        if (!writer) co_return true;
        bool ok = co_await write_extra(opts.part_footer, false);
        ok = co_await writer->close() == 0 && ok;
        if (ok && layout == parallel::FileLayout::Sharded) {
            // GCC 12 destroys a temporary built inside co_await twice.
            const dstd::Vector<dstd::String> shards = writer->output_paths();
            ok = co_await parallel::merge_shards(cur_path, shards) == 0;
        }
        writer.reset();
        if (!ok) {
            if (!failed())
                set_error(common::ErrorCode::Io, "cannot finish " + cur_path);
            co_return false;
        }
        if (opts.on_part) opts.on_part(part, cur_path, part_uc, part_lines);
        co_return true;
    }

    coro::CoroTask<void> write_member(Done& d) {
        if (!writer) {
            part = d.part;
            const auto open_part_ok = co_await open_part();
            if (!open_part_ok) co_return;
        }
        const bool gz = opts.compress;
        dstd::ByteView chunk =
            gz ? dstd::ByteView(d.comp.data(), d.comp.size())
               : dstd::ByteView(d.plain.data(), d.plain.size());
        const auto write_chunk_rc = co_await writer->write_chunk(0, chunk);
        if (write_chunk_rc != 0) {
            set_error(common::ErrorCode::Io, "write failed for " + cur_path);
            co_return;
        }
        auto span = writer->last_member(0);
        MemberInfo m{};
        m.part = part;
        m.index = member_idx++;
        m.c_offset = span ? span->offset : part_c;
        m.c_size = span ? span->length : chunk.size();
        m.uc_offset = part_uc;
        m.uc_size = d.uc_size;
        m.first_line = part_lines + 1;
        m.lines = d.lines;
        part_c = m.c_offset + m.c_size;
        part_uc += d.uc_size;
        part_lines += d.lines;
        summary.members++;
        summary.lines += d.lines;
        summary.uc_bytes += d.uc_size;
        summary.c_bytes += m.c_size;
        if (opts.on_member) opts.on_member(m);
        if (d.ends_part) {
            const auto part_closed = co_await close_part();
            if (!part_closed) co_return;
        }
    }

    coro::CoroTask<void> write_unordered(std::size_t slot, Done& d) {
        dstd::ByteView chunk =
            opts.compress ? dstd::ByteView(d.comp.data(), d.comp.size())
                          : dstd::ByteView(d.plain.data(), d.plain.size());
        const auto write_chunk_rc = co_await writer->write_chunk(slot, chunk);
        if (write_chunk_rc != 0) {
            set_error(common::ErrorCode::Io, "write failed for " + cur_path);
            co_return;
        }
        std::lock_guard<std::mutex> lk(mu);
        summary.members++;
        summary.lines += d.lines;
        summary.uc_bytes += d.uc_size;
        summary.c_bytes += chunk.size();
    }

    coro::CoroTask<void> drain() {
        co_await drain_mu.lock();
        for (;;) {
            Done d;
            {
                std::lock_guard<std::mutex> lk(mu);
                auto it = ready.find(next_write);
                if (it == ready.end()) break;
                d = std::move(it->second);
                ready.erase(it);
                ++next_write;
            }
            if (!failed()) co_await write_member(d);
            in_flight.fetch_sub(1, std::memory_order_relaxed);
            unreserve(*job_mem, d.units);
            slots->release(1);
        }
        drain_mu.unlock();
    }

    dstd::UniquePtr<compress::GzipMemberCompressor> take_compressor() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!pool.empty()) {
                auto c = std::move(pool.back());
                pool.pop_back();
                return c;
            }
        }
        return dstd::make_unique<compress::GzipMemberCompressor>(opts.level);
    }

    void give_compressor(dstd::UniquePtr<compress::GzipMemberCompressor> c) {
        std::lock_guard<std::mutex> lk(mu);
        pool.push_back(std::move(c));
    }

    coro::CoroTask<void> run_job(dstd::SharedPtr<Job> job) {
        co_await compute.acquire(1);
        std::size_t slot = 0;
        if (unordered()) {
            std::lock_guard<std::mutex> lk(mu);
            slot = free_slots.back();
            free_slots.pop_back();
        }
        Done d;
        d.part = job->part;
        d.ends_part = job->ends_part;
        const dstd::StringView bytes = job->bytes();
        d.units = job->units;
        d.uc_size = bytes.size();
        d.lines = static_cast<std::uint64_t>(
            std::count(bytes.begin(), bytes.end(), '\n'));
        if (!failed()) {
            try {
                if (opts.fold)
                    opts.fold({job->seq, job->part, job->part_index}, bytes);
                if (opts.compress) {
                    auto c = take_compressor();
                    const bool ok =
                        c->valid() && c->compress_member_into(
                                          d.comp, bytes.data(), bytes.size());
                    give_compressor(std::move(c));
                    if (!ok)
                        set_error(common::ErrorCode::Compression,
                                  "member compression failed");
                } else {
                    if (job->raw)
                        d.plain.assign(bytes);
                    else
                        d.plain = std::move(job->data);
                }
            } catch (const std::exception& e) {
                set_error(common::ErrorCode::Internal, e.what());
            } catch (...) {
                set_error(common::ErrorCode::Internal,
                          "unknown worker failure");
            }
        }
        if (unordered()) {
            if (!failed()) co_await write_unordered(slot, d);
            {
                std::lock_guard<std::mutex> lk(mu);
                free_slots.push_back(slot);
            }
            compute.release(1);
            in_flight.fetch_sub(1, std::memory_order_relaxed);
            unreserve(*job_mem, job->units);
            slots->release(1);
            co_return;
        }
        compute.release(1);
        {
            std::lock_guard<std::mutex> lk(mu);
            ready.emplace(job->seq, std::move(d));
        }
        co_await drain();
    }

    coro::CoroTask<void> submit(dstd::String data,
                                dstd::UniquePtr<char[]> raw = nullptr,
                                std::size_t raw_size = 0) {
        const std::size_t size0 = raw ? raw_size : data.size();
        std::size_t jb = size0;
        if (opts.compress) {
            auto c = take_compressor();
            jb += c->bound(size0);
            give_compressor(std::move(c));
        }
        const std::size_t units =
            co_await reserve(*job_mem, job_cap, units_of(jb));
        co_await slots->acquire(1);
        const std::size_t now =
            in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
        std::size_t p = peak.load(std::memory_order_relaxed);
        while (now > p && !peak.compare_exchange_weak(p, now)) {
        }
        const std::size_t size = raw ? raw_size : data.size();
        auto job = dstd::make_shared<Job>(
            Job{next_seq.fetch_add(1, std::memory_order_relaxed),
                std::move(data), std::move(raw), raw_size});
        job->units = units;
        if (unordered()) {
            job->part_index = job->seq;
        } else {
            job->part = sub_part;
            job->part_index = sub_index++;
            sub_uc += size;
            if (multi() && sub_uc >= opts.part_size) {
                job->ends_part = true;
                ++sub_part;
                sub_index = sub_uc = 0;
            }
        }
        scope.spawn(
            [this, job]() -> coro::CoroTask<void> { co_await run_job(job); });
    }

    dstd::UniquePtr<char[]> scratch;  ///< ordered append_with, reused
    std::size_t scratch_cap = 0;

    coro::CoroTask<dstd::Error> fail() {
        // A job a shutdown cancelled raises at the join, and the files of the
        // failed write are removed all the same.
        std::exception_ptr job_error;
        try {
            co_await scope.join();
        } catch (...) {
            job_error = std::current_exception();
        }
        if (!cleaned) {
            cleaned = true;
            if (writer) co_await writer->close();
            writer.reset();
            // Cleanup of a failed write, so a file already gone is fine.
            for (auto& p : created) (void)dstd::fs::remove(dstd::PathView(p));
        }
        if (job_error) std::rethrow_exception(job_error);
        co_return get_error();
    }
};

GzipLineWriter::GzipLineWriter() = default;
GzipLineWriter::GzipLineWriter(dstd::UniquePtr<Impl> impl)
    : impl_(std::move(impl)) {}
GzipLineWriter::GzipLineWriter(GzipLineWriter&&) noexcept = default;
GzipLineWriter& GzipLineWriter::operator=(GzipLineWriter&& o) noexcept {
    if (this != &o) {
        release_long_lived(std::move(impl_), "gzip_line_writer_abort");
        impl_ = std::move(o.impl_);
    }
    return *this;
}
GzipLineWriter::~GzipLineWriter() {
    release_long_lived(std::move(impl_), "gzip_line_writer_abort");
}

coro::CoroTask<dstd::Result<GzipLineWriter>> GzipLineWriter::open(
    dstd::String path, GzipWriterOptions opts) {
    if (opts.member_size == 0)
        co_return common::make_error(common::ErrorCode::InvalidArgument,
                                     "member_size must be positive");
    if (!opts.ordered && (opts.on_member || opts.on_part || opts.part_name ||
                          opts.part_size > 0))
        co_return common::make_error(
            common::ErrorCode::InvalidArgument,
            "unordered mode takes no on_member, on_part, part_size or "
            "part_name");
    const std::size_t workers =
        opts.workers
            ? opts.workers
            : std::max<std::size_t>(1, common::available_parallelism());
    auto impl =
        dstd::make_unique<Impl>(std::move(path), std::move(opts), workers);
    const auto open_part_ok = co_await impl->open_part();
    if (!open_part_ok) {
        dstd::Error err = co_await impl->fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_return GzipLineWriter(std::move(impl));
}

namespace {
// Takes every complete member out of `buf` (caller holds any lock on it).
dstd::Vector<dstd::String> take_members(dstd::String& buf, std::size_t size) {
    dstd::Vector<dstd::Pair<std::size_t, std::size_t>> spans;
    std::size_t pos = 0;
    while (buf.size() > pos + size) {
        const std::size_t nl = buf.find('\n', pos + size);
        if (nl == dstd::String::npos) break;
        spans.emplace_back(pos, nl + 1 - pos);
        pos = nl + 1;
    }
    dstd::Vector<dstd::String> out;
    if (spans.empty()) return out;
    // The last member takes over `buf` and only the tail after it is copied;
    // copying every member would fault in a fresh member-sized buffer each.
    for (std::size_t i = 0; i + 1 < spans.size(); ++i)
        out.push_back(buf.substr(spans[i].first, spans[i].second));
    dstd::String rest(buf, pos);
    buf.resize(pos);
    if (spans.back().first > 0) buf.erase(0, spans.back().first);
    out.push_back(std::move(buf));
    buf = std::move(rest);
    return out;
}
}  // namespace

coro::CoroTask<dstd::Result<void>> GzipLineWriter::append(
    dstd::StringView lines) {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    if (lines.empty()) co_return dstd::Result<void>{};
    if (lines.back() != '\n')
        co_return common::make_error(common::ErrorCode::InvalidArgument,
                                     "appended lines must end with a newline");
    dstd::Vector<dstd::String> members;
    {
        std::lock_guard<std::mutex> lk(s.buf_mu);
        s.buf.append(lines);
        members = take_members(s.buf, s.opts.member_size);
    }
    for (auto& m : members) co_await s.submit(std::move(m));
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::append_with(
    std::size_t max_bytes,
    dstd::Function<std::size_t(char*, std::size_t)> fill) {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    const char* bad = "appended lines must end with a newline";
    if (s.unordered()) {
        const std::size_t held_units =
            co_await s.reserve(*s.prod_mem, s.prod_cap - s.member_units,
                               Impl::units_of(max_bytes));
        dstd::UniquePtr<char[]> own =
            dstd::make_unique_for_overwrite<char[]>(max_bytes);
        const std::size_t n = fill(own.get(), max_bytes);
        if (n > max_bytes || (n > 0 && own[n - 1] != '\n')) {
            s.unreserve(*s.prod_mem, held_units);
            co_return common::make_error(common::ErrorCode::InvalidArgument,
                                         bad);
        }
        if (n > 0) {
            // GCC 12 destroys a temporary built inside co_await twice.
            dstd::String none;
            co_await s.submit(std::move(none), std::move(own), n);
        }
        s.unreserve(*s.prod_mem, held_units);
    } else {
        if (s.scratch_cap < max_bytes) {
            s.scratch = dstd::make_unique_for_overwrite<char[]>(max_bytes);
            s.scratch_cap = max_bytes;
        }
        const std::size_t n = fill(s.scratch.get(), max_bytes);
        if (n > max_bytes || (n > 0 && s.scratch[n - 1] != '\n'))
            co_return common::make_error(common::ErrorCode::InvalidArgument,
                                         bad);
        s.buf.append(s.scratch.get(), n);
        for (auto& m : take_members(s.buf, s.opts.member_size))
            co_await s.submit(std::move(m));
    }
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_return dstd::Result<void>{};
}

GzipLineWriter::Producer::Producer(Impl* impl) : impl_(impl) {}
GzipLineWriter::Producer::Producer(Producer&& o) noexcept
    : impl_(std::exchange(o.impl_, nullptr)),
      buf_(std::move(o.buf_)),
      units_(std::exchange(o.units_, 0)) {}
GzipLineWriter::Producer& GzipLineWriter::Producer::operator=(
    Producer&& o) noexcept {
    if (this != &o) {
        if (impl_) impl_->unreserve(*impl_->prod_mem, units_);
        impl_ = std::exchange(o.impl_, nullptr);
        buf_ = std::move(o.buf_);
        units_ = std::exchange(o.units_, 0);
    }
    return *this;
}
GzipLineWriter::Producer::~Producer() {
    if (impl_) impl_->unreserve(*impl_->prod_mem, units_);
}

dstd::Result<GzipLineWriter::Producer> GzipLineWriter::producer() {
    if (!impl_->unordered())
        return common::make_error(common::ErrorCode::InvalidArgument,
                                  "producers need unordered mode");
    return Producer(impl_.get());
}

// Holds buffer + extra bytes. A producer with leftover bytes drops its
// reservation before it waits for a larger one, so two never wait on each
// other.
coro::CoroTask<void> GzipLineWriter::Producer::reserve_for(std::size_t extra) {
    Impl& s = *impl_;
    const std::size_t need = Impl::units_of(buf_.size() + extra);
    if (need <= units_) co_return;
    s.unreserve(*s.prod_mem, units_);
    units_ = 0;
    units_ = co_await s.reserve(*s.prod_mem, s.prod_cap - s.member_units, need);
}

void GzipLineWriter::Producer::release_extra() {
    Impl& s = *impl_;
    const std::size_t keep = Impl::units_of(buf_.size());
    if (keep < units_) {
        s.unreserve(*s.prod_mem, units_ - keep);
        units_ = keep;
    }
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::Producer::submit_full() {
    Impl& s = *impl_;
    auto members = take_members(buf_, s.opts.member_size);
    for (auto& m : members) co_await s.submit(std::move(m));
    release_extra();
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::Producer::append(
    dstd::StringView lines) {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    if (lines.empty()) co_return dstd::Result<void>{};
    if (lines.back() != '\n')
        co_return common::make_error(common::ErrorCode::InvalidArgument,
                                     "appended lines must end with a newline");
    co_await reserve_for(lines.size());
    buf_.append(lines);
    co_return co_await submit_full();
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::Producer::append_with(
    std::size_t max_bytes,
    dstd::Function<std::size_t(char*, std::size_t)> fill) {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_await reserve_for(max_bytes);
    const std::size_t old = buf_.size();
    buf_.resize(old + max_bytes);
    const std::size_t n = fill(buf_.data() + old, max_bytes);
    if (n > max_bytes || (n > 0 && buf_[old + n - 1] != '\n')) {
        buf_.resize(old);
        release_extra();
        co_return common::make_error(common::ErrorCode::InvalidArgument,
                                     "appended lines must end with a newline");
    }
    buf_.resize(old + n);
    co_return co_await submit_full();
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::Producer::flush() {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    if (buf_.empty()) co_return dstd::Result<void>{};
    dstd::String pending = std::move(buf_);
    buf_.clear();
    co_await s.submit(std::move(pending));
    release_extra();
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<void>> GzipLineWriter::cut() {
    Impl& s = *impl_;
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    dstd::String pending;
    {
        std::lock_guard<std::mutex> lk(s.buf_mu);
        pending.swap(s.buf);
    }
    if (!pending.empty()) co_await s.submit(std::move(pending));
    co_return dstd::Result<void>{};
}

coro::CoroTask<dstd::Result<WriteSummary>> GzipLineWriter::close() {
    co_return co_await close(dstd::StringView{});
}

coro::CoroTask<dstd::Result<WriteSummary>> GzipLineWriter::close(
    dstd::StringView tail) {
    Impl& s = *impl_;
    if (s.cleaned) co_return dstd::Unexpected(s.get_error());
    if (s.prod_held.load(std::memory_order_relaxed) != 0) {
        s.set_error(common::ErrorCode::InvalidArgument,
                    "close with unflushed producers");
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    s.buf.append(tail);
    if (!s.failed() && !s.buf.empty()) co_await s.submit(std::move(s.buf));
    s.buf.clear();
    co_await s.scope.join();
    if (s.failed()) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    s.summary.parts = s.writer ? s.part + 1 : s.part;
    const auto close_part_ok = co_await s.close_part();
    if (!close_part_ok) {
        dstd::Error err = co_await s.fail();
        co_return dstd::Unexpected(std::move(err));
    }
    s.summary.peak_in_flight = s.peak.load(std::memory_order_relaxed);
    s.summary.peak_bytes =
        s.peak_held.load(std::memory_order_relaxed) * UNIT_BYTES;
    s.closed = true;
    co_return s.summary;
}

GzipLineWriterBlocking::GzipLineWriterBlocking(GzipLineWriter w)
    : w_(dstd::make_unique<GzipLineWriter>(std::move(w))) {}
GzipLineWriterBlocking::GzipLineWriterBlocking(
    GzipLineWriterBlocking&&) noexcept = default;
GzipLineWriterBlocking& GzipLineWriterBlocking::operator=(
    GzipLineWriterBlocking&&) noexcept = default;
GzipLineWriterBlocking::~GzipLineWriterBlocking() = default;

namespace {
template <typename T, typename F>
dstd::Result<T> run_on_default(const char* name, F&& f) {
    dstd::Optional<dstd::Result<T>> out;
    default_runtime().block_on(coro::named(name, [&]() -> coro::CoroTask<void> {
        out.emplace(co_await f());
        co_return;
    }()));
    return std::move(*out);
}
}  // namespace

dstd::Result<GzipLineWriterBlocking> GzipLineWriterBlocking::open(
    dstd::String path, GzipWriterOptions opts) {
    auto r = run_on_default<GzipLineWriter>("gzip_line_writer_open", [&] {
        return GzipLineWriter::open(std::move(path), std::move(opts));
    });
    if (!r) return dstd::Unexpected(r.error());
    return GzipLineWriterBlocking(std::move(*r));
}

dstd::Result<void> GzipLineWriterBlocking::append(dstd::StringView lines) {
    return run_on_default<void>("gzip_line_writer_append",
                                [&] { return w_->append(lines); });
}

dstd::Result<void> GzipLineWriterBlocking::append_with(
    std::size_t max_bytes,
    dstd::Function<std::size_t(char*, std::size_t)> fill) {
    return run_on_default<void>("gzip_line_writer_append_with", [&] {
        return w_->append_with(max_bytes, std::move(fill));
    });
}

dstd::Result<void> GzipLineWriterBlocking::cut() {
    return run_on_default<void>("gzip_line_writer_cut",
                                [&] { return w_->cut(); });
}

dstd::Result<WriteSummary> GzipLineWriterBlocking::close() {
    return close(dstd::StringView{});
}

dstd::Result<WriteSummary> GzipLineWriterBlocking::close(
    dstd::StringView tail) {
    return run_on_default<WriteSummary>("gzip_line_writer_close",
                                        [&] { return w_->close(tail); });
}

}  // namespace dftracer::utils::utilities::fileio
