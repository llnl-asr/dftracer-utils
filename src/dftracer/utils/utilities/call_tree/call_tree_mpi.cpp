#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/call_tree/internal/process_call_tree.h>
#include <dftracer/utils/call_tree/internal/process_key.h>
#include <dftracer/utils/call_tree/internal/trace_reader.h>
#include <dftracer/utils/call_tree/json_serializer.h>
#include <dftracer/utils/call_tree/mpi/builder.h>
#include <dftracer/utils/core/common/byte_view.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/utilities/fileio/parallel/merge.h>
#include <dftracer/utils/utilities/fileio/parallel/parallel_writer.h>
#include <dftracer/utils/utilities/reader/trace_reader.h>
#include <mpi.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace dftracer::utils::call_tree {

namespace {

bool is_trace_file(const std::string& path) {
    return (path.size() >= 4 &&
            path.compare(path.size() - 4, 4, ".pfw") == 0) ||
           (path.size() >= 7 &&
            path.compare(path.size() - 7, 7, ".pfw.gz") == 0);
}

coro::CoroTask<void> scan_file_pids(std::string path,
                                    std::set<std::uint32_t>* out);
coro::CoroTask<void> ingest_file(std::string path, internal::CallTree* tree,
                                 const std::set<std::uint32_t>* pids,
                                 std::atomic<std::size_t>* total);
coro::CoroTask<void> build_hierarchy_one(internal::CallTree* tree,
                                         internal::ProcessKey key);
coro::CoroTask<void> serialize_one(const internal::CallTree* tree,
                                   internal::ProcessKey key,
                                   const std::string* hostname_hash,
                                   std::vector<std::string>* slice_buffers,
                                   std::size_t index, std::uint64_t start_idx);

coro::CoroTask<void> scan_files_into(
    CoroScope* child, const std::vector<std::string>* paths,
    std::vector<std::set<std::uint32_t>>* per_file);
coro::CoroTask<void> ingest_files_into(
    CoroScope* child, const std::vector<std::string>* paths,
    const std::vector<std::unique_ptr<internal::CallTree>>* per_file,
    const std::set<std::uint32_t>* pids, std::atomic<std::size_t>* total);
coro::CoroTask<void> hierarchy_all(
    CoroScope* child, internal::CallTree* tree,
    const std::vector<internal::ProcessKey>* keys);
coro::CoroTask<void> serialize_all(
    CoroScope* child, const internal::CallTree* tree,
    const std::vector<internal::ProcessKey>* keys,
    const std::string* hostname_hash, std::vector<std::string>* slice_buffers,
    std::uint64_t rank_base, std::uint64_t stride);

coro::CoroTask<void> scan_file_pids(std::string path,
                                    std::set<std::uint32_t>* out) {
    using utilities::reader::ReadConfig;
    using utilities::reader::TraceReader;
    using utilities::reader::TraceReaderConfig;
    TraceReaderConfig cfg;
    cfg.file_path = std::move(path);
    cfg.auto_build_index = true;
    TraceReader reader(std::move(cfg));
    auto gen = reader.read_json(ReadConfig{});
    while (auto opt = co_await gen.next()) {
        auto pid = opt->parser->get_uint64("pid");
        if (pid) out->insert(static_cast<std::uint32_t>(*pid));
    }
}

coro::CoroTask<void> scan_files_into(
    CoroScope* child, const std::vector<std::string>* paths,
    std::vector<std::set<std::uint32_t>>* per_file) {
    for (std::size_t k = 0; k < paths->size(); ++k) {
        std::string path = (*paths)[k];
        std::set<std::uint32_t>* out = &(*per_file)[k];
        child->spawn([path = std::move(path),
                      out](CoroScope&) mutable -> coro::CoroTask<void> {
            co_await scan_file_pids(std::move(path), out);
        });
    }
    co_return;
}

coro::CoroTask<void> ingest_files_into(
    CoroScope* child, const std::vector<std::string>* paths,
    const std::vector<std::unique_ptr<internal::CallTree>>* per_file,
    const std::set<std::uint32_t>* pids, std::atomic<std::size_t>* total) {
    for (std::size_t i = 0; i < paths->size(); ++i) {
        std::string path = (*paths)[i];
        internal::CallTree* tree = (*per_file)[i].get();
        child->spawn([path = std::move(path), tree, pids,
                      total](CoroScope&) mutable -> coro::CoroTask<void> {
            co_await ingest_file(std::move(path), tree, pids, total);
        });
    }
    co_return;
}

coro::CoroTask<void> hierarchy_all(
    CoroScope* child, internal::CallTree* tree,
    const std::vector<internal::ProcessKey>* keys) {
    for (auto k : *keys) {
        child->spawn([tree, k](CoroScope&) mutable -> coro::CoroTask<void> {
            co_await build_hierarchy_one(tree, k);
        });
    }
    co_return;
}

coro::CoroTask<void> serialize_all(
    CoroScope* child, const internal::CallTree* tree,
    const std::vector<internal::ProcessKey>* keys,
    const std::string* hostname_hash, std::vector<std::string>* slice_buffers,
    std::uint64_t rank_base, std::uint64_t stride) {
    for (std::size_t i = 0; i < keys->size(); ++i) {
        internal::ProcessKey k = (*keys)[i];
        std::uint64_t start_idx = rank_base + i * stride;
        child->spawn([tree, k, start_idx, i, hostname_hash, slice_buffers](
                         CoroScope&) mutable -> coro::CoroTask<void> {
            co_await serialize_one(tree, k, hostname_hash, slice_buffers, i,
                                   start_idx);
        });
    }
    co_return;
}

coro::CoroTask<void> ingest_file(std::string path, internal::CallTree* tree,
                                 const std::set<std::uint32_t>* pids,
                                 std::atomic<std::size_t>* total) {
    auto counts =
        co_await internal::read_trace_file_async(std::move(path), tree, pids);
    total->fetch_add(counts.processed, std::memory_order_relaxed);
}

coro::CoroTask<void> build_hierarchy_one(internal::CallTree* tree,
                                         internal::ProcessKey key) {
    tree->build_hierarchy_for_process(key);
    co_return;
}

void serialize_process_dfs(const internal::ProcessCallTree& pgraph,
                           const internal::ProcessKey& key,
                           internal::JsonSerializer& serializer,
                           std::uint64_t start_idx, std::string& out) {
    char buffer[16384];
    std::uint64_t idx = start_idx;
    for (std::uint64_t root_id : pgraph.root_calls) {
        std::vector<std::uint64_t> stack;
        stack.push_back(root_id);
        while (!stack.empty()) {
            std::uint64_t nid = stack.back();
            stack.pop_back();
            auto it = pgraph.calls.find(nid);
            if (it == pgraph.calls.end()) continue;
            const auto& node = it->second;
            std::size_t w = serializer.serialize_node(
                buffer, static_cast<int>(idx++), *node, key.pid, key.tid);
            if (w > 0) {
                out.append(buffer, w - 1);
                out.append(",\n", 2);
            }
            const auto& children = node->get_children();
            for (auto cit = children.rbegin(); cit != children.rend(); ++cit)
                stack.push_back(*cit);
        }
    }
}

coro::CoroTask<void> serialize_one(const internal::CallTree* tree,
                                   internal::ProcessKey key,
                                   const std::string* hostname_hash,
                                   std::vector<std::string>* slice_buffers,
                                   std::size_t index, std::uint64_t start_idx) {
    auto* pgraph = const_cast<internal::CallTree*>(tree)->get(key);
    if (pgraph) {
        internal::JsonSerializer serializer;
        char init[8];
        serializer.initialize(init, *hostname_hash);
        (void)init;
        serialize_process_dfs(*pgraph, key, serializer, start_idx,
                              (*slice_buffers)[index]);
    }
    co_return;
}

}  // namespace

