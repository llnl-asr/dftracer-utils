#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/platform_compat.h>
#include <dftracer/utils/core/coro/async_mutex.h>
#include <dftracer/utils/core/coro/async_semaphore.h>
#include <dftracer/utils/core/runtime.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/fileio/compress/libdeflate_gzip.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dftracer::utils::utilities::fileio {

namespace pfw = parallel;
namespace cmp = compress;

std::string gzip_part_path(const std::string& base, int idx, bool multi) {
    if (!multi) return base;
    const std::size_t pos = base.rfind(".pfw");
    if (pos == std::string::npos) return base + "-" + std::to_string(idx);
    return base.substr(0, pos) + "-" + std::to_string(idx) + base.substr(pos);
}

namespace {

constexpr std::size_t BUFFER_HEADROOM_BYTES = 1 * 1024 * 1024;
constexpr std::size_t EXTRA_IN_FLIGHT = 2;

struct Job {
    std::uint64_t seq;
    std::string data;
    std::unique_ptr<char[]> raw;  ///< when set, the bytes instead of `data`
    std::size_t raw_size = 0;
    std::size_t part = 0;
    std::uint64_t part_index = 0;
    std::size_t units = 0;  ///< reservation held by this job
    bool ends_part = false;

    std::string_view bytes() const {
        return raw ? std::string_view(raw.get(), raw_size)
                   : std::string_view(data);
    }
};

struct Done {
    std::vector<std::uint8_t> comp;
    std::string plain;
    std::uint64_t uc_size = 0;
    std::uint64_t lines = 0;
    std::size_t part = 0;
    std::size_t units = 0;
    bool ends_part = false;
};

constexpr std::size_t UNIT_BYTES = 1024;

}  // namespace

struct GzipLineWriter::Impl {
    Impl(std::string base_path, GzipWriterOptions o, std::size_t w)
        : base(std::move(base_path)),
          opts(std::move(o)),
          workers(w),
          compute(w) {
        for (std::size_t i = w; i-- > 0;) free_slots.push_back(i);
        const std::uint64_t budget = resolve_spill_budget(opts.memory_budget);
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
        slots = std::make_unique<coro::CoroSemaphore>(
            std::max<std::size_t>(1, max_jobs));
        prod_mem =
            std::make_unique<coro::CoroSemaphore>(prod_cap - member_units);
        job_mem = std::make_unique<coro::CoroSemaphore>(job_cap);
        // The shared buffer (ordered mode, unordered append) is one member.
        held.store(member_units, std::memory_order_relaxed);
        peak_held.store(member_units, std::memory_order_relaxed);
    }

    static std::size_t units_of(std::size_t bytes) {
        return (bytes + UNIT_BYTES - 1) / UNIT_BYTES;
    }

    std::size_t member_units = 0, job_cap = 0, prod_cap = 0;
    std::unique_ptr<coro::CoroSemaphore> prod_mem, job_mem;
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
    coro::CoroTask<std::size_t> reserve(coro::CoroSemaphore& sem,
                                        std::size_t cap, std::size_t units) {
        units = std::min(units, cap);
        if (units == 0) co_return 0;
        co_await sem.acquire(units);
        if (&sem == prod_mem.get())
            prod_held.fetch_add(units, std::memory_order_relaxed);
        note_held(static_cast<std::int64_t>(units));
        co_return units;
    }

    void unreserve(coro::CoroSemaphore& sem, std::size_t units) {
        if (units == 0) return;
        if (&sem == prod_mem.get())
            prod_held.fetch_sub(units, std::memory_order_relaxed);
        note_held(-static_cast<std::int64_t>(units));
        sem.release(units);
    }

    std::string base;
    GzipWriterOptions opts;
    std::size_t workers;
    CoroScope scope;
    std::unique_ptr<coro::CoroSemaphore> slots;
    coro::CoroSemaphore compute;
    coro::AsyncMutex drain_mu;

    std::mutex buf_mu;
    std::string buf;
    std::atomic<std::uint64_t> next_seq{0};
    // Submit decides every member's part (ordered mode only).
    std::size_t sub_part = 0;
    std::uint64_t sub_index = 0, sub_uc = 0;
    std::vector<std::size_t> free_slots;

