// Compares the index store writer and reader with RocksDB, as the indexer uses
// it, over the same payload, keys, values and threads: write GB/s at 1, 16 and
// 96 threads with one shared file against one file per thread, the writes and
// syncs of the write path by kind, the bytes on disk and the on-disk
// amplification of both engines, the cost of a commit as the number of scopes
// grows, the read path (open, point get, scope scan and full scan) on the data
// the shared write case left behind, and a fold scan over many scopes, the way
// an index query sums per-file aggregates over every file of a trace set.
//
//   store_bench --smoke
//
//   store_bench --dir /scratch --case all --engine both --threads 1,16,96
//   --payload-mib 256
//
//   store_bench --dir /private/tmp --case query --engine rocksdb --gets 100000
//
//   store_bench --dir /private/tmp --case fold --fold-scopes 1000,8404
//   --fold-keys 1024
//
// Output: comment lines naming the page size, the I/O backend, the file system
// kind and the options both engines run under, then TSV rows. A write row is
// one mode, engine and thread count; its I/O counts come from io_fault's
// counting mode and cover every descriptor of the case, so they leave out the
// header each writer writes when it is created, and the RocksDB side prints a
// dash there. A query row is one operation and engine, read from the data the
// shared write case wrote. The scopes case is ours only. Keys are 16 bytes and
// unique, so the keys read back count the entries a case committed. A fold row
// is one engine, scope count and part count: its scan reads scopes * keys fold
// entries and yields one output key per distinct key, so ns_per_key divides by
// keys, and 1, 4 and 16 parts show how one fold scan scales.

#include <bench_counters.h>
#include <dftracer/utils/common/filesystem_info.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/little_endian.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/index/store/database.h>
#include <dftracer/utils/index/store/engine/io_fault.h>
#include <dftracer/utils/index/store/engine/page_size.h>
#include <dftracer/utils/index/store/engine/snapshot.h>
#include <dftracer/utils/index/store/engine/writer.h>
#include <dftracer/utils/runtime/executor/executor.h>
#include <dftracer/utils/runtime/io/io_backend.h>
#include <dftracer/utils/runtime/runtime.h>
#include <rocksdb/db.h>
#include <rocksdb/iterator.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/statistics.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace coro = dftracer::utils::coro;
namespace dstd = dftracer::utils::dstd;
namespace engine = dftracer::utils::index::store::engine;
namespace io_fault = dftracer::utils::index::store::engine::io_fault;

using dftracer::utils::ByteView;
using dftracer::utils::CoroScope;
using dftracer::utils::FilesystemKind;
using dftracer::utils::Runtime;
using engine::Batch;
using engine::NamespaceId;
using engine::NamespaceSpec;
using engine::ScopeKey;
using engine::Snapshot;
using engine::Writer;
using engine::WriterErrc;
using engine::WriterOptions;
using engine::WriterTestHook;

constexpr std::uint64_t MIB_BYTES = 1U << 20;
constexpr std::uint64_t GIB_BYTES = 1U << 30;
constexpr std::size_t KEY_BYTES = 16;
/// Scopes one coroutine commits to, so two coroutines share no scope.
constexpr std::uint64_t SCOPES_PER_CORO = 64;
/// The runtime the scope and fold cases commit and open on. Each measures one
/// committing coroutine; a second thread keeps the group commit running.
constexpr std::size_t SCOPE_CASE_THREADS = 2;
/// Busy retries of one commit before the case is called stuck.
constexpr int COMMIT_ATTEMPTS = 1 << 16;
/// The one namespace of the benchmark, the first byte of a RocksDB key.
constexpr std::uint8_t STORE_NAMESPACE_ID = 1;
/// Separates the scope from the entry key, so every key of a scope shares a
/// prefix and a scope is one range read.
constexpr char SCOPE_TERMINATOR = '\0';
/// A key index no write uses, so the entry is absent from every scope.
constexpr std::uint32_t ABSENT_KEY_INDEX = 0xFFFFFFFFU;
/// The seed of the query keys, fixed so two runs read the same keys.
constexpr std::uint64_t QUERY_SEED = 0x5EEDC0DEULL;
/// Scopes the scope-scan rows read.
constexpr std::uint64_t SCAN_SCOPES = 100;
/// Point gets of a run unless --gets says otherwise, and of a smoke run.
constexpr std::uint64_t POINT_GETS = 100000;
constexpr std::uint64_t POINT_GETS_SMOKE = 2000;
/// One miss for every this many hits.
constexpr std::uint64_t GETS_PER_MISS = 10;
/// The value of one fold entry: one little-endian int64.
constexpr std::size_t FOLD_VALUE_BYTES = 8;
/// The key of one fold entry: 'k' and six digits, so the keys of a fold scope
/// are in key-number order.
constexpr std::size_t FOLD_KEY_BYTES = 7;
/// The most keys the six digits of a fold key number.
constexpr std::uint64_t FOLD_KEY_LIMIT = 1000000;
/// No key number, so one comparison rejects a malformed fold key.
constexpr std::uint64_t FOLD_KEY_INVALID = ~std::uint64_t{0};
/// The modulus of the fold values, so a sum over every scope stays small.
constexpr std::uint64_t FOLD_VALUE_MODULUS = 1000;
/// The parts a fold scan is split into: the whole scan, then two meshes.
constexpr std::uint64_t FOLD_THREADS[] = {1, 4, 16};

const char* filesystem_name(FilesystemKind kind) {
    switch (kind) {
        case FilesystemKind::Local:
            return "local";
        case FilesystemKind::Tmpfs:
            return "tmpfs";
        case FilesystemKind::Nfs:
            return "nfs";
        case FilesystemKind::Lustre:
            return "lustre";
        case FilesystemKind::Gpfs:
            return "gpfs";
        case FilesystemKind::Beegfs:
            return "beegfs";
        case FilesystemKind::Unknown:
            break;
    }
    return "unknown";
}

const char* compression_name(::rocksdb::CompressionType type) {
    switch (type) {
        case ::rocksdb::kNoCompression:
            return "none";
        case ::rocksdb::kSnappyCompression:
            return "snappy";
        case ::rocksdb::kZlibCompression:
            return "zlib";
        case ::rocksdb::kBZip2Compression:
            return "bzip2";
        case ::rocksdb::kLZ4Compression:
            return "lz4";
        case ::rocksdb::kLZ4HCCompression:
            return "lz4hc";
        case ::rocksdb::kXpressCompression:
            return "xpress";
        case ::rocksdb::kZSTD:
            return "zstd";
        case ::rocksdb::kDisableCompressionOption:
            return "disabled";
        default:
            break;
    }
    return "unknown";
}

struct Options {
    bool smoke = false;
    bool help = false;
    std::string dir;
    std::vector<int> threads{1, 16, 96};
    std::vector<std::string> modes{"shared", "per-thread"};
    std::vector<std::string> engines{"ours", "rocksdb"};
    std::uint64_t payload_mib = 256;
    std::size_t value_bytes = 200;
    bool trace_values = true;
    std::size_t keys_per_batch = 256;
    std::vector<std::uint64_t> scopes{10, 1000, 100000};
    std::uint64_t commits = 20000;
    std::uint64_t gets = POINT_GETS;
    std::vector<std::uint64_t> fold_scopes{1000, 8404};
    std::uint64_t fold_keys = 1024;
    std::string which = "all";
};

void apply_smoke(Options& opt) {
    opt.smoke = true;
    opt.threads = {1, 2};
    opt.modes = {"shared", "per-thread"};
    opt.engines = {"ours", "rocksdb"};
    opt.payload_mib = 1;
    opt.value_bytes = 200;
    opt.keys_per_batch = 256;
    opt.scopes = {4, 64};
    opt.commits = 2000;
    opt.gets = POINT_GETS_SMOKE;
    opt.fold_scopes = {50};
    opt.fold_keys = 64;
    opt.which = "all";
}

std::vector<std::uint64_t> parse_u64_list(const std::string& text) {
    std::vector<std::uint64_t> out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = text.find(',', at);
        const std::string item =
            text.substr(at, end == std::string::npos ? end : end - at);
        if (item.empty()) return {};
        try {
            out.push_back(std::stoull(item));
        } catch (...) {
            return {};
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 < argc) return argv[++i];
            return {};
        };
        if (arg == "--smoke") {
            apply_smoke(opt);
        } else if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else if (arg == "--dir") {
            opt.dir = value();
        } else if (arg == "--threads") {
            std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.empty()) return false;
            opt.threads.clear();
            for (std::uint64_t threads : parsed) {
                if (threads == 0 || threads > 4096) return false;
                opt.threads.push_back(static_cast<int>(threads));
            }
        } else if (arg == "--mode") {
            const std::string mode = value();
            if (mode == "shared") {
                opt.modes = {"shared"};
            } else if (mode == "per-thread") {
                opt.modes = {"per-thread"};
            } else if (mode == "both") {
                opt.modes = {"shared", "per-thread"};
            } else {
                return false;
            }
        } else if (arg == "--engine") {
            const std::string which_engine = value();
            if (which_engine == "ours") {
                opt.engines = {"ours"};
            } else if (which_engine == "rocksdb") {
                opt.engines = {"rocksdb"};
            } else if (which_engine == "both") {
                opt.engines = {"ours", "rocksdb"};
            } else {
                return false;
            }
        } else if (arg == "--payload-mib") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0) return false;
            opt.payload_mib = parsed[0];
        } else if (arg == "--value-kind") {
            const std::string kind = value();
            if (kind == "trace") {
                opt.trace_values = true;
            } else if (kind == "repeat") {
                opt.trace_values = false;
            } else {
                return false;
            }
        } else if (arg == "--value-bytes") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0) return false;
            opt.value_bytes = parsed[0];
        } else if (arg == "--keys-per-batch") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0) return false;
            opt.keys_per_batch = parsed[0];
        } else if (arg == "--scopes") {
            opt.scopes = parse_u64_list(value());
            if (opt.scopes.empty()) return false;
            for (std::uint64_t scopes : opt.scopes) {
                if (scopes == 0) return false;
            }
        } else if (arg == "--commits") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0) return false;
            opt.commits = parsed[0];
        } else if (arg == "--gets") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0) return false;
            opt.gets = parsed[0];
        } else if (arg == "--fold-scopes") {
            opt.fold_scopes = parse_u64_list(value());
            if (opt.fold_scopes.empty()) return false;
            for (std::uint64_t scopes : opt.fold_scopes) {
                if (scopes == 0) return false;
            }
        } else if (arg == "--fold-keys") {
            const std::vector<std::uint64_t> parsed = parse_u64_list(value());
            if (parsed.size() != 1 || parsed[0] == 0 ||
                parsed[0] >= FOLD_KEY_LIMIT) {
                return false;
            }
            opt.fold_keys = parsed[0];
        } else if (arg == "--case") {
            const std::string which = value();
            if (which != "write" && which != "query" && which != "scopes" &&
                which != "fold" && which != "all") {
                return false;
            }
            opt.which = which;
        } else {
            return false;
        }
    }
    return true;
}