MPICallTreeBuilder::MPICallTreeBuilder(const MPICallTreeConfig& config)
    : config_(config), call_tree_(std::make_unique<internal::CallTree>()) {
    MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size_);
    call_tree_->initialize();
}

MPICallTreeBuilder::~MPICallTreeBuilder() = default;

MPICallTreeBuilder::MPICallTreeBuilder(MPICallTreeBuilder&&) noexcept = default;
MPICallTreeBuilder& MPICallTreeBuilder::operator=(
    MPICallTreeBuilder&&) noexcept = default;

void MPICallTreeBuilder::add_trace_files(
    const std::vector<std::string>& files) {
    trace_files_.insert(trace_files_.end(), files.begin(), files.end());
    std::sort(trace_files_.begin(), trace_files_.end());
}

void MPICallTreeBuilder::add_trace_directory(const std::string& directory,
                                             const std::string& /*pattern*/) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (is_trace_file(entry.path().string()))
            files.push_back(entry.path().string());
    }
    add_trace_files(files);
}

namespace {

struct DiscoverState {
    std::vector<std::string> my_paths;
    std::vector<std::set<std::uint32_t>> per_file;
};

coro::CoroTask<void> discover_scan_phase(CoroScope* scope, DiscoverState* st) {
    const std::vector<std::string>* paths_ptr = &st->my_paths;
    std::vector<std::set<std::uint32_t>>* per_file_ptr = &st->per_file;
    co_await scope->scope(
        [paths_ptr,
         per_file_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await scan_files_into(&child, paths_ptr, per_file_ptr);
        });
}

}  // namespace

