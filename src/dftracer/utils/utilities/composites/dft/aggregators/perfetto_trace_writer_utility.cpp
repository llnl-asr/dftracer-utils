#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/io/io.h>
#include <dftracer/utils/core/rocksdb/column_families.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/tasks/coro_scope.h>
#include <dftracer/utils/utilities/common/serialization/binary_codec.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_config.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/aggregation_serialization.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/association_tracker.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/event_aggregator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/perfetto_trace_writer_utility.h>
#include <dftracer/utils/utilities/compression/zlib/streaming_compressor_utility.h>
#include <dftracer/utils/utilities/fileio/parallel/layout.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/fileio/streaming_file_writer_utility.h>
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

namespace dftracer::utils::utilities::composites::dft::aggregators {

namespace {

class JsonBuffer {
   public:
    explicit JsonBuffer(std::size_t capacity)
        : data_(std::make_unique<char[]>(capacity)),
          capacity_(capacity),
          size_(0) {}

    void append(const char* ptr, std::size_t len) {
        std::memcpy(data_.get() + size_, ptr, len);
        size_ += len;
    }

    void append(std::string_view sv) { append(sv.data(), sv.size()); }

    void push_back(char c) { data_[size_++] = c; }

    template <std::size_t N>
    void append_literal(const char (&lit)[N]) {
        append(lit, N - 1);
    }

    void append_u64(std::uint64_t v) {
        auto res =
            std::to_chars(data_.get() + size_, data_.get() + capacity_, v);
        size_ = static_cast<std::size_t>(res.ptr - data_.get());
    }

    void append_i64(std::int64_t v) {
        auto res =
            std::to_chars(data_.get() + size_, data_.get() + capacity_, v);
        size_ = static_cast<std::size_t>(res.ptr - data_.get());
    }

    void append_double(double value) {
        int n;
        if (std::abs(value - std::round(value)) < 1e-9) {
            n = std::snprintf(data_.get() + size_, capacity_ - size_, "%lld",
                              static_cast<long long>(std::round(value)));
        } else {
            n = std::snprintf(data_.get() + size_, capacity_ - size_, "%.2f",
                              value);
        }
        size_ += static_cast<std::size_t>(n);
    }