void usage() {
    std::cerr
        << "usage: store_bench [--smoke] [--value-kind trace|repeat] [--dir "
           "PATH] [--threads 1,16,96]\n"
           "                   [--mode shared|per-thread|both]\n"
           "                   [--engine ours|rocksdb|both]\n"
           "                   [--payload-mib N] [--value-bytes N]\n"
           "                   [--keys-per-batch N] [--scopes 10,1000,100000]\n"
           "                   [--commits N] [--gets N]\n"
           "                   [--fold-scopes 1000,8404] [--fold-keys N]\n"
           "                   [--case write|query|scopes|fold|all]\n";
}

dstd::String dstring(std::string_view text) {
    return dstd::String(text.data(), text.size());
}

std::string to_std(const dstd::Error& error) {
    const std::string_view message = error.message();
    if (message.empty()) return std::string(error.name());
    return std::string(error.name()) + ": " + std::string(message);
}

std::string format_ms(std::uint64_t nanos) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3)
        << static_cast<double>(nanos) / 1e6;
    return out.str();
}

/// Every block the snapshot has read so far: the commits it verified, the
/// blocks of them, the read calls that covered them and the bytes. An open
/// leaves these at zero, and the first query for a copy pays its commits.
std::string format_read_stats(const engine::ReadStats& reads) {
    std::ostringstream out;
    out << "commits_read=" << reads.commits_verified
        << " blocks_read=" << reads.blocks_read
        << " metadata_blocks_read=" << reads.metadata_blocks_read
        << " read_calls=" << reads.read_calls << " mib_read=" << std::fixed
        << std::setprecision(1)
        << static_cast<double>(reads.bytes_read) /
               static_cast<double>(MIB_BYTES);
    return out.str();
}

/// What one open did, and what the snapshot has read so far, as one extra
/// column.
std::string format_stats(const engine::OpenStats& stats,
                         const engine::ReadStats& reads) {
    std::ostringstream out;
    out << "files=" << stats.files << " pages=" << stats.pages << " "
        << format_read_stats(reads) << " ms list=" << std::setprecision(3)
        << stats.ms_list << " files=" << stats.ms_open_files
        << " tables=" << stats.ms_tables << " total=" << stats.ms_total;
    return out.str();
}

std::string format_rate(std::uint64_t rows, std::uint64_t nanos) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1)
        << (nanos > 0
                ? static_cast<double>(rows) * 1e9 / static_cast<double>(nanos)
                : 0);
    return out.str();
}

double seconds_between(std::chrono::steady_clock::time_point from,
                       std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

std::uint64_t nanos_between(std::chrono::steady_clock::time_point from,
                            std::chrono::steady_clock::time_point to) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from)
            .count());
}

std::uint64_t indexed(const io_fault::Counts& counts,
                      io_fault::WriteKind kind) {
    return static_cast<std::size_t>(kind) < io_fault::KINDS
               ? counts.bytes[static_cast<std::size_t>(kind)]
               : 0;
}

std::uint64_t total_bytes(const io_fault::Counts& counts) {
    std::uint64_t total = 0;
    for (std::size_t at = 0; at < io_fault::KINDS; ++at) {
        total += counts.bytes[at];
    }
    return total;
}

/// The key of one entry: the coroutine, its batch ordinal and the position in
/// the batch, all unique, so the keys of a case count its entries.
std::array<std::uint8_t, KEY_BYTES> make_key(std::uint32_t coro,
                                             std::uint32_t ordinal,
                                             std::uint32_t index) {
    std::array<std::uint8_t, KEY_BYTES> key{};
    dftracer::utils::write_u64_le(key.data(), coro);
    dftracer::utils::write_u32_le(key.data() + 8, ordinal);
    dftracer::utils::write_u32_le(key.data() + 12, index);
    return key;
}

/// The values every case writes. trace: VALUE_POOL different lines shaped
/// like DFTracer events, so they compress about as real index data does;
/// repeat: one value of one repeated byte. A key picks its value by a hash of
/// its bytes, so a reader can check any value without a table.
class ValuePool {
   public:
    ValuePool(std::size_t value_bytes, bool trace) {
        if (!trace) {
            values_.emplace_back(value_bytes, 'v');
            return;
        }
        std::mt19937_64 rng(VALUE_SEED);
        static const char* const NAMES[] = {"read",  "write",   "open64",
                                            "close", "lseek64", "fstat"};
        static const char* const CATS[] = {"POSIX", "STDIO", "MPI"};
        values_.reserve(VALUE_POOL);
        for (std::size_t i = 0; i < VALUE_POOL; ++i) {
            std::string line =
                "{\"id\":" + std::to_string(rng() % 100000000) +
                ",\"name\":\"" + NAMES[rng() % 6] + "\",\"cat\":\"" +
                CATS[rng() % 3] +
                "\",\"pid\":" + std::to_string(rng() % 65536) +
                ",\"tid\":" + std::to_string(rng() % 65536) +
                ",\"ts\":" + std::to_string(rng() % 10000000000000ULL) +
                ",\"dur\":" + std::to_string(rng() % 100000) +
                ",\"args\":{\"fhash\":\"" + std::to_string(rng()) +
                "\",\"size_t\":" + std::to_string(rng() % 1048576) + "}}";
            while (line.size() < value_bytes) line += line;
            line.resize(value_bytes);
            values_.push_back(std::move(line));
        }
    }

    const std::string& for_key(const std::uint8_t* key,
                               std::size_t size) const {
        std::uint64_t hash = 1469598103934665603ULL;
        for (std::size_t i = 0; i < size; ++i) {
            hash = (hash ^ key[i]) * 1099511628211ULL;
        }
        return values_[static_cast<std::size_t>(hash % values_.size())];
    }

   private:
    static constexpr std::size_t VALUE_POOL = 4096;
    static constexpr std::uint64_t VALUE_SEED = 0x5eed;
    std::vector<std::string> values_;
};

std::string scope_name(std::uint64_t coro, std::uint64_t slot) {
    return "/bench/c" + std::to_string(coro) + "-s" + std::to_string(slot) +
           ".dft";
}

/// The key prefix of one scope on the RocksDB side: the namespace id and the
/// scope, terminated, so the scope's entries are one range of keys.
std::string scope_prefix(std::uint64_t coro, std::uint64_t slot) {
    std::string prefix;
    prefix.reserve(1 + 32 + 1);
    prefix.push_back(static_cast<char>(STORE_NAMESPACE_ID));
    prefix += scope_name(coro, slot);
    prefix.push_back(SCOPE_TERMINATOR);
    return prefix;
}

std::string rocks_key(std::uint64_t coro, std::uint64_t batch_index,
                      std::uint32_t index) {
    const std::size_t slot = batch_index % SCOPES_PER_CORO;
    std::string key = scope_prefix(coro, slot);
    const auto entry = make_key(static_cast<std::uint32_t>(coro),
                                static_cast<std::uint32_t>(batch_index), index);
    key.append(reinterpret_cast<const char*>(entry.data()), entry.size());
    return key;
}

/// The key of one fold entry: 'k' and six digits, so a lexicographic scan
/// yields the keys in key-number order and a key needs no table.
std::string fold_key(std::uint64_t index) {
    std::string key(FOLD_KEY_BYTES, '0');
    key[0] = 'k';
    for (std::size_t at = FOLD_KEY_BYTES; at-- > 1;) {
        key[at] = static_cast<char>('0' + index % 10);
        index /= 10;
    }
    return key;
}

std::vector<std::string> fold_key_names(std::uint64_t keys) {
    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(keys));
    for (std::uint64_t at = 0; at < keys; ++at) {
        names.push_back(fold_key(at));
    }
    return names;
}

/// The key number of one fold key, or FOLD_KEY_INVALID when the bytes are not
/// one of ours.
std::uint64_t fold_key_index(ByteView key) {
    if (key.size() != FOLD_KEY_BYTES) return FOLD_KEY_INVALID;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(key.data());
    if (bytes[0] != static_cast<std::uint8_t>('k')) return FOLD_KEY_INVALID;
    std::uint64_t index = 0;
    for (std::size_t at = 1; at < FOLD_KEY_BYTES; ++at) {
        if (bytes[at] < '0' || bytes[at] > '9') return FOLD_KEY_INVALID;
        index = index * 10 + static_cast<std::uint64_t>(bytes[at] - '0');
    }
    return index;
}

/// The value the first section of one scope holds for one key; the second
/// section holds 1 for every key of the scope.
std::uint64_t fold_value(std::uint64_t scope, std::uint64_t key) {
    return (scope + key) % FOLD_VALUE_MODULUS;
}

/// What a fold scan over `scopes` scopes must sum one key to: the two
/// sections of every scope, the first (scope + key) mod FOLD_VALUE_MODULUS and
/// the second one.
std::vector<std::int64_t> fold_expectation(std::uint64_t scopes,
                                           std::uint64_t keys) {
    std::vector<std::int64_t> expected(static_cast<std::size_t>(keys), 0);
    for (std::uint64_t key = 0; key < keys; ++key) {
        std::int64_t sum = 0;
        for (std::uint64_t scope = 0; scope < scopes; ++scope) {
            sum += static_cast<std::int64_t>(fold_value(scope, key)) + 1;
        }
        expected[static_cast<std::size_t>(key)] = sum;
    }
    return expected;
}

/// A fresh directory under `base`, so concurrent runs never share one. The
/// caller removes it.
std::string make_temp_dir(const std::filesystem::path& base) {
    const std::string pattern = (base / "dftu_store_bench_XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const char* made = ::mkdtemp(buffer.data());
    return made == nullptr ? std::string() : std::string(made);
}

/// The bytes of every regular file under `dir`, at any depth.
std::uint64_t directory_bytes(const std::string& dir) {
    std::uint64_t total = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(dir, error);
    if (error) return 0;
    const std::filesystem::recursive_directory_iterator end;
    while (iterator != end) {
        std::error_code size_error;
        if (iterator->is_regular_file(size_error)) {
            const auto size = iterator->file_size(size_error);
            if (!size_error) total += size;
        }
        iterator.increment(error);
        if (error) break;
    }
    return total;
}

/// The keys every engine writes, and which of them a commit wrote or a key
/// absent from every scope: what the query case reads and checks.
struct Workload {
    std::uint64_t threads = 0;
    std::uint64_t batches = 0;
    std::size_t keys_per_batch = 0;
    std::size_t value_bytes = 0;
    std::size_t entry_bytes = 0;
    std::uint64_t per_thread_keys = 0;
    std::uint64_t payload = 0;
    std::uint64_t total_keys = 0;
    std::uint64_t scopes_per_thread = 0;
    std::uint64_t total_scopes = 0;
};

Workload workload_of(const Options& opt, std::uint64_t threads) {
    Workload workload;
    workload.threads = threads;
    workload.keys_per_batch = opt.keys_per_batch;
    workload.value_bytes = opt.value_bytes;
    workload.entry_bytes = KEY_BYTES + opt.value_bytes;
    const std::uint64_t per_thread = opt.payload_mib * MIB_BYTES / threads;
    const std::uint64_t batch_bytes =
        static_cast<std::uint64_t>(workload.keys_per_batch) *
        workload.entry_bytes;
    workload.batches = std::max<std::uint64_t>(1, per_thread / batch_bytes);
    workload.per_thread_keys = workload.batches * workload.keys_per_batch;
    workload.payload =
        workload.per_thread_keys * workload.entry_bytes * threads;
    workload.total_keys = workload.per_thread_keys * threads;
    workload.scopes_per_thread =
        std::min<std::uint64_t>(workload.batches, SCOPES_PER_CORO);
    workload.total_scopes = workload.scopes_per_thread * threads;
    return workload;
}

/// The entries one scope holds: the batch ordinals that map to it.
std::uint64_t scope_rows(const Workload& workload, std::uint64_t slot) {
    if (slot >= workload.batches) return 0;
    return ((workload.batches - 1 - slot) / SCOPES_PER_CORO + 1) *
           workload.keys_per_batch;
}

bool has_engine(const std::vector<std::string>& engines,
                const std::string& name) {
    return std::find(engines.begin(), engines.end(), name) != engines.end();
}

const ::rocksdb::WriteOptions& rocks_write_options() {
    static const ::rocksdb::WriteOptions options = [] {
        ::rocksdb::WriteOptions write;
        write.disableWAL = true;
        return write;
    }();
    return options;
}

/// The options the indexer would open with, so both sides write under one
/// table and cache configuration.
::rocksdb::Options rocks_bench_options() {
    auto options =
        dftracer::utils::index::store::RocksDatabase::default_options();
    static_cast<::rocksdb::ColumnFamilyOptions&>(options) = dftracer::utils::
        index::store::RocksDatabase::default_column_family_options();
    return options;
}

::rocksdb::Options rocks_read_options() {
    auto options = rocks_bench_options();
    options.create_if_missing = false;
    options.create_missing_column_families = false;
    return options;
}

/// The commit of one batch with the retry the writer documents: the bytes of
/// earlier commits are written, then the same batch commits again (a Busy
/// commit left it whole).
coro::CoroTask<bool> commit_retrying(Writer& writer, Batch batch,
                                     int attempts) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto committed = writer.commit(std::move(batch));
        if (committed.has_value()) co_return true;
        if (!committed.error().is_code(WriterErrc::Busy)) co_return false;
        co_await writer.space();
    }
    co_return false;
}

