#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_config.h>
#include <dftracer/utils/index/schemas/dft/agg/aggregation_serialization.h>
#include <dftracer/utils/index/schemas/dft/agg/association_tracker.h>
#include <dftracer/utils/index/schemas/dft/agg/dft_trace_writer.h>
#include <dftracer/utils/index/schemas/dft/agg/event_aggregator.h>
#include <dftracer/utils/index/store/column_families.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/fileio/gzip_line_writer.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <fcntl.h>

#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

namespace dftracer::utils::index::schemas::dft::agg {

namespace {

class JsonBuffer {
   public:
    explicit JsonBuffer(std::size_t capacity)
        : owned_(std::make_unique<char[]>(capacity)),
          data_(owned_.get()),
          capacity_(capacity),
          size_(0) {}

    JsonBuffer(char* dst, std::size_t capacity)
        : data_(dst), capacity_(capacity), size_(0) {}

    void append(const char* ptr, std::size_t len) {
        std::memcpy(data_ + size_, ptr, len);
        size_ += len;
    }

    void append(std::string_view sv) { append(sv.data(), sv.size()); }

    void push_back(char c) { data_[size_++] = c; }

    template <std::size_t N>
    void append_literal(const char (&lit)[N]) {
        append(lit, N - 1);
    }

    void append_u64(std::uint64_t v) {
        auto res = std::to_chars(data_ + size_, data_ + capacity_, v);
        size_ = static_cast<std::size_t>(res.ptr - data_);
    }

    void append_i64(std::int64_t v) {
        auto res = std::to_chars(data_ + size_, data_ + capacity_, v);
        size_ = static_cast<std::size_t>(res.ptr - data_);
    }

    void append_double(double value) {
        int n;
        if (std::abs(value - std::round(value)) < 1e-9) {
            n = std::snprintf(data_ + size_, capacity_ - size_, "%lld",
                              static_cast<long long>(std::round(value)));
        } else {
            n = std::snprintf(data_ + size_, capacity_ - size_, "%.2f", value);
        }
        size_ += static_cast<std::size_t>(n);
    }

    int format(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(data_ + size_, capacity_ - size_, fmt, ap);
        va_end(ap);
        size_ += static_cast<std::size_t>(n);
        return n;
    }

    void append_json_escaped(std::string_view str) {
        const char* start = str.data();
        const char* end = start + str.size();
        const char* safe_start = start;
        for (const char* p = start; p < end; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c >= 32 && c < 127 && c != '"' && c != '\\') continue;
            if (p > safe_start) {
                append(safe_start, static_cast<std::size_t>(p - safe_start));
            }
            switch (c) {
                case '"':
                    append_literal("\\\"");
                    break;
                case '\\':
                    append_literal("\\\\");
                    break;
                case '\b':
                    append_literal("\\b");
                    break;
                case '\f':
                    append_literal("\\f");
                    break;
                case '\n':
                    append_literal("\\n");
                    break;
                case '\r':
                    append_literal("\\r");
                    break;
                case '\t':
                    append_literal("\\t");
                    break;
                default:
                    format("\\u%04x", c);
                    break;
            }
            safe_start = p + 1;
        }
        if (end > safe_start) {
            append(safe_start, static_cast<std::size_t>(end - safe_start));
        }
    }

    const char* data() const { return data_; }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    std::size_t remaining() const { return capacity_ - size_; }
    bool empty() const { return size_ == 0; }
    void clear() { size_ = 0; }
    ByteView view() const { return ByteView(data_, size_); }