coro::CoroTask<bool> MPICallTreeBuilder::discover_pids(CoroScope* scope) {
    auto state = std::make_unique<DiscoverState>();
    for (std::size_t i = 0; i < trace_files_.size(); ++i) {
        if (static_cast<int>(i % static_cast<std::size_t>(world_size_)) ==
            rank_)
            state->my_paths.push_back(trace_files_[i]);
    }
    state->per_file.resize(state->my_paths.size());

    co_await discover_scan_phase(scope, state.get());

    std::set<std::uint32_t> local_pids;
    for (auto& s : state->per_file) local_pids.insert(s.begin(), s.end());
    state.reset();

    std::vector<std::uint32_t> local_vec(local_pids.begin(), local_pids.end());
    int my_bytes = static_cast<int>(local_vec.size() * sizeof(std::uint32_t));
    std::vector<int> rank_bytes(world_size_, 0);
    MPI_Allgather(&my_bytes, 1, MPI_INT, rank_bytes.data(), 1, MPI_INT,
                  MPI_COMM_WORLD);
    std::vector<int> displs(world_size_, 0);
    int total = 0;
    for (int r = 0; r < world_size_; ++r) {
        displs[r] = total;
        total += rank_bytes[r];
    }
    std::vector<char> gathered(total);
    MPI_Allgatherv(local_vec.data(), my_bytes, MPI_CHAR, gathered.data(),
                   rank_bytes.data(), displs.data(), MPI_CHAR, MPI_COMM_WORLD);
    for (int r = 0; r < world_size_; ++r) {
        const auto* p =
            reinterpret_cast<const std::uint32_t*>(gathered.data() + displs[r]);
        const std::size_t n = rank_bytes[r] / sizeof(std::uint32_t);
        for (std::size_t i = 0; i < n; ++i) all_pids_.insert(p[i]);
    }

    std::vector<std::uint32_t> sorted_pids(all_pids_.begin(), all_pids_.end());
    for (std::size_t i = static_cast<std::size_t>(rank_);
         i < sorted_pids.size(); i += static_cast<std::size_t>(world_size_)) {
        assigned_pids_.insert(sorted_pids[i]);
    }

    if (rank_ == 0) {
        DFTRACER_UTILS_LOG_DEBUG(
            "[rank 0] discovered %zu unique pids across %zu "
            "files",
            all_pids_.size(), trace_files_.size());
    }
    co_return true;
}

coro::CoroTask<bool> MPICallTreeBuilder::build(CoroScope* scope) {
    if (assigned_pids_.empty()) co_return true;

    const std::size_t n = trace_files_.size();
    std::vector<std::unique_ptr<internal::CallTree>> per_file;
    per_file.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        per_file.push_back(std::make_unique<internal::CallTree>());
        per_file.back()->initialize();
    }

    const std::set<std::uint32_t>* pids_ptr = &assigned_pids_;
    std::atomic<std::size_t> total_events{0};
    std::atomic<std::size_t>* total_ptr = &total_events;

    const std::vector<std::string>* paths_ptr = &trace_files_;
    const std::vector<std::unique_ptr<internal::CallTree>>* per_file_ptr =
        &per_file;

    co_await scope->scope(
        [paths_ptr, per_file_ptr, pids_ptr,
         total_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await ingest_files_into(&child, paths_ptr, per_file_ptr,
                                       pids_ptr, total_ptr);
        });

    for (auto& t : per_file)
        if (t) call_tree_->merge_from(std::move(*t));
    my_process_keys_ = call_tree_->keys();

    DFTRACER_UTILS_LOG_DEBUG(
        "[rank %d/%d] build done: %zu events, %zu processes", rank_,
        world_size_, total_events.load(), my_process_keys_.size());
    co_return true;
}

coro::CoroTask<bool> MPICallTreeBuilder::hierarchy(CoroScope* scope) {
    internal::CallTree* tree = call_tree_.get();
    const std::vector<internal::ProcessKey>* keys_ptr = &my_process_keys_;
    co_await scope->scope(
        [tree, keys_ptr](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await hierarchy_all(&child, tree, keys_ptr);
        });
    co_return true;
}