struct WriteRow {
    std::string mode;
    std::string engine = "ours";
    int threads = 0;
    std::uint64_t payload = 0;
    double seconds = 0;
    /// The write path up to the flush and the close, and the same with the
    /// compaction wait between them; ours has one phase, so both are the close.
    double seconds_flushed = 0;
    double seconds_compacted = 0;
    io_fault::Counts counts;
    std::uint64_t busy = 0;
    std::uint64_t commits = 0;
    std::uint64_t keys = 0;
    std::uint64_t read_keys = 0;
    std::uint64_t disk_bytes = 0;
    std::uint64_t db_bytes_written = 0;
    std::uint64_t db_compaction_bytes = 0;
    /// The directory the query case reads, when this case keeps one.
    std::string keep_dir;
};

struct QueryRow {
    std::string engine;
    std::string op;
    std::uint64_t count = 0;
    double ms_total = 0;
    double ns_per_item = 0;
    std::string extra;
    double rss_growth_mib = 0;
};

struct ScopeRow {
    std::uint64_t scopes = 0;
    double ns_new_scope = 0;
    double ns_commit = 0;
};

struct FoldRow {
    std::string engine;
    std::uint64_t scopes = 0;
    std::uint64_t keys = 0;
    std::uint64_t threads = 0;
    double ms_open = 0;
    double ms_scan = 0;
    double ns_per_key = 0;
    std::uint64_t mismatches = 0;
    /// What the snapshot had read when the scan of this row finished.
    engine::ReadStats reads;
};

/// One timed fold scan: its parts added, against the sums written.
struct FoldScan {
    std::uint64_t nanos = 0;
    std::uint64_t mismatches = 0;
    bool ok = true;
};

/// One query key of one scope, in both encodings, so the timed loop only reads.
struct QueryItem {
    std::size_t scope = 0;
    std::array<std::uint8_t, KEY_BYTES> key{};
    std::string rocks;
};

struct QueryScan {
    std::size_t scope = 0;
    std::uint64_t rows = 0;
};

struct QueryPlan {
    std::vector<ScopeKey> scopes;
    std::vector<std::string> prefixes;
    std::vector<QueryItem> hits;
    std::vector<QueryItem> misses;
    std::vector<QueryScan> scans;
};

struct Bench {
    Options opt;
    std::string root;
    std::string backend = "none";
    bool backend_known = false;
    bool write_header_written = false;
    bool query_header_written = false;
    bool scope_header_written = false;
    bool fold_header_written = false;
    bool comment_written = false;
    bool failed = false;
    std::uint32_t page_size = 0;
    std::uint64_t case_index = 0;
    std::vector<QueryRow> query_rows;

    NamespaceSpec spec() const {
        NamespaceSpec namespace_spec;
        namespace_spec.name = dstring("bench");
        namespace_spec.format_version = 0;
        namespace_spec.fold = engine::FoldKind::None;
        namespace_spec.durable = false;
        return namespace_spec;
    }

    WriterOptions writer_options() const {
        WriterOptions options;
        options.host = dstring("store_bench");
        options.label = dstring("store_bench");
        return options;
    }

    void fail(const std::string& message) {
        failed = true;
        std::cerr << "store_bench: " << message << "\n";
    }

    bool setup() {
        std::error_code error;
        std::filesystem::path base;
        if (opt.smoke) {
            base = std::filesystem::temp_directory_path(error);
            if (error) {
                fail("cannot find a temporary directory: " + error.message());
                return false;
            }
        } else {
            if (opt.dir.empty()) {
                fail("--dir is required unless --smoke");
                usage();
                return false;
            }
            base = opt.dir;
        }
        std::filesystem::create_directories(base, error);
        if (error) {
            fail("cannot create " + base.string() + ": " + error.message());
            return false;
        }
        root = make_temp_dir(base);
        if (root.empty()) {
            fail("cannot create a directory under " + base.string());
            return false;
        }
        auto chosen = engine::page_size_for(root);
        if (!chosen) {
            fail("page size of " + root + ": " + to_std(chosen.error()));
            return false;
        }
        page_size = *chosen;
        return true;
    }

    void cleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::string case_dir(const std::string& name) {
        return root + "/" + std::to_string(case_index++) + "-" + name;
    }

    void note_backend(Runtime& runtime) {
        if (backend_known) return;
        if (runtime.executor()->has_io_backend()) {
            backend = runtime.executor()->io_backend().name();
        }
        backend_known = true;
    }

    void ensure_comment() {
        if (comment_written) return;
        comment_written = true;
        const auto cf = dftracer::utils::index::store::RocksDatabase::
            default_column_family_options();
        std::cout
            << "# page_size=" << page_size << " backend=" << backend
            << " fs=" << filesystem_name(dftracer::utils::filesystem_kind(root))
            << " dir=" << root << "\n"
            << "# ours codec=none; Snapshot::open reads only the metadata, and"
               " a query reads and checks each page on its first read\n"
            << "# rocksdb compression=" << compression_name(cf.compression)
            << " bottommost=" << compression_name(cf.bottommost_compression)
            << " wal=off block_cache_mib="
            << (dftracer::utils::index::store::g_block_cache_bytes.load(
                    std::memory_order_relaxed) /
                MIB_BYTES)
            << " write_buffer_mib=" << (cf.write_buffer_size / MIB_BYTES)
            << " per column family (one block cache and one write buffer"
               " manager for every DB of the process)\n"
            << "# rocksdb default column family uses"
               " default_column_family_options (32 KiB blocks, no bloom"
               " filter); the indexer opens it with"
               " point_lookup_column_family_options\n"
            << "# rocksdb point gets and scans use default ReadOptions (no"
               " async prefetch)\n"
            << (opt.trace_values
                    ? "# values: 4096 different DFTracer-like event lines, "
                      "picked by key hash; ours stores them uncompressed\n"
                    : "# values: one repeated byte, so RocksDB compression is "
                      "at its best and ours stores it uncompressed\n")
            << "# rss_growth_mib is the growth of the peak resident set of the"
               " process (getrusage maxrss) across a phase, and a peak only"
               " rises, so a phase that reuses the memory of an earlier one"
               " reports little\n";
    }

    void ensure_write_header() {
        if (write_header_written) return;
        write_header_written = true;
        std::cout
            << "mode\tengine\tthreads\tpayload_bytes\tseconds\tgib_per_s"
               "\tseconds_flushed\tseconds_compacted\tpage_writes\tpage_bytes"
               "\tbig_writes\tbig_bytes\trecord_writes\trecord_bytes"
               "\theader_writes\theader_bytes\tfooter_bytes\ttrailer_bytes"
               "\tsyncs\ttotal_bytes\twrite_amp\tdisk_bytes\tdisk_amp"
               "\tdb_bytes_written\tdb_compaction_bytes\tdb_write_amp\tbusy"
               "\tmax_page_bytes\tcommits_per_s\n";
    }

    void ensure_query_header() {
        if (query_header_written) return;
        query_header_written = true;
        std::cout << "engine\top\tcount\tms_total\tns_per_item\textra"
                     "\trss_growth_mib\n";
    }

    void ensure_scope_header() {
        if (scope_header_written) return;
        scope_header_written = true;
        std::cout << "scopes\tns_new_scope\tns_commit\n";
    }

    void ensure_fold_header() {
        if (fold_header_written) return;
        fold_header_written = true;
        std::cout << "engine\tscopes\tkeys\tthreads\tms_open\tms_scan"
                     "\tns_per_key\tmismatches\treads\n";
    }

    bool run();
    WriteRow run_ours_write_case(Runtime& runtime, const Workload& workload,
                                 const std::string& dir, bool per_thread,
                                 bool keep_dir);
    WriteRow run_rocksdb_write_case(const Workload& workload,
                                    const std::string& dir, bool per_thread,
                                    bool keep_dir);
    QueryPlan make_query_plan(const Workload& workload) const;
    bool run_query_case(const Workload& workload, const std::string& ours_dir,
                        const std::string& rocksdb_dir);
    bool query_ours(const Workload& workload, const std::string& dir,
                    const QueryPlan& plan);
    bool query_rocksdb(const Workload& workload, const std::string& dir,
                       const QueryPlan& plan);
    ScopeRow run_scope_case(Runtime& runtime, std::uint64_t scopes);
    NamespaceSpec fold_spec() const;
    bool write_fold_ours(Runtime& runtime, const std::string& dir,
                         std::uint64_t scopes, std::uint64_t keys);
    bool write_fold_rocksdb(const std::string& dir, std::uint64_t scopes,
                            std::uint64_t keys);
    FoldScan fold_scan_ours(const Snapshot& snapshot,
                            const NamespaceSpec& namespace_spec,
                            const std::vector<ScopeKey>& scopes,
                            std::uint64_t keys, std::uint64_t threads,
                            const std::vector<std::int64_t>& expected);
    FoldScan fold_scan_rocksdb(::rocksdb::DB& db,
                               const std::vector<std::string>& prefixes,
                               std::uint64_t keys, std::uint64_t threads,
                               const std::vector<std::int64_t>& expected);
    bool run_fold_case(Runtime& runtime, std::uint64_t scopes);
    void print_write_row(const WriteRow& row);
    void print_query_row(const QueryRow& row);
    void print_scope_row(const ScopeRow& row);
    void print_fold_row(const FoldRow& row);
    bool check_write_row(const WriteRow& row);
    std::uint64_t count_keys(const std::string& dir);
    std::uint64_t count_rocks_keys(const std::string& dir);
    void close_writers(Runtime& runtime,
                       std::vector<std::unique_ptr<Writer>>& writers,
                       std::atomic<int>& errors);
    bool report_single_thread_open(const std::string& dir,
                                   const std::string& prefix, bool print_now);
};