    int format(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        int n = std::vsnprintf(data_.get() + size_, capacity_ - size_, fmt, ap);
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

    const char* data() const { return data_.get(); }
    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    std::size_t remaining() const { return capacity_ - size_; }
    bool empty() const { return size_ == 0; }
    void clear() { size_ = 0; }
    ByteView view() const { return ByteView(data_.get(), size_); }

   private:
    std::unique_ptr<char[]> data_;
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
    // FULL or FULL_WITH_SKETCH
    auto count = r.varint();
    auto total = r.varint();
    auto min = r.varint();
    auto max = r.varint();
    (void)r.f64();  // mean
    auto m2 = r.f64();
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
        // `m2` holds the raw power sum
        // `sum_x^2`, not Welford's central M2. Convert to central
        // moment then to sample variance. Clamp at zero for float
        // cancellation.
        const double n = static_cast<double>(count);
        const double sum_x = static_cast<double>(total);
        const double central = m2 - sum_x * sum_x / n;
        const double var = (central > 0.0 ? central : 0.0) / (n - 1.0);
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
    // FULL: count, total, min, max (varints), 2 doubles (mean, m2)
    r.varint();
    r.varint();
    r.varint();
    r.varint();
    r.skip(16);
    if (fmt == METRIC_FMT_FULL_WITH_SKETCH) r.skip_blob();
}

// Compress `data` into a standalone gzip member so the result can be written
// at any offset in a concatenated-gzip file.
coro::CoroTask<bool> compress_to_gzip_member(int level, ByteView data,
                                             std::vector<unsigned char>& out) {
    out.clear();
    compression::zlib::ManualStreamingCompressorUtility comp(
        level, compression::zlib::CompressionFormat::GZIP);
    if (data.size() > 0) {
        auto gen = comp.compress(data);
        while (auto view = co_await gen.next()) {
            const auto* p =
                reinterpret_cast<const unsigned char*>(view->data());
            out.insert(out.end(), p, p + view->size());
        }
    }
    auto fin = comp.finalize_stream();
    while (auto view = co_await fin.next()) {
        const auto* p = reinterpret_cast<const unsigned char*>(view->data());
        out.insert(out.end(), p, p + view->size());
    }
    co_return true;
}

coro::CoroTask<bool> write_shard_events(
    std::size_t worker_idx, std::uint16_t shard_begin, std::uint16_t shard_end,
    std::size_t flush_threshold, std::size_t buffer_capacity,
    fileio::parallel::ParallelWriter* writer,
    const PerfettoTraceWriterInput* input) {
    using namespace dftracer::utils::utilities;

    JsonBuffer buf(buffer_capacity);
    std::vector<unsigned char> compressed;

    auto flush_buffer = [&]() -> coro::CoroTask<int> {
        if (buf.empty()) co_return 0;
        int rc;
        if (input->compress) {
            co_await compress_to_gzip_member(input->compression_level,
                                             buf.view(), compressed);
            rc = co_await writer->write_chunk(
                worker_idx,
                ByteView(reinterpret_cast<const char*>(compressed.data()),
                         compressed.size()));
        } else {
            rc = co_await writer->write_chunk(worker_idx, buf.view());
        }
        buf.clear();
        co_return rc;
    };

    std::size_t local_keys = 0;
    std::vector<std::string> pending_chunks;
    input->aggregator->scan_shard_range_raw(
        shard_begin, shard_end,
        [&](std::string_view key_bytes, std::string_view value_bytes) {
            local_keys++;

            // Layout: shard(2) map_type(1) cat(varint ID) name(varint ID)
            //         pid(varint) tid(varint) hhash(varint ID) fhash(varint ID)
            //         time_bucket(varint) num_extra(2) [k(varint ID) v(varint
            //         ID)]*
            auto& intern = aggregation_intern();
            BinaryReader kr(key_bytes);
            kr.skip(2);     // shard
            (void)kr.u8();  // map_type
            auto cat = intern.resolve(static_cast<std::uint32_t>(kr.varint()));
            auto name = intern.resolve(static_cast<std::uint32_t>(kr.varint()));
            auto pid = kr.varint();
            auto tid = kr.varint();
            auto hhash_id = static_cast<std::uint32_t>(kr.varint());
            auto hhash =
                hhash_id ? intern.resolve(hhash_id) : std::string_view{};
            auto fhash_id = static_cast<std::uint32_t>(kr.varint());
            auto fhash =
                fhash_id ? intern.resolve(fhash_id) : std::string_view{};
            auto time_bucket = kr.varint();
            auto num_extra = kr.be16();

            // For REGULAR, pre-parse ts/te by skipping through value bytes.
            std::uint64_t regular_ts = 0, regular_te = 0;
            if (input->format == PerfettoEventFormat::REGULAR) {
                BinaryReader tmp(value_bytes);
                tmp.varint();            // count
                skip_metric_stats(tmp);  // duration
                skip_metric_stats(tmp);  // size
                regular_ts = tmp.varint();
                regular_te = tmp.varint();
            }

            // Emit event header
            if (input->format == PerfettoEventFormat::COUNTER) {
                buf.append_literal("{\"name\":\"");
                buf.append_json_escaped(name);
                buf.append_literal("\",\"cat\":\"");
                buf.append_json_escaped(cat);
                buf.append_literal("\",\"ts\":");
                buf.append_u64(time_bucket);
                buf.append_literal(",\"ph\":\"C\",\"pid\":");
                buf.append_u64(pid);
                buf.append_literal(",\"tid\":");
                buf.append_u64(tid);
                buf.append_literal(",\"args\":{");
            } else if (input->format == PerfettoEventFormat::REGULAR) {
                std::uint64_t duration = regular_te - regular_ts;
                buf.append_literal("{\"name\":\"");
                buf.append_json_escaped(name);
                buf.append_literal("\",\"cat\":\"");
                buf.append_json_escaped(cat);
                buf.append_literal("\",\"ts\":");
                buf.append_u64(regular_ts);
                buf.append_literal(",\"dur\":");
                buf.append_u64(duration);
                buf.append_literal(",\"ph\":\"X\",\"pid\":");
                buf.append_u64(pid);
                buf.append_literal(",\"tid\":");
                buf.append_u64(tid);
                buf.append_literal(",\"args\":{");
            }

            // hhash
            buf.append_literal("\"hhash\":\"");
            buf.append_json_escaped(hhash);
            buf.append_literal("\"");

            // fhash
            if (!fhash.empty()) {
                buf.append_literal(",\"fhash\":\"");
                buf.append_json_escaped(fhash);
                buf.append_literal("\"");
            }

            // extra keys (varint intern IDs)
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

            // Value bytes: count, dur, size, ts, te, parent_pid, num_custom,
            // customs
            BinaryReader vr(value_bytes);
            auto count = vr.varint();

            buf.append_literal(",\"dft_cnt\":");
            buf.append_u64(count);

            emit_metric_stats_from_bytes(vr, "dur", input->compute_statistics,
                                         buf);
            emit_metric_stats_from_bytes(vr, "ret", input->compute_statistics,
                                         buf);

            auto m_ts = vr.varint();
            auto m_te = vr.varint();
            auto m_parent_pid = vr.varint();

            // Custom metrics come AFTER ts/te in the stream but BEFORE ts/te
            // in the JSON output order, so emit them now.
            auto num_custom = vr.varint();
            for (std::uint64_t i = 0; i < num_custom; ++i) {
                auto cname = vr.str();
                emit_metric_stats_from_bytes(vr, cname,
                                             input->compute_statistics, buf);
            }

            buf.append_literal(",\"ts\":");
            buf.append_u64(m_ts);
            buf.append_literal(",\"te\":");
            buf.append_u64(m_te);

            // Compute effective parent_pid (tracker may override)
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
                pending_chunks.emplace_back(buf.data(), buf.size());
                buf.clear();
            }
            return true;
        });