   private:
    std::unique_ptr<char[]> owned_;
    char* data_;
    std::size_t capacity_;
    std::size_t size_;
};

using dftracer::utils::utilities::common::serialization::BinaryReader;

inline void emit_metric_stats_from_bytes(BinaryReader& r,
                                         std::string_view prefix,
                                         bool compute_statistics,
                                         JsonBuffer& buf) {
    auto fmt = r.u8();
    if (fmt == METRIC_FMT_COMPACT) {
        auto val = r.varint();
        if (val == 0) return;
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_sum\":");
        buf.append_u64(val);
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_min\":");
        buf.append_u64(val);
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_max\":");
        buf.append_u64(val);
        return;
    }
    auto count = r.varint();
    auto total = r.varint();
    auto min = r.varint();
    auto max = r.varint();
    r.skip(16);  // mean, mean_lo
    auto m2 = r.f64();
    r.skip(16);  // m3, m4
    if (fmt == METRIC_FMT_FULL_WITH_SKETCH) {
        r.skip_blob();
    }

    buf.append_literal(",\"");
    buf.append(prefix);
    buf.append_literal("_sum\":");
    buf.append_u64(total);

    if (min != std::numeric_limits<std::uint64_t>::max()) {
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_min\":");
        buf.append_u64(min);
    }
    if (max > 0) {
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_max\":");
        buf.append_u64(max);
    }

    if (compute_statistics && count >= 2) {
        // `m2` is the central second moment, sum (x - mean)^2: the sample
        // variance is that over n - 1. Clamp at zero for rounding.
        const double n = static_cast<double>(count);
        const double var = (m2 > 0.0 ? m2 : 0.0) / (n - 1.0);
        const double stddev = var > 0.0 ? std::sqrt(var) : 0.0;
        buf.append_literal(",\"");
        buf.append(prefix);
        buf.append_literal("_std\":");
        buf.append_double(stddev);
    }
}

inline void skip_metric_stats(BinaryReader& r) {
    auto fmt = r.u8();
    if (fmt == METRIC_FMT_COMPACT) {
        r.varint();
        return;
    }
    // FULL: count, total, min, max (varints), 5 doubles (mean, mean_lo, m2,
    // m3, m4)
    r.varint();
    r.varint();
    r.varint();
    r.varint();
    r.skip(40);
    if (fmt == METRIC_FMT_FULL_WITH_SKETCH) r.skip_blob();
}