QueryRow query_row(std::string engine_name, std::string op, std::uint64_t count,
                   std::uint64_t nanos, std::uint64_t rss_before,
                   std::string extra) {
    QueryRow row;
    row.engine = std::move(engine_name);
    row.op = std::move(op);
    row.count = count;
    row.ms_total = static_cast<double>(nanos) / 1e6;
    row.ns_per_item =
        count > 0 ? static_cast<double>(nanos) / static_cast<double>(count) : 0;
    row.extra = std::move(extra);
    row.rss_growth_mib =
        static_cast<double>(dftu::bench::peak_heap_bytes() - rss_before) /
        static_cast<double>(MIB_BYTES);
    return row;
}

void Bench::print_write_row(const WriteRow& row) {
    ensure_write_header();
    ensure_comment();
    const bool ours = row.engine == "ours";
    const std::uint64_t total = total_bytes(row.counts);
    const double seconds = row.seconds;
    const double gib_per_s =
        seconds > 0 ? static_cast<double>(row.payload) / seconds / GIB_BYTES
                    : 0;
    const double write_amp =
        ours && row.payload > 0
            ? static_cast<double>(total) / static_cast<double>(row.payload)
            : 0;
    const double disk_amp = row.payload > 0
                                ? static_cast<double>(row.disk_bytes) /
                                      static_cast<double>(row.payload)
                                : 0;
    const double db_write_amp =
        !ours && row.payload > 0
            ? static_cast<double>(row.db_bytes_written +
                                  row.db_compaction_bytes) /
                  static_cast<double>(row.payload)
            : 0;
    const double commits_per_s =
        seconds > 0 ? static_cast<double>(row.commits) / seconds : 0;
    const auto& counts = row.counts;
    const auto writes_of = [&counts](io_fault::WriteKind kind) {
        return counts.writes[static_cast<std::size_t>(kind)];
    };
    std::ostringstream line;
    line << row.mode << '\t' << row.engine << '\t' << row.threads << '\t'
         << row.payload << '\t' << std::fixed << std::setprecision(6) << seconds
         << '\t' << std::setprecision(4) << gib_per_s << '\t'
         << std::setprecision(6) << row.seconds_flushed << '\t'
         << row.seconds_compacted << '\t';
    const auto ours_only = [&line, ours](std::uint64_t value) {
        if (ours) {
            line << value;
        } else {
            line << '-';
        }
        line << '\t';
    };
    const auto rocksdb_only = [&line, ours](std::uint64_t value) {
        if (ours) {
            line << '-';
        } else {
            line << value;
        }
        line << '\t';
    };
    ours_only(writes_of(io_fault::WriteKind::Page));
    ours_only(indexed(counts, io_fault::WriteKind::Page));
    ours_only(writes_of(io_fault::WriteKind::Big));
    ours_only(indexed(counts, io_fault::WriteKind::Big));
    ours_only(writes_of(io_fault::WriteKind::Record));
    ours_only(indexed(counts, io_fault::WriteKind::Record));
    ours_only(writes_of(io_fault::WriteKind::Header));
    ours_only(indexed(counts, io_fault::WriteKind::Header));
    ours_only(indexed(counts, io_fault::WriteKind::Footer));
    ours_only(indexed(counts, io_fault::WriteKind::Trailer));
    ours_only(counts.syncs);
    ours_only(total);
    if (ours) {
        line << std::setprecision(4) << write_amp;
    } else {
        line << '-';
    }
    line << '\t' << row.disk_bytes << '\t' << std::setprecision(4) << disk_amp
         << '\t';
    rocksdb_only(row.db_bytes_written);
    rocksdb_only(row.db_compaction_bytes);
    if (ours) {
        line << '-';
    } else {
        line << std::setprecision(4) << db_write_amp;
    }
    line << '\t';
    ours_only(row.busy);
    ours_only(
        counts.max_bytes[static_cast<std::size_t>(io_fault::WriteKind::Page)]);
    line << std::setprecision(1) << commits_per_s;
    std::cout << line.str() << '\n';
}

void Bench::print_query_row(const QueryRow& row) {
    ensure_query_header();
    ensure_comment();
    std::ostringstream line;
    line << row.engine << '\t' << row.op << '\t' << row.count << '\t'
         << std::fixed << std::setprecision(3) << row.ms_total << '\t'
         << std::setprecision(1) << row.ns_per_item << '\t' << row.extra << '\t'
         << std::setprecision(3) << row.rss_growth_mib;
    std::cout << line.str() << '\n';
}

void Bench::print_scope_row(const ScopeRow& row) {
    ensure_scope_header();
    ensure_comment();
    std::ostringstream line;
    line << row.scopes << '\t' << std::fixed << std::setprecision(1)
         << row.ns_new_scope << '\t' << row.ns_commit;
    std::cout << line.str() << '\n';
}

void Bench::print_fold_row(const FoldRow& row) {
    ensure_fold_header();
    ensure_comment();
    std::ostringstream line;
    line << row.engine << '\t' << row.scopes << '\t' << row.keys << '\t'
         << row.threads << '\t' << std::fixed << std::setprecision(3)
         << row.ms_open << '\t' << row.ms_scan << '\t' << std::setprecision(1)
         << row.ns_per_key << '\t' << row.mismatches << '\t'
         << format_read_stats(row.reads);
    std::cout << line.str() << '\n';
}

/// Opens `dir` a second time under a one-thread default runtime, restores the
/// default runtime that was installed, and adds the row next to the timed open.
/// One worker still coalesces the runs, so read_calls shows what the split
/// alone gives and the time shows what the other workers add. A case whose
/// rows are printed as they come asks for the row to be printed here.
bool Bench::report_single_thread_open(const std::string& dir,
                                      const std::string& prefix,
                                      bool print_now) {
    const std::shared_ptr<Runtime> previous =
        dftracer::utils::peek_default_runtime();
    dftracer::utils::set_default_runtime(std::make_shared<Runtime>(1));
    const std::uint64_t rss = dftu::bench::peak_heap_bytes();
    const auto started = std::chrono::steady_clock::now();
    auto opened = Snapshot::open(dir);
    const std::uint64_t nanos =
        nanos_between(started, std::chrono::steady_clock::now());
    dftracer::utils::set_default_runtime(previous);
    if (!opened) {
        fail("one-thread snapshot of " + dir + ": " + to_std(opened.error()));
        return false;
    }
    QueryRow row = query_row(
        "ours", "open_1t", 1, nanos, rss,
        prefix + format_stats(opened->open_stats(), opened->read_stats()));
    if (print_now) {
        print_query_row(row);
    } else {
        query_rows.push_back(std::move(row));
    }
    return true;
}

bool Bench::check_write_row(const WriteRow& row) {
    const auto& counts = row.counts;
    const std::size_t page =
        static_cast<std::size_t>(io_fault::WriteKind::Page);
    const std::size_t record =
        static_cast<std::size_t>(io_fault::WriteKind::Record);
    const std::size_t header =
        static_cast<std::size_t>(io_fault::WriteKind::Header);
    const std::size_t footer =
        static_cast<std::size_t>(io_fault::WriteKind::Footer);
    const std::size_t trailer =
        static_cast<std::size_t>(io_fault::WriteKind::Trailer);
    bool ok = true;
    const auto require = [&](bool condition, const char* what) {
        if (!condition) {
            fail(row.engine + " " + row.mode + " " +
                 std::to_string(row.threads) + " threads: " + what);
            ok = false;
        }
    };
    require(row.seconds > 0, "no time");
    require(row.seconds_flushed > 0, "no flush time");
    require(row.seconds_compacted > 0, "no compaction time");
    require(row.keys > 0, "no keys");
    require(row.read_keys == row.keys, "the keys read back differ");
    require(row.disk_bytes > 0, "no bytes on disk");
    if (!(row.engine == "ours")) {
        require(row.db_bytes_written > 0, "RocksDB wrote no bytes");
        require(row.seconds_compacted >= row.seconds_flushed,
                "the compaction time is below the flush time");
        return ok;
    }
    require(counts.writes[page] > 0, "no page write");
    require(counts.bytes[page] > 0, "no page bytes");
    require(counts.writes[record] > 0, "no record write");
    require(counts.bytes[record] > 0, "no record bytes");
    require(counts.writes[header] > 0, "no header write");
    require(counts.bytes[header] > 0, "no header bytes");
    require(counts.bytes[footer] > 0, "no footer bytes");
    require(counts.bytes[trailer] > 0, "no trailer bytes");
    require(counts.syncs > 0, "no sync");
    const std::uint64_t total = total_bytes(counts);
    require(total > 0, "no bytes written");
    require(total >= row.payload, "wrote less than the payload");
    return ok;
}

std::uint64_t Bench::count_keys(const std::string& dir) {
    auto snapshot = Snapshot::open(dir);
    if (!snapshot) {
        fail("snapshot of " + dir + ": " + to_std(snapshot.error()));
        return 0;
    }
    std::uint64_t count = 0;
    auto cursor = snapshot->scan_all(spec(), {}, {});
    while (!cursor.at_end()) {
        ++count;
        cursor.next();
    }
    if (cursor.error()) {
        fail("scan of " + dir + ": " + to_std(*cursor.error()));
    }
    return count;
}

std::uint64_t Bench::count_rocks_keys(const std::string& dir) {
    std::unique_ptr<::rocksdb::DB> db;
    auto status =
        ::rocksdb::DB::OpenForReadOnly(rocks_read_options(), dir, &db);
    if (!status.ok()) {
        fail("RocksDB read-only open of " + dir + ": " + status.ToString());
        return 0;
    }
    ::rocksdb::ReadOptions read;
    auto iterator = std::unique_ptr<::rocksdb::Iterator>(db->NewIterator(read));
    std::uint64_t count = 0;
    for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) ++count;
    if (!iterator->status().ok()) {
        fail("RocksDB scan of " + dir + ": " + iterator->status().ToString());
        return 0;
    }
    return count;
}

void Bench::close_writers(Runtime& runtime,
                          std::vector<std::unique_ptr<Writer>>& writers,
                          std::atomic<int>& errors) {
    runtime.run_blocking(
        "store_bench close", [&](CoroScope& scope) -> coro::CoroTask<void> {
            for (std::size_t at = 0; at < writers.size(); ++at) {
                Writer* writer = writers[at].get();
                scope.spawn(
                    [writer, &errors](CoroScope&) -> coro::CoroTask<void> {
                        auto closed = co_await writer->close();
                        if (!closed) {
                            errors.fetch_add(1, std::memory_order_relaxed);
                        }
                        co_return;
                    });
            }
            co_await scope.join();
        });
}