    std::mutex mu;
    std::map<std::uint64_t, Done> ready;
    std::uint64_t next_write = 0;
    std::optional<DFTUtilsError> error;
    std::vector<std::unique_ptr<cmp::GzipMemberCompressor>> pool;

    std::atomic<std::size_t> in_flight{0};
    std::atomic<std::size_t> peak{0};

    // Writer step state; touched only under drain_mu, or after join.
    std::unique_ptr<pfw::ParallelWriter> writer;
    pfw::FileLayout layout = pfw::FileLayout::STRIPED;
    std::string cur_path;
    std::size_t part = 0;
    std::uint64_t member_idx = 0;
    std::uint64_t part_c = 0;
    std::uint64_t part_uc = 0;
    std::uint64_t part_lines = 0;
    std::vector<std::string> created;
    WriteSummary summary;
    bool cleaned = false;
    bool closed = false;

    ~Impl() {
        if (cleaned || closed) return;
        set_error(ErrorCode::INTERNAL, "writer destroyed without close");
        try {
            default_runtime().run_blocking(
                "gzip_line_writer_abort",
                [this](CoroScope&) -> coro::CoroTask<void> {
                    co_await fail();
                    co_return;
                });
        } catch (...) {
            std::error_code ec;
            for (auto& p : created) fs::remove(p, ec);
        }
    }

    bool multi() const { return opts.part_size > 0; }
    bool unordered() const { return !opts.ordered; }

    void set_error(ErrorCode code, std::string msg) {
        std::lock_guard<std::mutex> lk(mu);
        if (!error) error = DFTUtilsError{code, std::move(msg)};
    }

    DFTUtilsError get_error() {
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
        auto cw = pfw::make_writer_for_path(
            {cur_path, unordered() ? workers : 1, opts.member_size,
             BUFFER_HEADROOM_BYTES, opts.compress});
        writer = std::move(cw.writer);
        layout = cw.layout.layout;
        part_c = part_uc = part_lines = 0;
        if (co_await writer->open(cur_path, unordered() ? workers : 1,
                                  opts.compress, &scope) != 0) {
            set_error(ErrorCode::IO, "cannot open " + cur_path);
            co_return false;
        }
        for (auto& p : writer->output_paths()) created.push_back(p);
        co_return co_await write_extra(opts.part_header, true);
    }