coro::CoroTask<bool> write_shard_events(
    std::uint16_t shard_begin, std::uint16_t shard_end,
    std::size_t flush_threshold, std::size_t buffer_capacity,
    utilities::fileio::GzipLineWriter* writer,
    const DftracerTraceWriterInput* input) {
    std::size_t local_keys = 0;
    std::string after;
    bool more = true;

    auto fill = [&](char* dst, std::size_t cap) -> std::size_t {
        JsonBuffer buf(dst, cap);
        more = false;
        input->aggregator->scan_shard_range_raw(
            shard_begin, shard_end,
            [&](std::string_view key_bytes, std::string_view value_bytes) {
                local_keys++;

                // Layout: shard(2) map_type(1) cat(varint ID) name(varint ID)
                //         pid(varint) tid(varint) hhash(varint ID)
                //         fhash(8 raw bytes if inline else varint ID)
                //         time_bucket(varint) num_extra(2)
                //         [k(varint ID) v(varint ID)]*
                auto& intern = input->aggregator->intern();
                BinaryReader kr(key_bytes);
                kr.skip(2);  // shard
                const std::uint8_t type_byte = kr.u8();
                const bool fhash_inline =
                    (type_byte & AGG_KEY_FHASH_INLINE) != 0;
                auto cat =
                    intern.resolve(static_cast<std::uint32_t>(kr.varint()));
                auto name =
                    intern.resolve(static_cast<std::uint32_t>(kr.varint()));
                auto pid = kr.varint();
                auto tid = kr.varint();
                auto hhash_id = static_cast<std::uint32_t>(kr.varint());
                auto hhash =
                    hhash_id ? intern.resolve(hhash_id) : std::string_view{};
                char fbuf[::dftracer::utils::hash::HEX64_DIGITS];
                std::string_view fhash;
                if (fhash_inline) {
                    std::uint64_t fh = kr.be64();
                    if (fh != 0) {
                        ::dftracer::utils::hash::format_hex64(fh, fbuf);
                        fhash = std::string_view(fbuf, sizeof(fbuf));
                    }
                } else {
                    auto fhash_id = static_cast<std::uint32_t>(kr.varint());
                    fhash = fhash_id ? intern.resolve(fhash_id)
                                     : std::string_view{};
                }
                auto time_bucket = kr.varint();
                auto num_extra = kr.be16();

                // For REGULAR, pre-parse ts/te by skipping through value bytes.
                std::uint64_t regular_ts = 0, regular_te = 0;
                if (input->format == TraceEventFormat::REGULAR) {
                    BinaryReader tmp(value_bytes);
                    tmp.varint();            // count
                    skip_metric_stats(tmp);  // duration
                    skip_metric_stats(tmp);  // size
                    skip_metric_stats(tmp);  // offset
                    regular_ts = tmp.varint();
                    regular_te = tmp.varint();
                }

                const int type_int =
                    trace::event_type_to_int(trace::event_type_from_cat(cat));
                if (input->format == TraceEventFormat::REGULAR) {
                    // Expand back to a timed COMPLETE event (ts + dur).
                    std::uint64_t duration = regular_te - regular_ts;
                    buf.append_literal("{\"name\":\"");
                    buf.append_json_escaped(name);
                    buf.append_literal("\",\"cat\":\"");
                    buf.append_json_escaped(cat);
                    buf.append_literal("\",\"ts\":");
                    buf.append_u64(regular_ts);
                    buf.append_literal(",\"dur\":");
                    buf.append_u64(duration);
                    buf.append_literal(",\"ph\":");
                    buf.append_u64(
                        trace::phase_to_int(trace::RecordPhase::COMPLETE));
                    buf.append_literal(",\"type\":");
                    buf.append_i64(type_int);
                    buf.append_literal(",\"pid\":");
                    buf.append_u64(pid);
                    buf.append_literal(",\"tid\":");
                    buf.append_u64(tid);
                    buf.append_literal(",\"args\":{");
                } else {
                    // AGGREGATED (ph:3) folds many events into one record at
                    // the bucket time; COUNTER (ph:2) renders the same shape as
                    // a counter sample. Both are instantaneous (no dur).
                    trace::RecordPhase rp =
                        input->format == TraceEventFormat::COUNTER
                            ? trace::RecordPhase::COUNTER
                            : trace::RecordPhase::AGGREGATED;
                    buf.append_literal("{\"name\":\"");
                    buf.append_json_escaped(name);
                    buf.append_literal("\",\"cat\":\"");
                    buf.append_json_escaped(cat);
                    buf.append_literal("\",\"ts\":");
                    buf.append_u64(time_bucket);
                    buf.append_literal(",\"ph\":");
                    buf.append_u64(trace::phase_to_int(rp));
                    buf.append_literal(",\"type\":");
                    buf.append_i64(type_int);
                    buf.append_literal(",\"pid\":");
                    buf.append_u64(pid);
                    buf.append_literal(",\"tid\":");
                    buf.append_u64(tid);
                    buf.append_literal(",\"args\":{");
                }

                buf.append_literal("\"hhash\":\"");
                buf.append_json_escaped(hhash);
                buf.append_literal("\"");

                if (!fhash.empty()) {
                    buf.append_literal(",\"fhash\":\"");
                    buf.append_json_escaped(fhash);
                    buf.append_literal("\"");
                }

                for (std::uint16_t i = 0; i < num_extra; ++i) {
                    auto ek =
                        intern.resolve(static_cast<std::uint32_t>(kr.varint()));
                    auto ev =
                        intern.resolve(static_cast<std::uint32_t>(kr.varint()));
                    buf.append_literal(",\"");
                    buf.append_json_escaped(ek);
                    buf.append_literal("\":\"");
                    buf.append_json_escaped(ev);
                    buf.append_literal("\"");
                }

                // Value bytes: count, dur, size, offset, ts, te, parent_pid,
                // num_custom, customs, distinct_sketch
                BinaryReader vr(value_bytes);
                auto count = vr.varint();

                buf.append_literal(",\"dftu_cnt\":");
                buf.append_u64(count);

                emit_metric_stats_from_bytes(vr, "dur",
                                             input->compute_statistics, buf);
                emit_metric_stats_from_bytes(vr, "ret",
                                             input->compute_statistics, buf);
                emit_metric_stats_from_bytes(vr, "offset",
                                             input->compute_statistics, buf);

                auto m_ts = vr.varint();
                auto m_te = vr.varint();
                auto m_parent_pid = vr.varint();

                // Custom metrics come AFTER ts/te in the stream but BEFORE
                // ts/te in the JSON output order, so emit them now.
                auto num_custom = vr.varint();
                for (std::uint64_t i = 0; i < num_custom; ++i) {
                    auto cname = vr.str();
                    emit_metric_stats_from_bytes(
                        vr, cname, input->compute_statistics, buf);
                }

                buf.append_literal(",\"ts\":");
                buf.append_u64(m_ts);
                buf.append_literal(",\"te\":");
                buf.append_u64(m_te);

                std::uint64_t effective_parent = m_parent_pid;
                if (input->tracker && input->agg_config &&
                    input->agg_config->track_process_parents &&
                    input->tracker->has_process_tree()) {
                    auto pp = input->tracker->get_parent_pid(pid);
                    if (pp != 0) effective_parent = pp;
                }

                // Boundary associations (emitted between ts/te and parent_pid)
                if (input->tracker && input->agg_config &&
                    !input->agg_config->boundary_events.empty() &&
                    input->tracker->has_boundary_events()) {
                    auto mid = (m_ts + m_te) / 2;
                    auto bpid = effective_parent > 0 ? effective_parent : pid;
                    auto assoc =
                        input->tracker->get_boundary_associations(bpid, mid);
                    for (const auto& [an, av] : assoc) {
                        buf.append_literal(",\"");
                        buf.append_json_escaped(an);
                        buf.append_literal("\":\"");
                        buf.append_json_escaped(av);
                        buf.append_literal("\"");
                    }
                }

                if (effective_parent > 0) {
                    buf.append_literal(",\"parent_pid\":");
                    buf.append_u64(effective_parent);
                }

                buf.append_literal("}}\n");

                if (buf.size() >= flush_threshold) {
                    after.assign(key_bytes);
                    more = true;
                    return false;
                }
                return true;
            },
            after);
        return buf.size();
    };

    while (more) {
        if (!(co_await writer->append_with(buffer_capacity, fill))) {
            co_return false;
        }
    }

    if (input->keys_written) {
        input->keys_written->fetch_add(local_keys, std::memory_order_relaxed);
    }
    co_return true;
}

}  // namespace