WriteRow Bench::run_ours_write_case(Runtime& runtime, const Workload& workload,
                                    const std::string& dir, bool per_thread,
                                    bool keep_dir) {
    WriteRow row;
    row.engine = "ours";
    row.mode = per_thread ? "per-thread" : "shared";
    row.threads = static_cast<int>(workload.threads);
    row.payload = workload.payload;
    row.keys = workload.total_keys;
    row.commits = workload.batches * workload.threads;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        fail("cannot create " + dir + ": " + error.message());
        return row;
    }

    const std::size_t writers_count =
        per_thread ? static_cast<std::size_t>(workload.threads) : 1;
    std::vector<std::unique_ptr<Writer>> writers(writers_count);
    std::vector<NamespaceId> namespaces(writers_count, NamespaceId{0});
    for (std::size_t at = 0; at < writers_count; ++at) {
        auto created =
            Writer::create(dir, *runtime.executor(), writer_options());
        if (!created) {
            fail("writer of " + dir + ": " + to_std(created.error()));
            return row;
        }
        writers[at] = std::move(*created);
        auto namespace_id = writers[at]->add_namespace(spec());
        if (!namespace_id) {
            fail("namespace of " + dir + ": " + to_std(namespace_id.error()));
            return row;
        }
        namespaces[at] = *namespace_id;
    }

    const ValuePool values(workload.value_bytes, opt.trace_values);
    std::atomic<int> errors{0};
    io_fault::start_counting();
    const auto started = std::chrono::steady_clock::now();
    runtime.run_blocking(
        "store_bench write", [&](CoroScope& scope) -> coro::CoroTask<void> {
            for (std::uint64_t at = 0; at < workload.threads; ++at) {
                scope.spawn([&, at](CoroScope&) -> coro::CoroTask<void> {
                    const std::size_t slot =
                        per_thread ? static_cast<std::size_t>(at) : 0;
                    Writer& writer = *writers[slot];
                    const NamespaceId namespace_id = namespaces[slot];
                    for (std::uint64_t batch_index = 0;
                         batch_index < workload.batches; ++batch_index) {
                        const ScopeKey scope_key = ScopeKey::file(
                            scope_name(at, batch_index % SCOPES_PER_CORO));
                        Batch batch = writer.batch(namespace_id, scope_key, 0);
                        for (std::size_t k = 0; k < opt.keys_per_batch; ++k) {
                            const auto key = make_key(
                                static_cast<std::uint32_t>(at),
                                static_cast<std::uint32_t>(batch_index),
                                static_cast<std::uint32_t>(k));
                            const std::string& value =
                                values.for_key(key.data(), key.size());
                            batch.put(ByteView{key.data(), key.size()},
                                      ByteView{value.data(), value.size()});
                        }
                        const bool committed = co_await commit_retrying(
                            writer, std::move(batch), COMMIT_ATTEMPTS);
                        if (!committed) {
                            errors.fetch_add(1, std::memory_order_relaxed);
                            co_return;
                        }
                    }
                    co_return;
                });
            }
            co_await scope.join();
        });
    close_writers(runtime, writers, errors);
    const auto stopped = std::chrono::steady_clock::now();
    row.counts = io_fault::stop_counting();
    row.seconds = seconds_between(started, stopped);
    row.seconds_flushed = row.seconds;
    row.seconds_compacted = row.seconds;
    for (const auto& writer : writers) {
        row.busy += WriterTestHook::pages(*writer).busy;
    }
    if (errors.load() > 0) {
        fail(row.mode + " " + std::to_string(workload.threads) +
             " threads: a commit or a close failed");
    }
    row.read_keys = count_keys(dir);
    row.disk_bytes = directory_bytes(dir);
    check_write_row(row);
    writers.clear();
    if (keep_dir) {
        row.keep_dir = dir;
    } else {
        std::filesystem::remove_all(dir, error);
    }
    return row;
}

WriteRow Bench::run_rocksdb_write_case(const Workload& workload,
                                       const std::string& dir, bool per_thread,
                                       bool keep_dir) {
    WriteRow row;
    row.engine = "rocksdb";
    row.mode = per_thread ? "per-thread" : "shared";
    row.threads = static_cast<int>(workload.threads);
    row.payload = workload.payload;
    row.keys = workload.total_keys;
    row.commits = workload.batches * workload.threads;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        fail("cannot create " + dir + ": " + error.message());
        return row;
    }

    auto options = rocks_bench_options();
    options.statistics = ::rocksdb::CreateDBStatistics();
    const std::size_t dbs_count =
        per_thread ? static_cast<std::size_t>(workload.threads) : 1;
    std::vector<std::string> db_dirs(dbs_count);
    for (std::size_t at = 0; at < dbs_count; ++at) {
        db_dirs[at] = per_thread ? dir + "/db" + std::to_string(at) : dir;
    }
    std::vector<std::unique_ptr<::rocksdb::DB>> dbs(dbs_count);
    for (std::size_t at = 0; at < dbs_count; ++at) {
        auto status = ::rocksdb::DB::Open(options, db_dirs[at], &dbs[at]);
        if (!status.ok()) {
            fail("RocksDB open of " + db_dirs[at] + ": " + status.ToString());
            return row;
        }
    }

    const ValuePool values(workload.value_bytes, opt.trace_values);
    std::atomic<int> errors{0};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> writers;
    writers.reserve(static_cast<std::size_t>(workload.threads));
    for (std::uint64_t at = 0; at < workload.threads; ++at) {
        writers.emplace_back([&, at] {
            ::rocksdb::DB* db =
                dbs[per_thread ? static_cast<std::size_t>(at) : 0].get();
            for (std::uint64_t batch_index = 0; batch_index < workload.batches;
                 ++batch_index) {
                ::rocksdb::WriteBatch batch;
                for (std::size_t k = 0; k < opt.keys_per_batch; ++k) {
                    const std::string key = rocks_key(
                        at, batch_index, static_cast<std::uint32_t>(k));
                    const auto entry =
                        make_key(static_cast<std::uint32_t>(at),
                                 static_cast<std::uint32_t>(batch_index),
                                 static_cast<std::uint32_t>(k));
                    const std::string& value =
                        values.for_key(entry.data(), entry.size());
                    batch.Put(::rocksdb::Slice(key), ::rocksdb::Slice(value));
                }
                auto status = db->Write(rocks_write_options(), &batch);
                if (!status.ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& writer : writers) writer.join();

    // The close of one DB: flush, then wait for the compactions the flush
    // scheduled, then close. The flush time is the last flush to end over the
    // DBs, and the close time the last DB to close.
    std::vector<std::uint64_t> flushed(dbs_count, 0);
    std::vector<std::uint64_t> compacted(dbs_count, 0);
    std::vector<std::thread> closers;
    closers.reserve(dbs_count);
    for (std::size_t at = 0; at < dbs_count; ++at) {
        closers.emplace_back([&, at] {
            ::rocksdb::FlushOptions flush_options;
            flush_options.wait = true;
            auto status = dbs[at]->Flush(flush_options);
            if (!status.ok()) errors.fetch_add(1, std::memory_order_relaxed);
            flushed[at] =
                nanos_between(started, std::chrono::steady_clock::now());
            ::rocksdb::WaitForCompactOptions wait_options;
            wait_options.abort_on_pause = false;
            wait_options.flush = false;
            status = dbs[at]->WaitForCompact(wait_options);
            if (!status.ok()) errors.fetch_add(1, std::memory_order_relaxed);
            compacted[at] =
                nanos_between(started, std::chrono::steady_clock::now());
            dbs[at].reset();
        });
    }
    for (auto& closer : closers) closer.join();
    const std::uint64_t closed =
        nanos_between(started, std::chrono::steady_clock::now());
    const std::uint64_t flush_nanos =
        *std::max_element(flushed.begin(), flushed.end());
    const std::uint64_t compact_nanos =
        *std::max_element(compacted.begin(), compacted.end());
    // The close itself happens after the compaction wait, so its cost is what
    // is left between the last compaction and the last close.
    row.seconds_flushed =
        static_cast<double>(flush_nanos + (closed - compact_nanos)) / 1e9;
    row.seconds_compacted = static_cast<double>(closed) / 1e9;
    row.seconds = row.seconds_flushed;

    row.db_bytes_written =
        options.statistics->getTickerCount(::rocksdb::BYTES_WRITTEN);
    row.db_compaction_bytes =
        options.statistics->getTickerCount(::rocksdb::COMPACT_WRITE_BYTES);
    row.disk_bytes = directory_bytes(dir);
    if (errors.load() > 0) {
        fail(row.mode + " " + std::to_string(workload.threads) +
             " threads: a RocksDB write, flush or compaction failed");
    }
    for (const std::string& db_dir : db_dirs) {
        row.read_keys += count_rocks_keys(db_dir);
    }
    check_write_row(row);
    if (keep_dir) {
        row.keep_dir = dir;
    } else {
        std::filesystem::remove_all(dir, error);
    }
    return row;
}

QueryPlan Bench::make_query_plan(const Workload& workload) const {
    QueryPlan plan;
    const std::size_t scopes = static_cast<std::size_t>(workload.total_scopes);
    plan.scopes.reserve(scopes);
    plan.prefixes.reserve(scopes);
    for (std::uint64_t coro = 0; coro < workload.threads; ++coro) {
        for (std::uint64_t slot = 0; slot < workload.scopes_per_thread;
             ++slot) {
            plan.prefixes.push_back(scope_prefix(coro, slot));
            plan.scopes.push_back(ScopeKey::file(scope_name(coro, slot)));
        }
    }

    std::mt19937_64 random(QUERY_SEED);
    const auto add = [&](std::vector<QueryItem>& items, std::uint64_t count,
                         bool absent) {
        items.reserve(items.size() + static_cast<std::size_t>(count));
        for (std::uint64_t at = 0; at < count; ++at) {
            const std::uint64_t coro = random() % workload.threads;
            const std::uint64_t batch_index = random() % workload.batches;
            const std::uint32_t index =
                absent ? ABSENT_KEY_INDEX
                       : static_cast<std::uint32_t>(random() %
                                                    workload.keys_per_batch);
            QueryItem item;
            item.scope =
                static_cast<std::size_t>(coro) * workload.scopes_per_thread +
                batch_index % SCOPES_PER_CORO;
            item.key = make_key(static_cast<std::uint32_t>(coro),
                                static_cast<std::uint32_t>(batch_index), index);
            item.rocks = plan.prefixes[item.scope];
            item.rocks.append(reinterpret_cast<const char*>(item.key.data()),
                              item.key.size());
            items.push_back(std::move(item));
        }
    };
    add(plan.hits, opt.gets, false);
    add(plan.misses, std::max<std::uint64_t>(1, opt.gets / GETS_PER_MISS),
        true);

    std::vector<std::size_t> every_scope(scopes);
    std::iota(every_scope.begin(), every_scope.end(), std::size_t{0});
    std::mt19937_64 scan_random(QUERY_SEED ^ 0x9E3779B97F4A7C15ULL);
    std::shuffle(every_scope.begin(), every_scope.end(), scan_random);
    if (every_scope.size() > SCAN_SCOPES) {
        every_scope.resize(static_cast<std::size_t>(SCAN_SCOPES));
    }
    for (std::size_t index : every_scope) {
        const std::uint64_t slot =
            static_cast<std::uint64_t>(index) % workload.scopes_per_thread;
        plan.scans.push_back(QueryScan{index, scope_rows(workload, slot)});
    }
    return plan;
}

bool Bench::query_ours(const Workload& workload, const std::string& dir,
                       const QueryPlan& plan) {
    const std::string engine_name = "ours";
    const NamespaceSpec namespace_spec = spec();

    // An open of its own first, so the reported open is the warm one: the
    // second reads only what the first left in the page cache of the operating
    // system, and this store caches nothing between opens. The peak resident
    // set of a process only rises, so the memory an open holds is the growth of
    // the first one and not of the second, which reuses it.
    const std::uint64_t cold_rss = dftu::bench::peak_heap_bytes();
    const auto cold_started = std::chrono::steady_clock::now();
    std::uint64_t cold_nanos = 0;
    dstd::Optional<Snapshot> cold;
    {
        auto opened_cold = Snapshot::open(dir);
        cold_nanos =
            nanos_between(cold_started, std::chrono::steady_clock::now());
        if (!opened_cold) {
            fail("cold snapshot of " + dir + ": " +
                 to_std(opened_cold.error()));
            return false;
        }
        cold = std::move(*opened_cold);
    }
    const auto open_started = std::chrono::steady_clock::now();
    auto opened = Snapshot::open(dir);
    const std::uint64_t open_nanos =
        nanos_between(open_started, std::chrono::steady_clock::now());
    if (!opened) {
        fail("snapshot of " + dir + ": " + to_std(opened.error()));
        return false;
    }
    Snapshot snapshot = std::move(*opened);
    query_rows.push_back(query_row(
        engine_name, "open", 1, open_nanos, cold_rss,
        "warm ms reported, cold=" + format_ms(cold_nanos) +
            " ms, rss_growth of the cold open, " +
            format_stats(snapshot.open_stats(), snapshot.read_stats())));
    // The first get of the cold snapshot: it pays the reads of the commits
    // that hold the copy, which the open above did not make.
    if (cold.has_value()) {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        auto first =
            cold->get(namespace_spec, plan.scopes[plan.hits.front().scope],
                      ByteView{plan.hits.front().key.data(),
                               plan.hits.front().key.size()});
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        if (first.has_value() && first->has_value()) {
            query_rows.push_back(
                query_row(engine_name, "first_get", 1, nanos, rss,
                          format_read_stats(cold->read_stats())));
        } else {
            fail("ours: the first get of the cold snapshot of " + dir +
                 " returned nothing");
        }
        cold.reset();
    }
    // The same open under a one-thread default runtime, so the read calls the
    // runs coalesce and the time of a single worker sit next to the pool's.
    if (!report_single_thread_open(dir, "", false)) return false;

    const ValuePool values(workload.value_bytes, opt.trace_values);
    std::uint64_t mismatches = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryItem& item : plan.hits) {
            auto found =
                snapshot.get(namespace_spec, plan.scopes[item.scope],
                             ByteView{item.key.data(), item.key.size()});
            if (!found) {
                fail("get of " + dir + ": " + to_std(found.error()));
                return false;
            }
            const auto& value = *found;
            if (!value.has_value() ||
                std::string_view(
                    reinterpret_cast<const char*>(value->bytes().data()),
                    value->bytes().size()) !=
                    values.for_key(item.key.data(), item.key.size())) {
                ++mismatches;
            }
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(
            query_row(engine_name, "get_hit", plan.hits.size(), nanos, rss,
                      "mismatches=" + std::to_string(mismatches) + " " +
                          format_read_stats(snapshot.read_stats())));
    }
    if (mismatches > 0) {
        fail("ours: " + std::to_string(mismatches) + " point gets of " + dir +
             " did not return the value written");
    }
    mismatches = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryItem& item : plan.misses) {
            auto found =
                snapshot.get(namespace_spec, plan.scopes[item.scope],
                             ByteView{item.key.data(), item.key.size()});
            if (!found) {
                fail("get of " + dir + ": " + to_std(found.error()));
                return false;
            }
            if (found->has_value()) ++mismatches;
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(
            query_row(engine_name, "get_miss", plan.misses.size(), nanos, rss,
                      "mismatches=" + std::to_string(mismatches) + " " +
                          format_read_stats(snapshot.read_stats())));
    }
    if (mismatches > 0) {
        fail("ours: " + std::to_string(mismatches) + " absent keys of " + dir +
             " were found");
    }

    std::uint64_t scanned = 0;
    std::uint64_t wrong_scopes = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryScan& scan : plan.scans) {
            std::uint64_t rows = 0;
            auto cursor =
                snapshot.scan(namespace_spec, plan.scopes[scan.scope], {});
            while (!cursor.at_end()) {
                ++rows;
                cursor.next();
            }
            if (cursor.error()) {
                fail("scan of " + dir + ": " + to_std(*cursor.error()));
            }
            scanned += rows;
            if (rows != scan.rows) ++wrong_scopes;
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        std::ostringstream extra;
        extra << "scopes=" << plan.scans.size()
              << " rows_per_scope=" << std::fixed << std::setprecision(1)
              << (plan.scans.empty()
                      ? 0
                      : static_cast<double>(scanned) / plan.scans.size())
              << " " << format_read_stats(snapshot.read_stats());
        query_rows.push_back(query_row(engine_name, "scan_scope", scanned,
                                       nanos, rss, extra.str()));
    }
    if (wrong_scopes > 0) {
        fail("ours: " + std::to_string(wrong_scopes) + " scopes of " + dir +
             " hold another row count than written");
    }

    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t rows = 0;
        auto cursor = snapshot.scan_all(namespace_spec, {}, {});
        while (!cursor.at_end()) {
            ++rows;
            cursor.next();
        }
        if (cursor.error()) {
            fail("scan of " + dir + ": " + to_std(*cursor.error()));
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(query_row(
            engine_name, "full_scan", rows, nanos, rss,
            "rows_per_s=" + format_rate(rows, nanos) +
                " rows_expected=" + std::to_string(workload.total_keys) + " " +
                format_read_stats(snapshot.read_stats())));
        if (rows != workload.total_keys) {
            fail("ours: the full scan of " + dir + " read " +
                 std::to_string(rows) + " rows, not " +
                 std::to_string(workload.total_keys));
        }
    }
    return true;
}

