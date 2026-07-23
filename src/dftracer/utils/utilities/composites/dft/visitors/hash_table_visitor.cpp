#include <dftracer/utils/utilities/composites/dft/visitors/hash_table_visitor.h>
#include <dftracer/utils/utilities/composites/dft/visitors/visitor_dom_helpers.h>
#include <dftracer/utils/utilities/indexer/index_batch_sink.h>

namespace dftracer::utils::utilities::composites::dft::visitors {

namespace {
// While streaming, flush once the buffered entries cross this count. Two puts
// (forward + reverse) are written per entry.
constexpr std::size_t STREAM_FLUSH_ENTRIES = 65536;
}  // namespace

void HashTableVisitor::begin(std::size_t /*num_checkpoints*/) {
    file_hashes_.clear();
    host_hashes_.clear();
    string_hashes_.clear();
    proc_metadata_.clear();
}

void HashTableVisitor::on_checkpoint(std::size_t /*checkpoint_idx*/) {}

void HashTableVisitor::on_event(const EventRecord& record) {
    const auto& ev = record.ev;
    if (!ev.is_metadata()) {
        return;
    }
    if (!record.has_args) {
        return;
    }

    auto name_val = dom_string(record.args_dom, "name");
    auto hash_val = dom_string(record.args_dom, "value");

    if (name_val.empty() || hash_val.empty()) {
        return;
    }

    if (ev.name == "FH") {
        file_hashes_.try_emplace(std::string(hash_val), std::string(name_val));
    } else if (ev.name == "HH") {
        host_hashes_.try_emplace(std::string(hash_val), std::string(name_val));
    } else if (ev.name == "SH") {
        string_hashes_.try_emplace(std::string(hash_val),
                                   std::string(name_val));
    } else if (ev.name == "PR") {
        proc_metadata_.try_emplace(std::string(hash_val),
                                   std::string(name_val));
    }

    if (stream_sink_ && num_entries() >= STREAM_FLUSH_ENTRIES) {
        flush_to_stream();
    }
}

void HashTableVisitor::flush_to_stream() {
    if (!file_hashes_.empty()) stream_sink_(HashType::FILE, file_hashes_);
    if (!host_hashes_.empty()) stream_sink_(HashType::HOST, host_hashes_);
    if (!string_hashes_.empty()) stream_sink_(HashType::STRING, string_hashes_);
    if (!proc_metadata_.empty()) stream_sink_(HashType::PROC, proc_metadata_);
    file_hashes_.clear();
    host_hashes_.clear();
    string_hashes_.clear();
    proc_metadata_.clear();
}

std::unique_ptr<DftEventVisitor> HashTableVisitor::create_parallel_slice()
    const {
    return std::make_unique<HashTableVisitor>(stream_sink_);
}

void HashTableVisitor::merge_parallel_slice(DftEventVisitor& slice_base) {
    auto* slice = dynamic_cast<HashTableVisitor*>(&slice_base);
    if (!slice) return;
    // A streaming slice wrote as it filled; flush what it still holds to the
    // same destination rather than pulling it into this visitor's buffer.
    if (stream_sink_) {
        slice->flush_to_stream();
        return;
    }
    auto absorb = [](HashMap& dst, HashMap& src) {
        for (auto& [k, v] : src) {
            dst.try_emplace(std::move(const_cast<std::string&>(k)),
                            std::move(v));
        }
    };
    absorb(file_hashes_, slice->file_hashes_);
    absorb(host_hashes_, slice->host_hashes_);
    absorb(string_hashes_, slice->string_hashes_);
    absorb(proc_metadata_, slice->proc_metadata_);
}

void HashTableVisitor::finalize(indexer::IndexBatchSink& writer,
                                int /*file_id*/) {
    if (stream_sink_) {
        flush_to_stream();
        return;
    }
    auto write_entries = [&writer](const HashMap& entries, HashType type) {
        for (const auto& [hash, name] : entries) {
            writer.insert_hash_table_entry(static_cast<std::uint8_t>(type),
                                           hash, name);
        }
    };

    write_entries(file_hashes_, HashType::FILE);
    write_entries(host_hashes_, HashType::HOST);
    write_entries(string_hashes_, HashType::STRING);
    write_entries(proc_metadata_, HashType::PROC);
}

std::size_t HashTableVisitor::num_entries() const {
    return file_hashes_.size() + host_hashes_.size() + string_hashes_.size() +
           proc_metadata_.size();
}

}  // namespace dftracer::utils::utilities::composites::dft::visitors