coro::CoroTask<bool> DftTraceWriter::operator()(
    CoroScope& ctx, const DftracerTraceWriterInput& input) const {
    DFTRACER_UTILS_TRACE_SCOPE("write perfetto");
    using namespace dftracer::utils::utilities;

    constexpr std::size_t HEADER_BUFFER_BYTES = 4 * 1024 * 1024;
    constexpr std::size_t FLUSH_BYTES = 12 * 1024 * 1024;
    constexpr std::size_t BUFFER_HEADROOM_BYTES = 4 * 1024 * 1024;

    const std::size_t baseline = std::min<std::size_t>(
        ctx.get_executor()->get_num_threads(), AGG_KEY_NUM_SHARDS);
    const auto sizing = utilities::fileio::parallel::make_writer_for_path(
                            {input.output_path, baseline, FLUSH_BYTES,
                             BUFFER_HEADROOM_BYTES, input.compress})
                            .sizing;
    const std::size_t num_workers =
        std::max<std::size_t>(1, sizing.num_workers);
    const std::size_t flush_threshold = sizing.flush_threshold;
    const std::size_t buffer_capacity = sizing.buffer_capacity;

    JsonBuffer header(HEADER_BUFFER_BYTES);
    if (input.emit_header) header.append_literal("[\n");

    if (input.emit_header &&
        (input.trace_duration > 0 || !input.boundary_ranges.empty())) {
        header.append_literal(
            "{\"name\":\"trace_metadata\",\"cat\":\"metadata\",\"ph\":4,"
            "\"type\""
            ":1,\"args\":{");
        header.format("\"trace_duration\":%llu",
                      static_cast<unsigned long long>(input.trace_duration));

        if (!input.boundary_ranges.empty()) {
            header.append_literal(",\"boundary_ranges\":{");
            bool first_boundary = true;

            for (const auto& [boundary_name, value_map] :
                 input.boundary_ranges) {
                if (!first_boundary) header.append_literal(",");
                first_boundary = false;
                header.append_literal("\"");
                header.append_json_escaped(boundary_name);
                header.append_literal("\":{");
                bool first_value = true;
                for (const auto& [value, time_range] : value_map) {
                    if (!first_value) header.append_literal(",");
                    first_value = false;
                    header.append_literal("\"");
                    header.append_json_escaped(value);
                    header.append_literal("\":{");
                    header.format(
                        "\"ts\":%llu,\"te\":%llu",
                        static_cast<unsigned long long>(time_range.ts),
                        static_cast<unsigned long long>(time_range.te));
                    header.append_literal("}");
                }
                header.append_literal("}");
            }
            header.append_literal("}");
        }
        header.append_literal("}}\n");
    }

    if (input.emit_header) {
        for (std::uint64_t pid : input.root_pids) {
            header.format(
                "{\"name\":\"root_process\",\"cat\":\"dftracer\",\"ph\":4,"
                "\"type\":1,\"pid\":%llu,\"tid\":%llu,"
                "\"args\":{\"is_root\":\"true\"}}\n",
                static_cast<unsigned long long>(pid),
                static_cast<unsigned long long>(pid));
        }
    }

    utilities::fileio::GzipWriterOptions opts;
    opts.member_size = flush_threshold;
    opts.level = input.compression_level < 0 ? 6 : input.compression_level;
    opts.workers = num_workers;
    opts.compress = input.compress;
    opts.ordered = false;
    opts.part_header.assign(header.data(), header.size());
    if (input.emit_footer) opts.part_footer = "]\n";
    auto opened = co_await utilities::fileio::GzipLineWriter::open(
        input.output_path, std::move(opts));
    if (!opened) co_return false;
    auto writer = std::move(*opened);

    std::atomic<bool> worker_success{true};
    const std::uint16_t range_begin = input.shard_begin;
    const std::uint16_t range_end =
        input.shard_end == 0 ? AGG_KEY_NUM_SHARDS : input.shard_end;
    const std::uint16_t range_width =
        range_end > range_begin
            ? static_cast<std::uint16_t>(range_end - range_begin)
            : std::uint16_t{0};
    std::uint16_t shards_per_worker =
        static_cast<std::uint16_t>(range_width / num_workers);

    co_await ctx.scope([&](CoroScope& child) -> coro::CoroTask<void> {
        for (std::size_t i = 0; i < num_workers; ++i) {
            auto shard_begin =
                static_cast<std::uint16_t>(range_begin + i * shards_per_worker);
            auto shard_end =
                (i + 1 == num_workers)
                    ? range_end
                    : static_cast<std::uint16_t>(range_begin +
                                                 (i + 1) * shards_per_worker);
            const auto* input_ptr = &input;
            auto* success_ptr = &worker_success;
            auto* writer_ptr = &writer;
            child.spawn([shard_begin, shard_end, flush_threshold,
                         buffer_capacity, writer_ptr, input_ptr,
                         success_ptr](CoroScope&) -> coro::CoroTask<void> {
                auto ok = co_await write_shard_events(
                    shard_begin, shard_end, flush_threshold, buffer_capacity,
                    writer_ptr, input_ptr);
                if (!ok) success_ptr->store(false);
            });
        }
        co_return;
    });

    if (!worker_success.load()) {
        co_await writer.close();
        co_return false;
    }

    co_return static_cast<bool>(co_await writer.close());
}

}  // namespace dftracer::utils::index::schemas::dft::agg