bool Bench::query_rocksdb(const Workload& workload, const std::string& dir,
                          const QueryPlan& plan) {
    const std::string engine_name = "rocksdb";
    const auto options = rocks_read_options();

    // As on the ours side: an open of its own first, then the timed warm one,
    // and the peak resident set across the first.
    const std::uint64_t cold_rss = dftu::bench::peak_heap_bytes();
    const auto cold_started = std::chrono::steady_clock::now();
    std::uint64_t cold_nanos = 0;
    {
        std::unique_ptr<::rocksdb::DB> cold;
        auto status = ::rocksdb::DB::OpenForReadOnly(options, dir, &cold);
        cold_nanos =
            nanos_between(cold_started, std::chrono::steady_clock::now());
        if (!status.ok()) {
            fail("RocksDB cold open of " + dir + ": " + status.ToString());
            return false;
        }
    }
    const auto open_started = std::chrono::steady_clock::now();
    std::unique_ptr<::rocksdb::DB> db;
    auto status = ::rocksdb::DB::OpenForReadOnly(options, dir, &db);
    const std::uint64_t open_nanos =
        nanos_between(open_started, std::chrono::steady_clock::now());
    if (!status.ok()) {
        fail("RocksDB open of " + dir + ": " + status.ToString());
        return false;
    }
    query_rows.push_back(
        query_row(engine_name, "open", 1, open_nanos, cold_rss,
                  "warm ms reported, cold=" + format_ms(cold_nanos) +
                      " ms, rss_growth of the cold open"));

    const ValuePool values(workload.value_bytes, opt.trace_values);
    ::rocksdb::ReadOptions read;
    std::string value;
    std::uint64_t mismatches = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryItem& item : plan.hits) {
            value.clear();
            status = db->Get(read, ::rocksdb::Slice(item.rocks), &value);
            if (!status.ok() ||
                value != values.for_key(item.key.data(), item.key.size())) {
                ++mismatches;
            }
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(
            query_row(engine_name, "get_hit", plan.hits.size(), nanos, rss,
                      "mismatches=" + std::to_string(mismatches)));
    }
    if (mismatches > 0) {
        fail("rocksdb: " + std::to_string(mismatches) + " point gets of " +
             dir + " did not return the value written");
    }
    mismatches = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryItem& item : plan.misses) {
            value.clear();
            status = db->Get(read, ::rocksdb::Slice(item.rocks), &value);
            if (!status.IsNotFound()) ++mismatches;
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(
            query_row(engine_name, "get_miss", plan.misses.size(), nanos, rss,
                      "mismatches=" + std::to_string(mismatches)));
    }
    if (mismatches > 0) {
        fail("rocksdb: " + std::to_string(mismatches) + " absent keys of " +
             dir + " were found");
    }

    auto iterator = std::unique_ptr<::rocksdb::Iterator>(db->NewIterator(read));
    std::uint64_t scanned = 0;
    std::uint64_t wrong_scopes = 0;
    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        for (const QueryScan& scan : plan.scans) {
            const ::rocksdb::Slice prefix(plan.prefixes[scan.scope]);
            std::uint64_t rows = 0;
            iterator->Seek(prefix);
            while (iterator->Valid() && iterator->key().starts_with(prefix)) {
                ++rows;
                iterator->Next();
            }
            if (!iterator->status().ok()) {
                fail("rocksdb scan of " + dir + ": " +
                     iterator->status().ToString());
                return false;
            }
            scanned += rows;
            if (rows != scan.rows) ++wrong_scopes;
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        std::ostringstream extra;
        extra << "scopes=" << plan.scans.size()
              << " rows_per_scope=" << std::fixed << std::setprecision(1)
              << (plan.scans.empty()
                      ? 0
                      : static_cast<double>(scanned) / plan.scans.size());
        query_rows.push_back(query_row(engine_name, "scan_scope", scanned,
                                       nanos, rss, extra.str()));
    }
    if (wrong_scopes > 0) {
        fail("rocksdb: " + std::to_string(wrong_scopes) + " scopes of " + dir +
             " hold another row count than written");
    }

    {
        const std::uint64_t rss = dftu::bench::peak_heap_bytes();
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t rows = 0;
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next()) {
            ++rows;
        }
        if (!iterator->status().ok()) {
            fail("rocksdb full scan of " + dir + ": " +
                 iterator->status().ToString());
            return false;
        }
        const std::uint64_t nanos =
            nanos_between(started, std::chrono::steady_clock::now());
        query_rows.push_back(query_row(
            engine_name, "full_scan", rows, nanos, rss,
            "rows_per_s=" + format_rate(rows, nanos) +
                " rows_expected=" + std::to_string(workload.total_keys)));
        if (rows != workload.total_keys) {
            fail("rocksdb: the full scan of " + dir + " read " +
                 std::to_string(rows) + " rows, not " +
                 std::to_string(workload.total_keys));
        }
    }
    return true;
}

