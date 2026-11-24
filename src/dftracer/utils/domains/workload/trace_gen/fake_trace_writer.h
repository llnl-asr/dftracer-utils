// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_DOMAINS_WORKLOAD_TRACE_GEN_FAKE_TRACE_WRITER_H
#define DFTRACER_UTILS_DOMAINS_WORKLOAD_TRACE_GEN_FAKE_TRACE_WRITER_H

#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/fileio/hash/hasher_utility.h>
#include <dftracer/utils/trace/schema.h>
#include <dftracer/utils/trace/writer/trace_writer.h>

#include <cstdint>
#include <random>

/// Synthetic chrome/Perfetto trace emission (gzip .pfw.gz). A production
/// utility for generating trace files with known patterns; consumed by the
/// dftracer_gen_fake_trace binary and available to tests that need fixtures.
namespace dftracer::utils::domains::workload::trace_gen {

/// Deterministic 16-char hex hash using the project's HasherUtility
inline dstd::String make_hash(dstd::StringView name) {
    fileio::hash::HasherUtility hasher;
    hasher.reset();
    auto h = hasher.process(name);
    char buf[16];
    const auto text = dstd::fmt_to<"{:016x}">(buf, sizeof(buf), h.value);
    return dstd::String(buf, text.size);
}

/// +/-30% random variance around base
inline std::uint64_t jitter(std::mt19937_64& rng, std::uint64_t base) {
    std::uniform_real_distribution<double> dist(0.7, 1.3);
    return static_cast<std::uint64_t>(static_cast<double>(base) * dist(rng));
}

/// Metadata event: {"name":"HH"/"FH"/"SH","type":1,"pid":0,"tid":0,"ts":0,
///                  "ph":4,"args":{"hhash":"...","name":"...","value":"..."}}
/// Hash and tracer-bookkeeping metadata is the DFTRACER layer. A write error
/// is kept by the writer and returned by its close().
inline void emit_metadata(trace::writer::TraceWriter& w, dstd::StringView kind,
                          dstd::StringView hhash,
                          dstd::StringView resolved_name,
                          dstd::StringView hash_value) {
    const trace::writer::Arg args[] = {
        {"hhash", hhash}, {"name", resolved_name}, {"value", hash_value}};
    trace::writer::RecordExtras extras;
    extras.type = trace::EventType::Dftracer;
    w.write_metadata(kind, 0, 0, args, extras).discard();
}

/// Regular complete event (duration, ph=1)
struct EventArgs {
    std::uint64_t id = 0;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    dstd::String name;
    dstd::String cat;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;
    int level = 0;
    dstd::String hhash;
    dstd::String fhash;
    dstd::String cmd_hash;
    /// Instrumentation layer. Unknown lets emit_event infer it from cat.
    trace::EventType type = trace::EventType::Unknown;
    /// Extra args written after the fixed ones. Borrowed text must outlive
    /// the emit_event call.
    dstd::Vector<trace::writer::Arg> extra;
};

inline void emit_event(trace::writer::TraceWriter& w, const EventArgs& a) {
    trace::writer::RecordExtras extras;
    extras.has_id = true;
    extras.id = a.id;
    extras.type = a.type != trace::EventType::Unknown
                      ? a.type
                      : trace::event_type_from_cat(a.cat);

    dstd::Vector<trace::writer::Arg> args;
    args.reserve(4 + a.extra.size());
    args.push_back({"hhash", dstd::StringView(a.hhash)});
    args.push_back({"level", a.level});
    if (!a.fhash.empty()) args.push_back({"fhash", dstd::StringView(a.fhash)});
    if (!a.cmd_hash.empty())
        args.push_back({"cmd_hash", dstd::StringView(a.cmd_hash)});
    for (const auto& extra : a.extra) args.push_back(extra);

    w.write_event(a.name, a.cat, a.ts, a.dur, a.pid, a.tid, args, extras)
        .discard();
}

}  // namespace dftracer::utils::domains::workload::trace_gen

#endif  // DFTRACER_UTILS_DOMAINS_WORKLOAD_TRACE_GEN_FAKE_TRACE_WRITER_H
