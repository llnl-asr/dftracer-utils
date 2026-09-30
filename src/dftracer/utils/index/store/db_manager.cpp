#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/index/store/db_manager.h>

#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace dftracer::utils::index::store {

namespace {
// Close retained handles at exit while RocksDB's globals are still alive.
void ensure_exit_cleanup() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::atexit([] { RocksDBManager::instance().shutdown(); });
    });
}
}  // namespace

RocksDBManager& RocksDBManager::instance() {
    // Never destroyed: a handle's deleter reaches it however late the handle
    // dies.
    static auto* manager = new RocksDBManager();
    return *manager;
}

std::size_t RocksDBManager::retain_cap() {
    if (retain_cap_ == 0) {
        auto v = Env::get<int>("DFTRACER_UTILS_ROCKSDB_CACHE");
        retain_cap_ = (v && *v > 0) ? static_cast<std::size_t>(*v) : 64;
    }
    return retain_cap_;
}

void RocksDBManager::retain_locked(const std::string& db_path,
                                   const std::shared_ptr<RocksDatabase>& db) {
    if (!db || !db->is_read_only()) return;
    ensure_exit_cleanup();
    if (auto it = retained_index_.find(db_path); it != retained_index_.end()) {
        it->second->second = db;
        retained_.splice(retained_.begin(), retained_, it->second);
        return;
    }
    retained_.emplace_front(db_path, db);
    retained_index_[db_path] = retained_.begin();
    while (retained_.size() > retain_cap()) {
        retained_index_.erase(retained_.back().first);
        retained_.pop_back();
    }
}

void RocksDBManager::drop_retained_locked(const std::string& db_path) {
    if (auto it = retained_index_.find(db_path); it != retained_index_.end()) {
        retained_.erase(it->second);
        retained_index_.erase(it);
    }
}

namespace {
std::mutex& reset_listeners_mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::function<void(const std::string&)>>& reset_listeners() {
    static std::vector<std::function<void(const std::string&)>> v;
    return v;
}

// Lets path-keyed caches drop their handles; call outside mutex_, since a
// listener may re-enter the manager.
void notify_reset_listeners(const std::string& db_path) {
    std::lock_guard<std::mutex> lk(reset_listeners_mutex());
    for (auto& fn : reset_listeners())
        if (fn) fn(db_path);
}
}  // namespace

std::shared_ptr<RocksDatabase> RocksDBManager::get_or_open(
    const std::string& db_path, RocksDatabase::OpenMode open_mode,
    RocksDatabase::CfOptionsOverride cf_override) {
    bool caches_released = false;
    for (;;) {
        bool needs_upgrade = false;
        bool do_open = false;
        bool release_caches = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            for (;;) {
                if (auto it = databases_.find(db_path);
                    it != databases_.end()) {
                    auto current = it->second.lock();
                    if (!current) {
                        databases_.erase(it);
                        continue;
                    }
                    if (!(current->is_read_only() &&
                          open_mode == RocksDatabase::OpenMode::ReadWrite)) {
                        retain_locked(db_path, current);
                        return current;
                    }

                    if (opening_.contains(db_path)) {
                        cv_.wait(lock,
                                 [&] { return !opening_.contains(db_path); });
                        continue;
                    }

                    // Drop our retained ref so the count reflects only external
                    // holders before deciding an upgrade is safe.
                    drop_retained_locked(db_path);
                    if (current.use_count() != 1) {
                        // A cache may hold the only other reference; let the
                        // caches release it once before refusing.
                        if (!caches_released) {
                            release_caches = true;
                            break;
                        }
                        throw DFTUtilsException(
                            ErrorCode::INVALID_ARGUMENT,
                            "Cannot upgrade RocksDB instance at '" + db_path +
                                "' from read-only to read-write while it is "
                                "still "
                                "in use");
                    }

                    needs_upgrade = true;
                    opening_.insert(db_path);
                    do_open = true;
                    break;
                }

                if (opening_.contains(db_path) ||
                    (open_mode == RocksDatabase::OpenMode::ReadWrite &&
                     writers_.contains(db_path))) {
                    cv_.wait(lock, [&] {
                        return !opening_.contains(db_path) &&
                               (open_mode !=
                                    RocksDatabase::OpenMode::ReadWrite ||
                                !writers_.contains(db_path));
                    });
                    continue;
                }

                opening_.insert(db_path);
                do_open = true;
                break;
            }
        }

        if (release_caches) {
            caches_released = true;
            notify_reset_listeners(db_path);
            continue;
        }
        if (!do_open) {
            continue;
        }

        const bool writes =
            needs_upgrade || open_mode == RocksDatabase::OpenMode::ReadWrite;
        if (writes) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++writers_[db_path];
        }
        std::shared_ptr<RocksDatabase> database;
        try {
            database = std::shared_ptr<RocksDatabase>(
                new RocksDatabase(), [this, writes, db_path](RocksDatabase* p) {
                    delete p;
                    if (!writes) return;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (auto it = writers_.find(db_path);
                        it != writers_.end() && --it->second == 0)
                        writers_.erase(it);
                    cv_.notify_all();
                });
            if (cf_override) {
                database->set_cf_options_override(std::move(cf_override));
            }
            database->open(db_path, needs_upgrade
                                        ? RocksDatabase::OpenMode::ReadWrite
                                        : open_mode);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex_);
            opening_.erase(db_path);
            cv_.notify_all();
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = databases_.find(db_path);

            if (it == databases_.end()) {
                databases_[db_path] = database;
                retain_locked(db_path, database);
                opening_.erase(db_path);
                cv_.notify_all();
                return database;
            }

            auto current = it->second.lock();
            if (!current) {
                databases_[db_path] = database;
                retain_locked(db_path, database);
                opening_.erase(db_path);
                cv_.notify_all();
                return database;
            }

            if (!(current->is_read_only() &&
                  open_mode == RocksDatabase::OpenMode::ReadWrite)) {
                retain_locked(db_path, current);
                opening_.erase(db_path);
                cv_.notify_all();
                return current;
            }

            drop_retained_locked(db_path);
            if (current.use_count() != 1) {
                opening_.erase(db_path);
                cv_.notify_all();
                throw DFTUtilsException(ErrorCode::INVALID_ARGUMENT,
                                        "Cannot upgrade RocksDB instance at '" +
                                            db_path +
                                            "' from read-only to read-write "
                                            "while it is still in use");
            }

            databases_[db_path] = database;
            retain_locked(db_path, database);
            opening_.erase(db_path);
            cv_.notify_all();
            return database;
        }
    }
}

void register_reset_listener(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(reset_listeners_mutex());
    reset_listeners().push_back(std::move(fn));
}

void RocksDBManager::reset(const std::string& db_path) {
    // Caches drop their handles first, so this reset releases every strong
    // reference and the DB can close.
    notify_reset_listeners(db_path);

    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [&] { return !opening_.contains(db_path); });

    drop_retained_locked(db_path);
    databases_.erase(db_path);
}

void RocksDBManager::shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return opening_.empty(); });
    retained_index_.clear();
    retained_.clear();
    databases_.clear();
}

}  // namespace dftracer::utils::index::store