bool Bench::run_query_case(const Workload& workload,
                           const std::string& ours_dir,
                           const std::string& rocksdb_dir) {
    if (ours_dir.empty() && rocksdb_dir.empty()) {
        ensure_comment();
        std::cout << "# the query case reads the data of the shared write case,"
                     " which --mode per-thread does not write; skipped\n";
        return true;
    }
    const QueryPlan plan = make_query_plan(workload);
    if (!ours_dir.empty() && !query_ours(workload, ours_dir, plan)) {
        return false;
    }
    if (!rocksdb_dir.empty() && !query_rocksdb(workload, rocksdb_dir, plan)) {
        return false;
    }
    return true;
}

ScopeRow Bench::run_scope_case(Runtime& runtime, std::uint64_t scopes) {
    ScopeRow row;
    row.scopes = scopes;
    const std::string dir = case_dir("scopes-" + std::to_string(scopes));
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        fail("cannot create " + dir + ": " + error.message());
        return row;
    }
    auto created = Writer::create(dir, *runtime.executor(), writer_options());
    if (!created) {
        fail("writer of " + dir + ": " + to_std(created.error()));
        return row;
    }
    std::unique_ptr<Writer> writer = std::move(*created);
    auto namespace_id = writer->add_namespace(spec());
    if (!namespace_id) {
        fail("namespace of " + dir + ": " + to_std(namespace_id.error()));
        return row;
    }

    const std::string value(opt.value_bytes, 'v');
    std::atomic<bool> ok{true};
    std::uint64_t new_scope_nanos = 0;
    std::uint64_t commit_nanos = 0;
    runtime.run_blocking(
        "store_bench scopes", [&](CoroScope&) -> coro::CoroTask<void> {
            std::uint32_t ordinal = 0;
            const auto new_started = std::chrono::steady_clock::now();
            for (std::uint64_t at = 0; at < scopes; ++at) {
                const ScopeKey scope_key =
                    ScopeKey::file("/bench/s" + std::to_string(at) + ".dft");
                Batch batch = writer->batch(*namespace_id, scope_key, 0);
                const auto key =
                    make_key(0, ordinal, static_cast<std::uint32_t>(at));
                ordinal += 1;
                batch.put(ByteView{key.data(), key.size()},
                          ByteView{value.data(), value.size()});
                const bool committed = co_await commit_retrying(
                    *writer, std::move(batch), COMMIT_ATTEMPTS);
                if (!committed) {
                    ok.store(false, std::memory_order_relaxed);
                    co_return;
                }
            }
            const auto new_stopped = std::chrono::steady_clock::now();
            new_scope_nanos = nanos_between(new_started, new_stopped);

            std::mt19937_64 random(0x5EEDBEEFULL);
            const auto commit_started = std::chrono::steady_clock::now();
            for (std::uint64_t at = 0; at < opt.commits; ++at) {
                const std::uint64_t scope_index = random() % scopes;
                const ScopeKey scope_key = ScopeKey::file(
                    "/bench/s" + std::to_string(scope_index) + ".dft");
                Batch batch = writer->batch(*namespace_id, scope_key, 0);
                const auto key = make_key(
                    0, ordinal, static_cast<std::uint32_t>(scope_index));
                ordinal += 1;
                batch.put(ByteView{key.data(), key.size()},
                          ByteView{value.data(), value.size()});
                const bool committed = co_await commit_retrying(
                    *writer, std::move(batch), COMMIT_ATTEMPTS);
                if (!committed) {
                    ok.store(false, std::memory_order_relaxed);
                    co_return;
                }
            }
            commit_nanos =
                nanos_between(commit_started, std::chrono::steady_clock::now());
        });
    runtime.run_blocking(
        "store_bench scopes close", [&](CoroScope&) -> coro::CoroTask<void> {
            auto closed = co_await writer->close();
            if (!closed) {
                fail("close of " + dir + ": " + to_std(closed.error()));
                ok.store(false, std::memory_order_relaxed);
            }
        });
    if (!ok.load()) fail("scope case of " + std::to_string(scopes) + " scopes");
    row.ns_new_scope =
        static_cast<double>(new_scope_nanos) / static_cast<double>(scopes);
    row.ns_commit =
        static_cast<double>(commit_nanos) / static_cast<double>(opt.commits);
    writer.reset();
    std::filesystem::remove_all(dir, error);
    return row;
}

NamespaceSpec Bench::fold_spec() const {
    NamespaceSpec namespace_spec;
    namespace_spec.name = dstring("bench-fold");
    namespace_spec.format_version = 0;
    namespace_spec.fold = engine::FoldKind::SumI64;
    namespace_spec.durable = false;
    return namespace_spec;
}

bool Bench::write_fold_ours(Runtime& runtime, const std::string& dir,
                            std::uint64_t scopes, std::uint64_t keys) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        fail("cannot create " + dir + ": " + error.message());
        return false;
    }
    auto created = Writer::create(dir, *runtime.executor(), writer_options());
    if (!created) {
        fail("writer of " + dir + ": " + to_std(created.error()));
        return false;
    }
    std::unique_ptr<Writer> writer = std::move(*created);
    auto namespace_id = writer->add_namespace(fold_spec());
    if (!namespace_id) {
        fail("namespace of " + dir + ": " + to_std(namespace_id.error()));
        return false;
    }

    // Two batches per scope, so every scope holds two sections and the scan
    // folds across them: the first with fold_value(scope, key), the second
    // with 1 for every key.
    const std::vector<std::string> names = fold_key_names(keys);
    std::vector<std::uint8_t> values(static_cast<std::size_t>(keys) *
                                     FOLD_VALUE_BYTES);
    std::array<std::uint8_t, FOLD_VALUE_BYTES> one{};
    dftracer::utils::write_u64_le(one.data(), 1);
    std::atomic<bool> ok{true};
    runtime.run_blocking(
        "store_bench fold write", [&](CoroScope&) -> coro::CoroTask<void> {
            for (std::uint64_t scope = 0; scope < scopes; ++scope) {
                const ScopeKey scope_key = ScopeKey::file(scope_name(0, scope));
                for (std::size_t section = 0; section < 2; ++section) {
                    Batch batch = writer->batch(*namespace_id, scope_key, 0);
                    for (std::uint64_t key = 0; key < keys; ++key) {
                        const std::size_t at =
                            static_cast<std::size_t>(key) * FOLD_VALUE_BYTES;
                        const std::uint8_t* bytes = one.data();
                        if (section == 0) {
                            dftracer::utils::write_u64_le(
                                values.data() + at, fold_value(scope, key));
                            bytes = values.data() + at;
                        }
                        batch.fold(
                            ByteView{
                                names[static_cast<std::size_t>(key)].data(),
                                names[static_cast<std::size_t>(key)].size()},
                            ByteView{bytes, FOLD_VALUE_BYTES});
                    }
                    const bool committed = co_await commit_retrying(
                        *writer, std::move(batch), COMMIT_ATTEMPTS);
                    if (!committed) {
                        ok.store(false, std::memory_order_relaxed);
                        co_return;
                    }
                }
            }
        });
    runtime.run_blocking(
        "store_bench fold close", [&](CoroScope&) -> coro::CoroTask<void> {
            auto closed = co_await writer->close();
            if (!closed) {
                fail("close of " + dir + ": " + to_std(closed.error()));
                ok.store(false, std::memory_order_relaxed);
            }
        });
    if (!ok.load()) {
        fail("fold write of " + dir + " failed");
        return false;
    }
    return true;
}

bool Bench::write_fold_rocksdb(const std::string& dir, std::uint64_t scopes,
                               std::uint64_t keys) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    if (error) {
        fail("cannot create " + dir + ": " + error.message());
        return false;
    }
    std::unique_ptr<::rocksdb::DB> db;
    auto status = ::rocksdb::DB::Open(rocks_bench_options(), dir, &db);
    if (!status.ok()) {
        fail("RocksDB open of " + dir + ": " + status.ToString());
        return false;
    }

    // The same two batches per scope that ours gets, in the same order. The
    // key of each ends in its batch number, so the second does not overwrite
    // the first and the scan folds the two, as ours does.
    const std::vector<std::string> names = fold_key_names(keys);
    std::array<std::uint8_t, FOLD_VALUE_BYTES> value{};
    std::string key;
    for (std::uint64_t scope = 0; scope < scopes; ++scope) {
        const std::string prefix = scope_prefix(0, scope);
        for (std::uint8_t section = 0; section < 2; ++section) {
            ::rocksdb::WriteBatch batch;
            for (std::uint64_t at = 0; at < keys; ++at) {
                dftracer::utils::write_u64_le(
                    value.data(), section == 0 ? fold_value(scope, at) : 1);
                key.assign(prefix);
                key += names[static_cast<std::size_t>(at)];
                key.push_back(static_cast<char>(section));
                batch.Put(::rocksdb::Slice(key),
                          ::rocksdb::Slice(
                              reinterpret_cast<const char*>(value.data()),
                              FOLD_VALUE_BYTES));
            }
            status = db->Write(rocks_write_options(), &batch);
            if (!status.ok()) {
                fail("RocksDB write of " + dir + ": " + status.ToString());
                return false;
            }
        }
    }
    ::rocksdb::FlushOptions flush_options;
    flush_options.wait = true;
    status = db->Flush(flush_options);
    if (!status.ok()) {
        fail("RocksDB flush of " + dir + ": " + status.ToString());
        return false;
    }
    return true;
}