    coro::CoroTask<bool> write_extra(const std::string& bytes, bool header) {
        if (bytes.empty()) co_return true;
        std::vector<std::uint8_t> comp;
        if (opts.compress) {
            auto c = take_compressor();
            const bool ok = c->valid() && c->compress_member_into(
                                              comp, bytes.data(), bytes.size());
            give_compressor(std::move(c));
            if (!ok) {
                set_error(ErrorCode::COMPRESSION, "member compression failed");
                co_return false;
            }
        }
        ByteView chunk = opts.compress ? ByteView(comp.data(), comp.size())
                                       : ByteView(bytes.data(), bytes.size());
        if (unordered()) {
            const int rc = header ? co_await writer->write_header(chunk)
                                  : co_await writer->write_footer(chunk);
            if (rc != 0) {
                set_error(ErrorCode::IO, "write failed for " + cur_path);
                co_return false;
            }
            summary.c_bytes += chunk.size();
            co_return true;
        }
        if (co_await writer->write_chunk(0, chunk) != 0) {
            set_error(ErrorCode::IO, "write failed for " + cur_path);
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
        if (ok && layout == pfw::FileLayout::SHARDED) {
            // GCC 12 destroys a temporary built inside co_await twice.
            const std::vector<std::string> shards = writer->output_paths();
            ok = co_await pfw::merge_shards(cur_path, shards) == 0;
        }
        writer.reset();
        if (!ok) {
            if (!failed())
                set_error(ErrorCode::IO, "cannot finish " + cur_path);
            co_return false;
        }
        if (opts.on_part) opts.on_part(part, cur_path, part_uc, part_lines);
        co_return true;
    }

    coro::CoroTask<void> write_member(Done& d) {
        if (!writer) {
            part = d.part;
            if (!co_await open_part()) co_return;
        }
        const bool gz = opts.compress;
        ByteView chunk = gz ? ByteView(d.comp.data(), d.comp.size())
                            : ByteView(d.plain.data(), d.plain.size());
        if (co_await writer->write_chunk(0, chunk) != 0) {
            set_error(ErrorCode::IO, "write failed for " + cur_path);
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
        if (d.ends_part && !co_await close_part()) co_return;
    }

    coro::CoroTask<void> write_unordered(std::size_t slot, Done& d) {
        ByteView chunk = opts.compress
                             ? ByteView(d.comp.data(), d.comp.size())
                             : ByteView(d.plain.data(), d.plain.size());
        if (co_await writer->write_chunk(slot, chunk) != 0) {
            set_error(ErrorCode::IO, "write failed for " + cur_path);
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

    std::unique_ptr<cmp::GzipMemberCompressor> take_compressor() {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!pool.empty()) {
                auto c = std::move(pool.back());
                pool.pop_back();
                return c;
            }
        }
        return std::make_unique<cmp::GzipMemberCompressor>(opts.level);
    }

    void give_compressor(std::unique_ptr<cmp::GzipMemberCompressor> c) {
        std::lock_guard<std::mutex> lk(mu);
        pool.push_back(std::move(c));
    }

    coro::CoroTask<void> run_job(std::shared_ptr<Job> job) {
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
        const std::string_view bytes = job->bytes();
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
                        set_error(ErrorCode::COMPRESSION,
                                  "member compression failed");
                } else {
                    if (job->raw)
                        d.plain.assign(bytes);
                    else
                        d.plain = std::move(job->data);
                }
            } catch (const std::exception& e) {
                set_error(ErrorCode::INTERNAL, e.what());
            } catch (...) {
                set_error(ErrorCode::INTERNAL, "unknown worker failure");
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

    coro::CoroTask<void> submit(std::string data,
                                std::unique_ptr<char[]> raw = nullptr,
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
        auto job = std::make_shared<Job>(
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
        scope.spawn([this, job](CoroScope&) -> coro::CoroTask<void> {
            co_await run_job(job);
        });
    }

    std::unique_ptr<char[]> scratch;  ///< ordered append_with, reused
    std::size_t scratch_cap = 0;

    coro::CoroTask<DFTUtilsError> fail() {
        co_await scope.join();
        if (!cleaned) {
            cleaned = true;
            if (writer) co_await writer->close();
            writer.reset();
            std::error_code ec;
            for (auto& p : created) fs::remove(p, ec);
        }
        co_return get_error();
    }
};

GzipLineWriter::GzipLineWriter() = default;
GzipLineWriter::GzipLineWriter(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
GzipLineWriter::GzipLineWriter(GzipLineWriter&&) noexcept = default;
GzipLineWriter& GzipLineWriter::operator=(GzipLineWriter&&) noexcept = default;
GzipLineWriter::~GzipLineWriter() = default;

coro::CoroTask<Result<GzipLineWriter>> GzipLineWriter::open(
    std::string path, GzipWriterOptions opts) {
    if (opts.member_size == 0)
        co_return make_error(ErrorCode::INVALID_ARGUMENT,
                             "member_size must be positive");
    if (!opts.ordered && (opts.on_member || opts.on_part || opts.part_name ||
                          opts.part_size > 0))
        co_return make_error(
            ErrorCode::INVALID_ARGUMENT,
            "unordered mode takes no on_member, on_part, part_size or "
            "part_name");
    const std::size_t workers =
        opts.workers ? opts.workers
                     : std::max<std::size_t>(1, available_parallelism());
    auto impl =
        std::make_unique<Impl>(std::move(path), std::move(opts), workers);
    if (!co_await impl->open_part()) co_return unexpected(impl->get_error());
    co_return GzipLineWriter(std::move(impl));
}

namespace {
// Takes every complete member out of `buf` (caller holds any lock on it).
std::vector<std::string> take_members(std::string& buf, std::size_t size) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (buf.size() > pos + size) {
        const std::size_t nl = buf.find('\n', pos + size);
        out.push_back(buf.substr(pos, nl + 1 - pos));
        pos = nl + 1;
    }
    buf.erase(0, pos);
    return out;
}
}  // namespace

coro::CoroTask<Result<void>> GzipLineWriter::append(std::string_view lines) {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    if (lines.empty()) co_return Result<void>{};
    if (lines.back() != '\n')
        co_return make_error(ErrorCode::INVALID_ARGUMENT,
                             "appended lines must end with a newline");
    std::vector<std::string> members;
    {
        std::lock_guard<std::mutex> lk(s.buf_mu);
        s.buf.append(lines);
        members = take_members(s.buf, s.opts.member_size);
    }
    for (auto& m : members) co_await s.submit(std::move(m));
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    co_return Result<void>{};
}

coro::CoroTask<Result<void>> GzipLineWriter::append_with(
    std::size_t max_bytes,
    std::function<std::size_t(char*, std::size_t)> fill) {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    const char* bad = "appended lines must end with a newline";
    if (s.unordered()) {
        const std::size_t held_units =
            co_await s.reserve(*s.prod_mem, s.prod_cap - s.member_units,
                               Impl::units_of(max_bytes));
        std::unique_ptr<char[]> own(new char[max_bytes]);
        const std::size_t n = fill(own.get(), max_bytes);
        if (n > max_bytes || (n > 0 && own[n - 1] != '\n')) {
            s.unreserve(*s.prod_mem, held_units);
            co_return make_error(ErrorCode::INVALID_ARGUMENT, bad);
        }
        if (n > 0) {
            // GCC 12 destroys a temporary built inside co_await twice.
            std::string none;
            co_await s.submit(std::move(none), std::move(own), n);
        }
        s.unreserve(*s.prod_mem, held_units);
    } else {
        if (s.scratch_cap < max_bytes) {
            s.scratch.reset(new char[max_bytes]);
            s.scratch_cap = max_bytes;
        }
        const std::size_t n = fill(s.scratch.get(), max_bytes);
        if (n > max_bytes || (n > 0 && s.scratch[n - 1] != '\n'))
            co_return make_error(ErrorCode::INVALID_ARGUMENT, bad);
        s.buf.append(s.scratch.get(), n);
        for (auto& m : take_members(s.buf, s.opts.member_size))
            co_await s.submit(std::move(m));
    }
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    co_return Result<void>{};
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

Result<GzipLineWriter::Producer> GzipLineWriter::producer() {
    if (!impl_->unordered())
        return make_error(ErrorCode::INVALID_ARGUMENT,
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

coro::CoroTask<Result<void>> GzipLineWriter::Producer::submit_full() {
    Impl& s = *impl_;
    auto members = take_members(buf_, s.opts.member_size);
    for (auto& m : members) co_await s.submit(std::move(m));
    release_extra();
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    co_return Result<void>{};
}

coro::CoroTask<Result<void>> GzipLineWriter::Producer::append(
    std::string_view lines) {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    if (lines.empty()) co_return Result<void>{};
    if (lines.back() != '\n')
        co_return make_error(ErrorCode::INVALID_ARGUMENT,
                             "appended lines must end with a newline");
    co_await reserve_for(lines.size());
    buf_.append(lines);
    co_return co_await submit_full();
}

coro::CoroTask<Result<void>> GzipLineWriter::Producer::append_with(
    std::size_t max_bytes,
    std::function<std::size_t(char*, std::size_t)> fill) {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    co_await reserve_for(max_bytes);
    const std::size_t old = buf_.size();
    buf_.resize(old + max_bytes);
    const std::size_t n = fill(buf_.data() + old, max_bytes);
    if (n > max_bytes || (n > 0 && buf_[old + n - 1] != '\n')) {
        buf_.resize(old);
        release_extra();
        co_return make_error(ErrorCode::INVALID_ARGUMENT,
                             "appended lines must end with a newline");
    }
    buf_.resize(old + n);
    co_return co_await submit_full();
}

coro::CoroTask<Result<void>> GzipLineWriter::Producer::flush() {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    if (buf_.empty()) co_return Result<void>{};
    std::string pending = std::move(buf_);
    buf_.clear();
    co_await s.submit(std::move(pending));
    release_extra();
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    co_return Result<void>{};
}

coro::CoroTask<Result<void>> GzipLineWriter::cut() {
    Impl& s = *impl_;
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    std::string pending;
    {
        std::lock_guard<std::mutex> lk(s.buf_mu);
        pending.swap(s.buf);
    }
    if (!pending.empty()) co_await s.submit(std::move(pending));
    co_return Result<void>{};
}

coro::CoroTask<Result<WriteSummary>> GzipLineWriter::close() {
    co_return co_await close(std::string_view{});
}

coro::CoroTask<Result<WriteSummary>> GzipLineWriter::close(
    std::string_view tail) {
    Impl& s = *impl_;
    if (s.cleaned) co_return unexpected(s.get_error());
    if (s.prod_held.load(std::memory_order_relaxed) != 0) {
        s.set_error(ErrorCode::INVALID_ARGUMENT,
                    "close with unflushed producers");
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    s.buf.append(tail);
    if (!s.failed() && !s.buf.empty()) co_await s.submit(std::move(s.buf));
    s.buf.clear();
    co_await s.scope.join();
    if (s.failed()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    s.summary.parts = s.writer ? s.part + 1 : s.part;
    if (!co_await s.close_part()) {
        DFTUtilsError err = co_await s.fail();
        co_return unexpected(std::move(err));
    }
    s.summary.peak_in_flight = s.peak.load(std::memory_order_relaxed);
    s.summary.peak_bytes =
        s.peak_held.load(std::memory_order_relaxed) * UNIT_BYTES;
    s.closed = true;
    co_return s.summary;
}

GzipLineWriterBlocking::GzipLineWriterBlocking(GzipLineWriter w)
    : w_(std::make_unique<GzipLineWriter>(std::move(w))) {}
GzipLineWriterBlocking::GzipLineWriterBlocking(
    GzipLineWriterBlocking&&) noexcept = default;
GzipLineWriterBlocking& GzipLineWriterBlocking::operator=(
    GzipLineWriterBlocking&&) noexcept = default;
GzipLineWriterBlocking::~GzipLineWriterBlocking() = default;

namespace {
template <typename T, typename F>
Result<T> run_on_default(const char* name, F&& f) {
    std::optional<Result<T>> out;
    default_runtime().run_blocking(name,
                                   [&](CoroScope&) -> coro::CoroTask<void> {
                                       out.emplace(co_await f());
                                       co_return;
                                   });
    return std::move(*out);
}
}  // namespace

Result<GzipLineWriterBlocking> GzipLineWriterBlocking::open(
    std::string path, GzipWriterOptions opts) {
    auto r = run_on_default<GzipLineWriter>("gzip_line_writer_open", [&] {
        return GzipLineWriter::open(std::move(path), std::move(opts));
    });
    if (!r) return unexpected(r.error());
    return GzipLineWriterBlocking(std::move(*r));
}

Result<void> GzipLineWriterBlocking::append(std::string_view lines) {
    return run_on_default<void>("gzip_line_writer_append",
                                [&] { return w_->append(lines); });
}

Result<void> GzipLineWriterBlocking::append_with(
    std::size_t max_bytes,
    std::function<std::size_t(char*, std::size_t)> fill) {
    return run_on_default<void>("gzip_line_writer_append_with", [&] {
        return w_->append_with(max_bytes, std::move(fill));
    });
}

Result<void> GzipLineWriterBlocking::cut() {
    return run_on_default<void>("gzip_line_writer_cut",
                                [&] { return w_->cut(); });
}

Result<WriteSummary> GzipLineWriterBlocking::close() {
    return close(std::string_view{});
}

Result<WriteSummary> GzipLineWriterBlocking::close(std::string_view tail) {
    return run_on_default<WriteSummary>("gzip_line_writer_close",
                                        [&] { return w_->close(tail); });
}

}  // namespace dftracer::utils::utilities::fileio
