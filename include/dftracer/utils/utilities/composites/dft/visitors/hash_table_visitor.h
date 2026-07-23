#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_HASH_TABLE_VISITOR_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_HASH_TABLE_VISITOR_H

#include <dftracer/utils/utilities/composites/dft/dft_event_visitor.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace dftracer::utils::utilities::indexer {
class IndexBatchSink;
}

namespace dftracer::utils::utilities::composites::dft::visitors {

/// Captures FH/HH/SH/PR metadata events during indexing and stores them
/// in HASH_TABLES column family with bidirectional lookups:
///   - Forward (hash -> name): for resolving hashes in output
///   - Reverse (name -> hash): for query DSL like `file_name == "/path/..."`
class HashTableVisitor : public DftEventVisitor {
   public:
    /// Hash table types matching dfanalyzer naming conventions
    enum class HashType : std::uint8_t {
        FILE = 0,    // fhash <-> file_name
        HOST = 1,    // hhash <-> host_name
        STRING = 2,  // shash <-> string value
        PROC = 3     // phash <-> proc metadata
    };

    using HashMap = std::unordered_map<std::string, std::string>;
    using StreamSink = std::function<void(HashType, const HashMap&)>;

    HashTableVisitor() = default;
    explicit HashTableVisitor(StreamSink stream_sink)
        : stream_sink_(std::move(stream_sink)) {}
    HashTableVisitor(const HashTableVisitor&) = delete;
    HashTableVisitor& operator=(const HashTableVisitor&) = delete;
    HashTableVisitor(HashTableVisitor&&) noexcept = default;
    HashTableVisitor& operator=(HashTableVisitor&&) noexcept = default;

    void begin(std::size_t num_checkpoints) override;
    void on_checkpoint(std::size_t checkpoint_idx) override;
    void on_event(const EventRecord& record) override;

    std::unique_ptr<DftEventVisitor> create_parallel_slice() const override;
    void merge_parallel_slice(DftEventVisitor& slice) override;

    void finalize(indexer::IndexBatchSink& writer, int file_id);

    std::size_t num_entries() const;

   private:
    void flush_to_stream();

    HashMap file_hashes_;
    HashMap host_hashes_;
    HashMap string_hashes_;
    HashMap proc_metadata_;
    StreamSink stream_sink_;
};

}  // namespace dftracer::utils::utilities::composites::dft::visitors

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_VISITORS_HASH_TABLE_VISITOR_H
