#include <dftracer/utils/core/common/logging.h>
#include <dftracer/utils/core/common/transparent_string_hash.h>
#include <dftracer/utils/index/build/index_write_lock.h>
#include <dftracer/utils/index/extensions/dictionary_rows.h>
#include <dftracer/utils/index/store/index_database.h>
#include <dftracer/utils/index/store/index_database_writer_context.h>
#include <dftracer/utils/index/store/index_write.h>

#include <exception>
#include <mutex>
#include <utility>
#include <vector>

namespace dftracer::utils::index::extensions {

namespace {

// The row key [dict][0x00][key]; split_entry inverts it.
std::string entry_key(std::string_view dict, std::string_view key) {
    std::string k(dict);
    k.push_back('\0');
    k.append(key);
    return k;
}

std::pair<std::string_view, std::string_view> split_entry(
    std::string_view entry) {
    const auto sep = entry.find('\0');
    return {entry.substr(0, sep), entry.substr(sep + 1)};
}

}  // namespace

void DictionaryRows::add(std::string_view dict, std::string_view key,
                         const Fields& fields) {
    if (abandoned_) return;
    auto k = entry_key(dict, key);
    bytes_ += k.size();
    for (const auto& [name, value] : fields)
        bytes_ += name.size() + value.size();
    pending_.insert_or_assign(std::move(k), fields);

    // Over budget: stop building rather than slow the scan or grow unbounded.
    // The query is unaffected.
    if (bytes_ > budget_) {
        abandoned_ = true;
        pending_.clear();
        sealed_.clear();
        entry_file_.clear();
    }
}

void DictionaryRows::seal_unit(std::string_view file_path) {
    if (abandoned_) return;
    for (auto& [k, v] : pending_) {
        entry_file_.insert_or_assign(k, std::string(file_path));
        sealed_.insert_or_assign(k, std::move(v));
    }
    pending_.clear();
}

void DictionaryRows::merge(DictionaryRows& other) {
    if (other.abandoned_) {
        abandoned_ = true;
        sealed_.clear();
        entry_file_.clear();
        return;
    }
    if (abandoned_) return;
    if (index_path_.empty()) index_path_ = other.index_path_;
    for (auto& [k, v] : other.sealed_) {
        auto it = other.entry_file_.find(k);
        if (it != other.entry_file_.end())
            entry_file_.insert_or_assign(k, it->second);
        sealed_.insert_or_assign(k, std::move(v));
    }
    other.sealed_.clear();
}

coro::CoroTask<bool> DictionaryRows::commit(
    const trace::views::detail::CoverageSet& covered) {
    if (abandoned_ || sealed_.empty() || index_path_.empty()) co_return false;

    // Only files the scan read whole: a partially read file may define a hash
    // in a member never visited, and a missing entry resolves to an empty name
    // rather than failing.
    std::vector<std::pair<std::string, Fields>> writable;
    writable.reserve(sealed_.size());
    for (const auto& [k, v] : sealed_) {
        auto it = entry_file_.find(k);
        if (it == entry_file_.end()) continue;
        if (!covered.covers_file(it->second)) continue;
        writable.emplace_back(k, v);
    }
    if (writable.empty()) co_return false;

    // Drop what the index already holds, so the cost scales with new rows
    // rather than total ones. A read open takes no exclusive lock, so this
    // avoids the write open entirely when nothing is new.
    try {
        index::store::IndexDatabase ro(index_path_,
                                       index::store::IndexOpenMode::ReadOnly);
        std::erase_if(writable, [&](const auto& entry) {
            const auto [dict, key] = split_entry(entry.first);
            return ro.dict_row(dict, key).has_value();
        });
    } catch (const std::exception& e) {
        // Unreadable index: it is not known what is already there, so writing
        // blind would be unbounded waste. Declining is the conservative side,
        // and must not be silent - this is a skip, not an empty dictionary.
        DFTRACER_UTILS_LOG_WARN("dictionary delta check failed for %s: %s",
                                index_path_.c_str(), e.what());
        co_return false;
    }
    if (writable.empty()) co_return false;

    // A write open takes RocksDB's exclusive directory lock, so concurrent
    // committers would fight over it. Serialize them per index instead of
    // caching a handle: a cached writer would hold the lock for the process
    // lifetime and block the indexer, which runs in the same process as the
    // query in dfanalyzer.
    std::unique_lock<std::mutex> lk(
        index::build::index_write_mutex(index_path_));

    try {
        index::store::IndexDatabase db(index_path_);
        auto writer = db.begin_write();
        for (const auto& [k, fields] : writable) {
            const auto [dict, key] = split_entry(k);
            index::store::records::put_dict_row(*writer, dict, key, fields);
        }
        writer->commit();
    } catch (const std::exception& e) {
        // A read-only or already-locked index must degrade to "answered the
        // query, persisted nothing", never to a failed query - but it must not
        // look like "there was nothing to persist".
        DFTRACER_UTILS_LOG_WARN("dictionary materialization skipped for %s: %s",
                                index_path_.c_str(), e.what());
        co_return false;
    }
    co_return true;
}

void DictionaryRows::write(index::store::IndexWrite& w) const {
    for (const auto& [k, fields] : sealed_) {
        const auto [dict, key] = split_entry(k);
        index::store::records::put_dict_row(w, dict, key, fields);
    }
}

}  // namespace dftracer::utils::index::extensions