coro::CoroTask<bool> MPICallTreeBuilder::write(CoroScope* scope,
                                               std::string /*output_path*/,
                                               std::string staging_dir,
                                               bool gzip) {
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), "/rank_%05d.pfw%s", rank_,
                  gzip ? ".gz" : "");
    my_shard_path_ = staging_dir + suffix;
    if (rank_ == 0) {
        std::error_code ec;
        fs::create_directories(staging_dir, ec);
    }
    MPI_Barrier(MPI_COMM_WORLD);

    const std::size_t n = my_process_keys_.size();
    std::vector<std::string> slice_buffers(n);
    static constexpr std::uint64_t IDX_STRIDE = 1ull << 20;

    char hostname[256] = {};
    gethostname(hostname, sizeof(hostname) - 1);
    std::string hostname_hash(hostname);

    std::vector<std::string>* slice_buffers_ptr = &slice_buffers;
    const std::string* hostname_hash_ptr = &hostname_hash;
    const internal::CallTree* tree = call_tree_.get();
    const std::uint64_t rank_base = static_cast<std::uint64_t>(rank_) << 40;
    const std::vector<internal::ProcessKey>* keys_ptr = &my_process_keys_;

    co_await scope->scope(
        [tree, keys_ptr, hostname_hash_ptr, slice_buffers_ptr,
         rank_base](CoroScope& child) mutable -> coro::CoroTask<void> {
            co_await serialize_all(&child, tree, keys_ptr, hostname_hash_ptr,
                                   slice_buffers_ptr, rank_base, IDX_STRIDE);
        });

    std::string header;
    if (rank_ == 0) {
        header.append("[\n", 2);
        internal::JsonSerializer serializer;
        char init[8];
        serializer.initialize(init, hostname_hash);
        (void)init;
        char buf[8192];
        std::time_t now = std::time(nullptr);
        char ts[64];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S",
                      std::localtime(&now));
        std::size_t w = serializer.serialize_metadata(buf, "timestamp", ts, "M",
                                                      0, 0, true);
        if (w > 0) header.append(buf, w - 1);
        header.append(",\n", 2);
        w = serializer.serialize_metadata(buf, "format", "call_tree", "M", 0, 0,
                                          true);
        if (w > 0) header.append(buf, w - 1);
        header.append(",\n", 2);
    }

    utilities::fileio::parallel::WriterConfig wc;
    wc.layout = utilities::fileio::parallel::FileLayout::SHARDED;
    wc.gzip = gzip;
    auto writer = utilities::fileio::parallel::make_writer(wc);

    const std::size_t total_workers = (rank_ == 0 ? 1 : 0) + n;
    if (total_workers == 0) {
        FILE* f = std::fopen(my_shard_path_.c_str(), "wb");
        if (f) std::fclose(f);
        co_return true;
    }

    if (co_await writer->open(my_shard_path_, total_workers, gzip, scope) !=
        0) {
        DFTRACER_UTILS_LOG_ERROR("[rank %d] failed to open writer: %s", rank_,
                                 my_shard_path_.c_str());
        co_return false;
    }

    std::size_t widx = 0;
    if (rank_ == 0) {
        if (co_await writer->write_chunk(
                widx++, ByteView(header.data(), header.size())) != 0) {
            co_return false;
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        std::string& b = slice_buffers[i];
        const bool last_overall = (i + 1 == n) && (rank_ == world_size_ - 1);
        if (last_overall) {
            if (b.size() >= 2 && b[b.size() - 2] == ',' &&
                b[b.size() - 1] == '\n') {
                b.resize(b.size() - 2);
                b.append("\n]\n", 3);
            } else {
                b.append("]\n", 2);
            }
        }
        if (co_await writer->write_chunk(widx++,
                                         ByteView(b.data(), b.size())) != 0) {
            co_return false;
        }
    }

    if (co_await writer->close() != 0) co_return false;

    auto shards = writer->output_paths();
    if (shards.size() > 1) {
        if (co_await utilities::fileio::parallel::merge_shards(my_shard_path_,
                                                               shards) != 0) {
            DFTRACER_UTILS_LOG_ERROR("[rank %d] local merge failed", rank_);
            co_return false;
        }
    }
    co_return true;
}

coro::CoroTask<bool> MPICallTreeBuilder::merge(std::string output_path,
                                               std::string staging_dir,
                                               bool gzip, bool keep_staging) {
    MPI_Barrier(MPI_COMM_WORLD);
    if (rank_ != 0) co_return true;

    std::vector<std::string> shards;
    shards.reserve(world_size_);
    for (int r = 0; r < world_size_; ++r) {
        char rs[64];
        std::snprintf(rs, sizeof(rs), "/rank_%05d.pfw%s", r, gzip ? ".gz" : "");
        shards.emplace_back(staging_dir + rs);
    }
    if (co_await utilities::fileio::parallel::merge_shards(output_path,
                                                           shards) != 0) {
        DFTRACER_UTILS_LOG_ERROR("merge_shards failed for %s",
                                 output_path.c_str());
        co_return false;
    }
    if (!keep_staging) {
        std::error_code ec;
        fs::remove_all(staging_dir, ec);
    }
    co_return true;
}

}  // namespace dftracer::utils::call_tree
