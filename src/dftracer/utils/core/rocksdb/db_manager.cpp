#include <dftracer/utils/core/rocksdb/db_manager.h>

#include <stdexcept>

namespace dftracer::utils::rocksdb {

RocksDBManager& RocksDBManager::instance() {
    static RocksDBManager manager;
    return manager;
}

std::shared_ptr<RocksDatabase> RocksDBManager::get_or_open(
    const std::string& db_path, RocksDatabase::OpenMode open_mode,
    RocksDatabase::CfOptionsOverride cf_override) {
    for (;;) {
        bool needs_upgrade = false;
        bool do_open = false;

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
                        return current;
                    }

                    if (opening_.contains(db_path)) {
                        cv_.wait(lock,
                                 [&] { return !opening_.contains(db_path); });
                        continue;
                    }

                    if (current.use_count() != 1) {
                        throw std::runtime_error(
                            "Cannot upgrade RocksDB instance at '" + db_path +
                            "' from read-only to read-write while it is still "
                            "in use");
                    }

                    needs_upgrade = true;
                    opening_.insert(db_path);
                    do_open = true;
                    break;
                }

                if (opening_.contains(db_path)) {
                    cv_.wait(lock, [&] { return !opening_.contains(db_path); });
                    continue;
                }

                opening_.insert(db_path);
                do_open = true;
                break;
            }
        }

        if (!do_open) {
            continue;
        }

        std::shared_ptr<RocksDatabase> database;
        try {
            database = std::make_shared<RocksDatabase>();
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
                opening_.erase(db_path);
                cv_.notify_all();
                return database;
            }

            auto current = it->second.lock();
            if (!current) {
                databases_[db_path] = database;
                opening_.erase(db_path);
                cv_.notify_all();
                return database;
            }

            if (!(current->is_read_only() &&
                  open_mode == RocksDatabase::OpenMode::ReadWrite)) {
                opening_.erase(db_path);
                cv_.notify_all();
                return current;
            }

            if (current.use_count() != 1) {
                opening_.erase(db_path);
                cv_.notify_all();
                throw std::runtime_error(
                    "Cannot upgrade RocksDB instance at '" + db_path +
                    "' from read-only to read-write while it is still in use");
            }

            databases_[db_path] = database;
            opening_.erase(db_path);
            cv_.notify_all();
            return database;
        }
    }
}

void RocksDBManager::reset(const std::string& db_path) {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [&] { return !opening_.contains(db_path); });

    auto it = databases_.find(db_path);
    if (it == databases_.end()) {
        return;
    }

    databases_.erase(it);
}

void RocksDBManager::shutdown() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return opening_.empty(); });
        databases_.clear();
    }
}

}  // namespace dftracer::utils::rocksdb