FoldScan Bench::fold_scan_ours(const Snapshot& snapshot,
                               const NamespaceSpec& namespace_spec,
                               const std::vector<ScopeKey>& scopes,
                               std::uint64_t keys, std::uint64_t threads,
                               const std::vector<std::int64_t>& expected) {
    FoldScan scan;
    std::vector<std::vector<std::int64_t>> parts(
        static_cast<std::size_t>(threads),
        std::vector<std::int64_t>(static_cast<std::size_t>(keys), 0));
    std::atomic<int> errors{0};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (std::uint64_t at = 0; at < threads; ++at) {
        workers.emplace_back([&, at] {
            const std::size_t part = static_cast<std::size_t>(at);
            const std::size_t count = static_cast<std::size_t>(threads);
            const std::size_t begin = scopes.size() * part / count;
            const std::size_t end = scopes.size() * (part + 1) / count;
            std::vector<std::int64_t>& sums = parts[part];
            auto cursor = snapshot.scan(
                namespace_spec,
                dstd::Span<const ScopeKey>(scopes.data() + begin, end - begin),
                {}, {});
            while (!cursor.at_end()) {
                auto value = cursor.value();
                const std::uint64_t index = fold_key_index(cursor.key());
                if (!value.has_value() || value->size() != FOLD_VALUE_BYTES ||
                    index >= keys) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const auto* bytes =
                    reinterpret_cast<const std::uint8_t*>(value->data());
                sums[static_cast<std::size_t>(index)] +=
                    static_cast<std::int64_t>(
                        dftracer::utils::read_u64_le(bytes));
                cursor.next();
            }
            if (cursor.error()) errors.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) worker.join();
    scan.nanos = nanos_between(started, std::chrono::steady_clock::now());
    if (errors.load() > 0) {
        fail(
            "fold scan: a key or a value of the fold namespace is not the one "
            "written");
        scan.ok = false;
        return scan;
    }
    for (std::uint64_t key = 0; key < keys; ++key) {
        const std::size_t at = static_cast<std::size_t>(key);
        std::int64_t total = 0;
        for (const std::vector<std::int64_t>& sums : parts) total += sums[at];
        if (total != expected[at]) ++scan.mismatches;
    }
    return scan;
}

FoldScan Bench::fold_scan_rocksdb(::rocksdb::DB& db,
                                  const std::vector<std::string>& prefixes,
                                  std::uint64_t keys, std::uint64_t threads,
                                  const std::vector<std::int64_t>& expected) {
    FoldScan scan;
    std::vector<std::vector<std::int64_t>> parts(
        static_cast<std::size_t>(threads),
        std::vector<std::int64_t>(static_cast<std::size_t>(keys), 0));
    std::atomic<int> errors{0};
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));
    for (std::uint64_t at = 0; at < threads; ++at) {
        workers.emplace_back([&, at] {
            const std::size_t part = static_cast<std::size_t>(at);
            const std::size_t count = static_cast<std::size_t>(threads);
            const std::size_t begin = prefixes.size() * part / count;
            const std::size_t end = prefixes.size() * (part + 1) / count;
            std::vector<std::int64_t>& sums = parts[part];
            // The work a query does with keys it cannot index: fold by the key
            // bytes in a hash map, then sort, so the part ends in key order as
            // ours does.
            std::unordered_map<std::string, std::int64_t> folded;
            ::rocksdb::ReadOptions read;
            std::unique_ptr<::rocksdb::Iterator> iterator(db.NewIterator(read));
            for (std::size_t scope = begin; scope < end; ++scope) {
                const ::rocksdb::Slice prefix(prefixes[scope]);
                iterator->Seek(prefix);
                while (iterator->Valid() &&
                       iterator->key().starts_with(prefix)) {
                    const ::rocksdb::Slice key = iterator->key();
                    const ::rocksdb::Slice value = iterator->value();
                    if (value.size() != FOLD_VALUE_BYTES ||
                        key.size() < prefix.size() + 1U) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    const auto* bytes =
                        reinterpret_cast<const std::uint8_t*>(value.data());
                    folded[std::string(key.data() + prefix.size(),
                                       key.size() - prefix.size() - 1U)] +=
                        static_cast<std::int64_t>(
                            dftracer::utils::read_u64_le(bytes));
                    iterator->Next();
                }
                if (!iterator->status().ok()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            std::vector<std::pair<std::string, std::int64_t>> ordered(
                folded.begin(), folded.end());
            std::sort(ordered.begin(), ordered.end());
            for (const auto& entry : ordered) {
                const std::uint64_t index = fold_key_index(
                    ByteView(entry.first.data(), entry.first.size()));
                if (index >= keys) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                sums[static_cast<std::size_t>(index)] += entry.second;
            }
        });
    }
    for (auto& worker : workers) worker.join();
    scan.nanos = nanos_between(started, std::chrono::steady_clock::now());
    if (errors.load() > 0) {
        fail("RocksDB fold scan: a key or a value is not the one written");
        scan.ok = false;
        return scan;
    }
    for (std::uint64_t key = 0; key < keys; ++key) {
        const std::size_t at = static_cast<std::size_t>(key);
        std::int64_t total = 0;
        for (const std::vector<std::int64_t>& sums : parts) total += sums[at];
        if (total != expected[at]) ++scan.mismatches;
    }
    return scan;
}

bool Bench::run_fold_case(Runtime& runtime, std::uint64_t scopes) {
    const std::uint64_t keys = opt.fold_keys;
    const std::vector<std::int64_t> expected = fold_expectation(scopes, keys);
    std::vector<ScopeKey> scope_keys;
    std::vector<std::string> prefixes;
    scope_keys.reserve(static_cast<std::size_t>(scopes));
    prefixes.reserve(static_cast<std::size_t>(scopes));
    for (std::uint64_t scope = 0; scope < scopes; ++scope) {
        scope_keys.push_back(ScopeKey::file(scope_name(0, scope)));
        prefixes.push_back(scope_prefix(0, scope));
    }
    std::error_code error;
    bool ok = true;

    if (has_engine(opt.engines, "ours")) {
        const std::string dir = case_dir("fold-ours-" + std::to_string(scopes));
        if (!write_fold_ours(runtime, dir, scopes, keys)) {
            ok = false;
        } else {
            const auto open_started = std::chrono::steady_clock::now();
            auto opened = Snapshot::open(dir);
            const std::uint64_t open_nanos =
                nanos_between(open_started, std::chrono::steady_clock::now());
            const double ms_open = static_cast<double>(open_nanos) / 1e6;
            if (!opened) {
                fail("snapshot of " + dir + ": " + to_std(opened.error()));
                ok = false;
            } else {
                Snapshot snapshot = std::move(*opened);
                const std::string scope_column =
                    "scopes=" + std::to_string(scopes) + " ";
                // This case prints its rows as they come, so the open rows of
                // this directory are printed here and not queued.
                print_query_row(query_row(
                    "ours", "open", 1, open_nanos,
                    dftu::bench::peak_heap_bytes(),
                    scope_column + format_stats(snapshot.open_stats(),
                                                snapshot.read_stats())));
                if (!report_single_thread_open(dir, scope_column, true)) {
                    ok = false;
                }
                const NamespaceSpec namespace_spec = fold_spec();
                for (const std::uint64_t threads : FOLD_THREADS) {
                    const FoldScan scan =
                        fold_scan_ours(snapshot, namespace_spec, scope_keys,
                                       keys, threads, expected);
                    FoldRow row;
                    row.engine = "ours";
                    row.scopes = scopes;
                    row.keys = keys;
                    row.threads = threads;
                    row.ms_open = ms_open;
                    row.ms_scan = static_cast<double>(scan.nanos) / 1e6;
                    row.reads = snapshot.read_stats();
                    row.ns_per_key = static_cast<double>(scan.nanos) /
                                     static_cast<double>(keys);
                    row.mismatches = scan.mismatches;
                    print_fold_row(row);
                    if (scan.mismatches > 0) {
                        fail("ours: " + std::to_string(scan.mismatches) +
                             " fold sums of " + dir +
                             " differ from the ones written");
                        ok = false;
                    }
                    if (!scan.ok) ok = false;
                }
            }
        }
        std::filesystem::remove_all(dir, error);
    }

    if (has_engine(opt.engines, "rocksdb")) {
        const std::string dir =
            case_dir("fold-rocksdb-" + std::to_string(scopes));
        if (!write_fold_rocksdb(dir, scopes, keys)) {
            ok = false;
        } else {
            const auto open_started = std::chrono::steady_clock::now();
            std::unique_ptr<::rocksdb::DB> db;
            auto status =
                ::rocksdb::DB::OpenForReadOnly(rocks_read_options(), dir, &db);
            const double ms_open =
                static_cast<double>(nanos_between(
                    open_started, std::chrono::steady_clock::now())) /
                1e6;
            if (!status.ok()) {
                fail("RocksDB read-only open of " + dir + ": " +
                     status.ToString());
                ok = false;
            } else {
                for (const std::uint64_t threads : FOLD_THREADS) {
                    const FoldScan scan = fold_scan_rocksdb(*db, prefixes, keys,
                                                            threads, expected);
                    FoldRow row;
                    row.engine = "rocksdb";
                    row.scopes = scopes;
                    row.keys = keys;
                    row.threads = threads;
                    row.ms_open = ms_open;
                    row.ms_scan = static_cast<double>(scan.nanos) / 1e6;
                    row.ns_per_key = static_cast<double>(scan.nanos) /
                                     static_cast<double>(keys);
                    row.mismatches = scan.mismatches;
                    print_fold_row(row);
                    if (scan.mismatches > 0) {
                        fail("rocksdb: " + std::to_string(scan.mismatches) +
                             " fold sums of " + dir +
                             " differ from the ones written");
                        ok = false;
                    }
                    if (!scan.ok) ok = false;
                }
            }
        }
        std::filesystem::remove_all(dir, error);
    }
    return ok;
}

bool Bench::run() {
    const bool print_writes = opt.which == "write" || opt.which == "all";
    const bool do_query = opt.which == "query" || opt.which == "all";
    const bool do_scopes = opt.which == "scopes" || opt.which == "all";
    const bool do_fold = opt.which == "fold" || opt.which == "all";
    const bool ours_on = has_engine(opt.engines, "ours");
    const bool rocksdb_on = has_engine(opt.engines, "rocksdb");
    // The query case reads what the shared write case wrote, so with no write
    // rows asked for it writes that one case and prints only the query rows.
    const std::vector<std::string> modes =
        print_writes ? opt.modes : std::vector<std::string>{"shared"};

    if (print_writes || do_query) {
        for (int threads : opt.threads) {
            Runtime runtime(static_cast<std::size_t>(threads));
            note_backend(runtime);
            const Workload workload =
                workload_of(opt, static_cast<std::uint64_t>(threads));
            std::string ours_dir;
            std::string rocksdb_dir;
            for (const std::string& mode : modes) {
                const bool per_thread = mode == "per-thread";
                const bool keep_dir = do_query && !per_thread;
                const std::string name = mode + "-" + std::to_string(threads) +
                                         "-" + std::to_string(opt.payload_mib);
                if (ours_on) {
                    const WriteRow row = run_ours_write_case(
                        runtime, workload, case_dir("ours-" + name), per_thread,
                        keep_dir);
                    if (print_writes) print_write_row(row);
                    if (keep_dir) ours_dir = row.keep_dir;
                }
                if (rocksdb_on) {
                    const WriteRow row = run_rocksdb_write_case(
                        workload, case_dir("rocksdb-" + name), per_thread,
                        keep_dir);
                    if (print_writes) print_write_row(row);
                    if (keep_dir) rocksdb_dir = row.keep_dir;
                }
            }
            if (do_query) {
                run_query_case(workload, ours_dir, rocksdb_dir);
                std::error_code error;
                if (!ours_dir.empty()) {
                    std::filesystem::remove_all(ours_dir, error);
                }
                if (!rocksdb_dir.empty()) {
                    std::filesystem::remove_all(rocksdb_dir, error);
                }
            }
        }
    }
    for (const QueryRow& row : query_rows) print_query_row(row);
    if (do_scopes) {
        if (!ours_on) {
            ensure_comment();
            std::cout << "# the scopes case commits through ours only; skipped"
                         " with --engine rocksdb\n";
        } else {
            Runtime runtime(SCOPE_CASE_THREADS);
            note_backend(runtime);
            for (std::uint64_t scopes : opt.scopes) {
                const ScopeRow row = run_scope_case(runtime, scopes);
                print_scope_row(row);
            }
        }
    }
    if (do_fold) {
        Runtime runtime(SCOPE_CASE_THREADS);
        note_backend(runtime);
        for (std::uint64_t scopes : opt.fold_scopes) {
            if (!run_fold_case(runtime, scopes)) failed = true;
        }
    }
    return !failed;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        usage();
        return 2;
    }
    if (options.help) {
        usage();
        return 0;
    }
    Bench bench;
    bench.opt = options;
    if (!bench.setup()) {
        bench.cleanup();
        return 1;
    }
    const bool ok = bench.run();
    bench.cleanup();
    return ok ? 0 : 1;
}