    for (auto& s : pending_chunks) {
        if (input->compress) {
            co_await compress_to_gzip_member(
                input->compression_level,
                ByteView(reinterpret_cast<const std::byte*>(s.data()),
                         s.size()),
                compressed);
            auto rc = co_await writer->write_chunk(
                worker_idx,
                ByteView(reinterpret_cast<const std::byte*>(compressed.data()),
                         compressed.size()));
            if (rc != 0) co_return false;
        } else {
            auto rc = co_await writer->write_chunk(
                worker_idx,
                ByteView(reinterpret_cast<const std::byte*>(s.data()),
                         s.size()));
            if (rc != 0) co_return false;
        }
    }

    if (co_await flush_buffer() != 0) co_return false;

    if (input->keys_written) {
        input->keys_written->fetch_add(local_keys, std::memory_order_relaxed);
    }
    co_return true;
}

}  // namespace

coro::CoroTask<bool> PerfettoTraceWriterUtility::process(
    const PerfettoTraceWriterInput& input) {
    DFTRACER_UTILS_TRACE_SCOPE("write perfetto");
    using namespace dftracer::utils::utilities;

    constexpr std::size_t HEADER_BUFFER_BYTES = 4 * 1024 * 1024;
    constexpr std::size_t DEFAULT_FLUSH_BYTES = 12 * 1024 * 1024;
    constexpr std::size_t BUFFER_HEADROOM_BYTES = 4 * 1024 * 1024;

    auto layout_info = fileio::parallel::detect_layout(input.output_path);
    const std::size_t executor_threads =
        this->context().get_executor()->get_num_threads();
    const std::size_t baseline =
        std::min<std::size_t>(executor_threads, AGG_KEY_NUM_SHARDS);
    // Mirror make_writer's padded-layout gate so sizing picks the matching
    // flush_threshold.
    const bool uses_padded =
        layout_info.layout == fileio::parallel::FileLayout::STRIPED &&
        input.compress &&
        layout_info.stripe_size >= fileio::parallel::MIN_PADDED_STRIPE_BYTES;
    const auto sizing = fileio::parallel::compute_writer_sizing(
        layout_info, baseline, DEFAULT_FLUSH_BYTES, BUFFER_HEADROOM_BYTES,
        uses_padded);
    const std::size_t num_workers = sizing.num_workers;
    const std::size_t flush_threshold = sizing.flush_threshold;
    const std::size_t buffer_capacity = sizing.buffer_capacity;
    fileio::parallel::WriterConfig wcfg;
    wcfg.layout = layout_info.layout;
    wcfg.stripe_size = layout_info.stripe_size;
    wcfg.gzip = input.compress;
    auto writer = fileio::parallel::make_writer(wcfg);
    if (co_await writer->open(input.output_path, num_workers, input.compress,
                              &this->context()) != 0) {
        co_return false;
    }

    auto write_section = [&](ByteView data,
                             bool is_footer) -> coro::CoroTask<bool> {
        std::vector<unsigned char> compressed;
        ByteView payload = data;
        if (input.compress) {
            co_await compress_to_gzip_member(input.compression_level, data,
                                             compressed);
            payload =
                ByteView(reinterpret_cast<const std::byte*>(compressed.data()),
                         compressed.size());
        }
        int rc = is_footer ? co_await writer->write_footer(payload)
                           : co_await writer->write_header(payload);
        co_return rc == 0;
    };

    JsonBuffer header(HEADER_BUFFER_BYTES);
    if (input.emit_header) header.append_literal("[\n");

    if (input.emit_header &&
        (input.trace_duration > 0 || !input.boundary_ranges.empty())) {
        header.append_literal(
            "{\"name\":\"trace_metadata\",\"cat\":\"metadata\",\"ph\":"
            "\"M\",\"args\":{");
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
                "{\"name\":\"root_process\",\"cat\":\"dftracer\",\"ph\":"
                "\"M\",\"pid\":%llu,\"tid\":%llu,"
                "\"args\":{\"is_root\":\"true\"}}\n",
                static_cast<unsigned long long>(pid),
                static_cast<unsigned long long>(pid));
        }
    }

    if (!co_await write_section(header.view(), false)) co_return false;

    std::atomic<bool> worker_success{true};
    const std::uint16_t range_begin = input.shard_begin;
    const std::uint16_t range_end =
        input.shard_end == 0 ? AGG_KEY_NUM_SHARDS : input.shard_end;
    const std::uint16_t range_width =
        range_end > range_begin
            ? static_cast<std::uint16_t>(range_end - range_begin)
            : std::uint16_t{0};
    std::uint16_t shards_per_worker =
        num_workers > 0 ? static_cast<std::uint16_t>(range_width / num_workers)
                        : std::uint16_t{0};

    co_await this->context().scope(
        [&](CoroScope& child) -> coro::CoroTask<void> {
            for (std::size_t i = 0; i < num_workers; ++i) {
                auto shard_begin = static_cast<std::uint16_t>(
                    range_begin + i * shards_per_worker);
                auto shard_end =
                    (i + 1 == num_workers)
                        ? range_end
                        : static_cast<std::uint16_t>(
                              range_begin + (i + 1) * shards_per_worker);
                const auto* input_ptr = &input;
                auto* success_ptr = &worker_success;
                auto* writer_ptr = writer.get();
                child.spawn([i, shard_begin, shard_end, flush_threshold,
                             buffer_capacity, writer_ptr, input_ptr,
                             success_ptr](CoroScope&) -> coro::CoroTask<void> {
                    auto ok = co_await write_shard_events(
                        i, shard_begin, shard_end, flush_threshold,
                        buffer_capacity, writer_ptr, input_ptr);
                    if (!ok) success_ptr->store(false);
                });
            }
            co_return;
        });

    if (!worker_success.load()) {
        co_await writer->close();
        co_return false;
    }

    if (input.emit_footer) {
        const char footer[] = "]\n";
        if (!co_await write_section(
                ByteView(reinterpret_cast<const std::byte*>(footer), 2), true))
            co_return false;
    }

    if (co_await writer->close() != 0) co_return false;

    if (input.merge_on_sharded &&
        layout_info.layout == fileio::parallel::FileLayout::SHARDED) {
        auto shards = writer->output_paths();
        if (co_await fileio::parallel::merge_shards(input.output_path,
                                                    shards) != 0) {
            co_return false;
        }
    }

    co_return true;
}

}  // namespace dftracer::utils::utilities::composites::dft::aggregators
