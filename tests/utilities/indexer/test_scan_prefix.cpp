#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/indexer/error.h>
#include <dftracer/utils/utilities/indexer/internal/scan_prefix.h>
#include <doctest/doctest.h>
#include <rocksdb/iterator.h>
#include <rocksdb/slice.h>
#include <rocksdb/status.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using dftracer::utils::utilities::indexer::IndexerError;
using dftracer::utils::utilities::indexer::internal::scan_prefix_iterator;

namespace {

class FakeIterator final : public ::rocksdb::Iterator {
   public:
    FakeIterator(std::vector<std::pair<std::string, std::string>> entries,
                 ::rocksdb::Status status = ::rocksdb::Status::OK())
        : entries_(std::move(entries)), status_(std::move(status)) {}

    bool Valid() const override {
        return index_ < entries_.size() && status_.ok();
    }

    void SeekToFirst() override {
        index_ = entries_.empty() ? entries_.size() : 0;
    }

    void SeekToLast() override {
        index_ = entries_.empty() ? entries_.size() : entries_.size() - 1;
    }

    void Seek(const ::rocksdb::Slice& target) override {
        const auto key = target.ToString();
        index_ = 0;
        while (index_ < entries_.size() && entries_[index_].first < key) {
            ++index_;
        }
    }

    void SeekForPrev(const ::rocksdb::Slice& target) override {
        const auto key = target.ToString();
        index_ = entries_.size();
        while (index_ > 0 && entries_[index_ - 1].first > key) {
            --index_;
        }
        if (index_ > 0) {
            --index_;
        }
    }

    void Next() override {
        if (index_ < entries_.size()) {
            ++index_;
        }
    }

    void Prev() override {
        if (index_ == 0 || entries_.empty()) {
            index_ = entries_.size();
            return;
        }
        --index_;
    }

    ::rocksdb::Slice key() const override { return entries_[index_].first; }

    ::rocksdb::Slice value() const override { return entries_[index_].second; }

    ::rocksdb::Status status() const override { return status_; }

   private:
    std::vector<std::pair<std::string, std::string>> entries_;
    std::size_t index_ = 0;
    ::rocksdb::Status status_;
};

}  // namespace

TEST_SUITE("ScanPrefix") {
    TEST_CASE("iterates matching prefix entries and stops at the first miss") {
        std::vector<std::string> seen;
        scan_prefix_iterator(
            "scan failed", "ab|",
            [] {
                return std::make_unique<FakeIterator>(
                    std::vector<std::pair<std::string, std::string>>{
                        {"aa|0", "skip"},
                        {"ab|0", "v0"},
                        {"ab|1", "v1"},
                        {"ac|0", "stop"},
                    });
            },
            [&](::rocksdb::Iterator& it) {
                seen.push_back(it.key().ToString());
            });

        REQUIRE(seen.size() == 2);
        CHECK(seen[0] == "ab|0");
        CHECK(seen[1] == "ab|1");
    }

    TEST_CASE("throws IndexerError when iterator status is non-ok") {
        CHECK_THROWS_AS(
            scan_prefix_iterator(
                "scan failed", "ab|",
                [] {
                    return std::make_unique<FakeIterator>(
                        std::vector<std::pair<std::string, std::string>>{
                            {"ab|0", "v0"},
                        },
                        ::rocksdb::Status::IOError(
                            "synthetic iterator failure"));
                },
                [](::rocksdb::Iterator&) {}),
            IndexerError);
    }
}
