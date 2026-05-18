#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/rocksdb/database.h>
#include <dftracer/utils/core/rocksdb/db_manager.h>
#include <dftracer/utils/core/rocksdb/filesystem.h>
#include <dftracer/utils/core/rocksdb/key_codec.h>
#include <doctest/doctest.h>
#include <rocksdb/file_system.h>
#include <testing_utilities.h>

#include <array>
#include <cstring>
#include <memory>

using dftracer::utils::rocksdb::KeyBuilder;
using dftracer::utils::rocksdb::KeyCodec;
using dftracer::utils::rocksdb::RocksDatabase;
using dftracer::utils::rocksdb::RocksDBManager;

TEST_SUITE("RocksDBStorage") {
    TEST_CASE("key codec round-trips big-endian integers") {
        const std::uint32_t v32 = 0x10203040U;
        const std::uint64_t v64 = 0x0102030405060708ULL;

        CHECK(KeyCodec::decode_be32(KeyCodec::encode_be32(v32)) == v32);
        CHECK(KeyCodec::decode_be64(KeyCodec::encode_be64(v64)) == v64);

        KeyBuilder builder;
        builder.append_tag("f|").append_be32(17).append_separator().append_be64(
            9);
        auto built = builder.build();
        CHECK(built.size() == 2 + 4 + 1 + 8);
    }

    TEST_CASE("basic put/get works across column families") {
        auto root = dft_utils_test::make_unique_test_path("rocksdb_put_get");
        fs::create_directories(root);

        RocksDatabase db((root / ".dftindex").string());

        CHECK(db.is_open());
        CHECK(db.put("hello", "world").ok());
        CHECK(db.put("k1", "v1", "provenance").ok());

        std::string value;
        CHECK(db.get("hello", &value).ok());
        CHECK(value == "world");

        CHECK(db.get("k1", &value, "provenance").ok());
        CHECK(value == "v1");
    }

    TEST_CASE("manager reuses one live instance per db path") {
        auto root = dft_utils_test::make_unique_test_path("rocksdb_manager");
        fs::create_directories(root);

        auto path = (root / ".dftindex").string();
        auto& manager = RocksDBManager::instance();

        auto rw = manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(rw != nullptr);
        REQUIRE(rw->is_open());
        CHECK_FALSE(rw->is_read_only());

        auto rw_again =
            manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
        CHECK(rw_again == rw);

        auto ro = manager.get_or_open(path, RocksDatabase::OpenMode::ReadOnly);
        CHECK(ro == rw);
        CHECK_FALSE(ro->is_read_only());
    }

    TEST_CASE("manager reset drops the cached instance for one path") {
        auto root =
            dft_utils_test::make_unique_test_path("rocksdb_manager_reset");
        fs::create_directories(root);

        auto path = (root / ".dftindex").string();
        auto& manager = RocksDBManager::instance();

        std::weak_ptr<RocksDatabase> first_weak;
        {
            auto first =
                manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
            REQUIRE(first != nullptr);
            first_weak = first;
            manager.reset(path);
        }
        // After reset() + the only strong owner going out of scope, the old
        // instance must have been destroyed (RocksDB holds a per-process file
        // lock, so a stale cached instance would prevent reopening below).
        CHECK(first_weak.expired());

        auto second =
            manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(second != nullptr);
        CHECK(second->is_open());
    }

    TEST_CASE("manager shutdown clears cached instances") {
        auto root =
            dft_utils_test::make_unique_test_path("rocksdb_manager_shutdown");
        fs::create_directories(root);

        auto path = (root / ".dftindex").string();
        auto& manager = RocksDBManager::instance();

        std::weak_ptr<RocksDatabase> first_weak;
        {
            auto first =
                manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
            REQUIRE(first != nullptr);
            first_weak = first;
            manager.shutdown();
        }
        CHECK(first_weak.expired());

        auto second =
            manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(second != nullptr);
        CHECK(second->is_open());
    }

    TEST_CASE("manager rejects read-only upgrade while handle is alive") {
        auto root =
            dft_utils_test::make_unique_test_path("rocksdb_manager_upgrade");
        fs::create_directories(root);

        auto path = (root / ".dftindex").string();
        RocksDatabase seed(path, RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(seed.is_open());

        auto& manager = RocksDBManager::instance();
        auto ro = manager.get_or_open(path, RocksDatabase::OpenMode::ReadOnly);
        REQUIRE(ro != nullptr);
        CHECK(ro->is_read_only());
        CHECK_THROWS_WITH_AS(
            manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite),
            doctest::Contains("still in use"), std::runtime_error);
    }

    TEST_CASE("manager rejects read-only upgrade while handle is shared") {
        auto root = dft_utils_test::make_unique_test_path(
            "rocksdb_manager_upgrade_shared");
        fs::create_directories(root);

        auto path = (root / ".dftindex").string();
        RocksDatabase seed(path, RocksDatabase::OpenMode::ReadWrite);
        REQUIRE(seed.is_open());

        auto& manager = RocksDBManager::instance();
        auto ro = manager.get_or_open(path, RocksDatabase::OpenMode::ReadOnly);
        REQUIRE(ro != nullptr);
        CHECK(ro->is_read_only());

        auto ro_shared = ro;
        CHECK_THROWS_WITH_AS(
            manager.get_or_open(path, RocksDatabase::OpenMode::ReadWrite),
            doctest::Contains("still in use"), std::runtime_error);
    }

    TEST_CASE("custom filesystem supports async read polling") {
        auto root = dft_utils_test::make_unique_test_path("rocksdb_async_read");
        fs::create_directories(root);

        auto file_system =
            dftracer::utils::rocksdb::make_dftracer_file_system();
        auto test_file = (root / "async-read.bin").string();

        {
            std::unique_ptr<::rocksdb::FSWritableFile> writable;
            REQUIRE(file_system
                        ->NewWritableFile(test_file, ::rocksdb::FileOptions(),
                                          &writable, nullptr)
                        .ok());
            const std::string payload = "abcdefghijklmnop";
            REQUIRE(writable
                        ->Append(::rocksdb::Slice(payload),
                                 ::rocksdb::IOOptions(), nullptr)
                        .ok());
            REQUIRE(writable->Close(::rocksdb::IOOptions(), nullptr).ok());
        }

        std::unique_ptr<::rocksdb::FSRandomAccessFile> random;
        REQUIRE(file_system
                    ->NewRandomAccessFile(test_file, ::rocksdb::FileOptions(),
                                          &random, nullptr)
                    .ok());

        int64_t supported_ops = 0;
        file_system->SupportedOps(supported_ops);
        CHECK((supported_ops & (1LL << ::rocksdb::FSSupportedOps::kAsyncIO)) !=
              0);

        std::array<char, 5> scratch{};
        ::rocksdb::FSReadRequest request;
        request.offset = 2;
        request.len = 4;
        request.scratch = scratch.data();

        bool callback_called = false;
        bool callback_status_ok = false;
        std::string callback_result;
        void* io_handle = nullptr;
        ::rocksdb::IOHandleDeleter deleter;

        REQUIRE(
            random
                ->ReadAsync(
                    request, ::rocksdb::IOOptions(),
                    [&callback_called, &callback_status_ok, &callback_result](
                        ::rocksdb::FSReadRequest& completed, void*) {
                        callback_called = true;
                        callback_status_ok = completed.status.ok();
                        callback_result = completed.result.ToString();
                    },
                    nullptr, &io_handle, &deleter, nullptr)
                .ok());
        REQUIRE(io_handle != nullptr);

        std::vector<void*> io_handles{io_handle};
        REQUIRE(file_system->Poll(io_handles, 1).ok());
        CHECK(callback_called);
        CHECK(callback_status_ok);
        CHECK(callback_result == "cdef");

        deleter(io_handle);
    }
}
