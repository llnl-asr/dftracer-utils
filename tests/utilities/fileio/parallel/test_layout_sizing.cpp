#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/fileio/parallel/layout.h>
#include <doctest/doctest.h>

using dftracer::utils::utilities::fileio::parallel::compute_writer_sizing;
using dftracer::utils::utilities::fileio::parallel::FileLayout;
using dftracer::utils::utilities::fileio::parallel::FilesystemKind;
using dftracer::utils::utilities::fileio::parallel::LayoutInfo;

namespace {
constexpr std::size_t MB = 1024 * 1024;
}

TEST_CASE("compute_writer_sizing - local FS uses defaults") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LOCAL, 0, 0};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 8);
    CHECK(s.flush_threshold == 12 * MB);
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - NFS keeps defaults (no stripe info)") {
    LayoutInfo info{FileLayout::SHARDED, FilesystemKind::NFS, 0, 0};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 8);
    CHECK(s.flush_threshold == 12 * MB);
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - Lustre caps workers at stripe_count") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LUSTRE, 1 * MB, 4};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 4);
    CHECK(s.flush_threshold == 12 * MB);  // stripe 1MB < default 12MB
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - Lustre grows flush to stripe_size") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LUSTRE, 32 * MB, 2};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 2);
    CHECK(s.flush_threshold == 32 * MB);
    CHECK(s.buffer_capacity == 36 * MB);
}

TEST_CASE("compute_writer_sizing - baseline smaller than stripe_count wins") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LUSTRE, 4 * MB, 16};
    auto s = compute_writer_sizing(info, 4, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 4);
}

TEST_CASE("compute_writer_sizing - zero baseline coerced to one worker") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LOCAL, 0, 0};
    auto s = compute_writer_sizing(info, 0, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 1);
}

TEST_CASE(
    "compute_writer_sizing - GPFS treated like Lustre when stripe given") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::GPFS, 8 * MB, 3};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.num_workers == 3);
    CHECK(s.flush_threshold == 12 * MB);
}

TEST_CASE("compute_writer_sizing - padded layout clamps flush to stripe") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LUSTRE, 4 * MB, 4};
    auto s = compute_writer_sizing(info, 8, 12 * MB, 4 * MB,
                                   /*padded_layout=*/true);
    // Padded layout does not cap workers by stripe_count; the packer
    // serializes stripe assembly, so extra compression workers are useful.
    CHECK(s.num_workers == 8);
    CHECK(s.flush_threshold == 4 * MB);  // clamped to stripe, not default
    CHECK(s.buffer_capacity == 8 * MB);  // flush + headroom
}

TEST_CASE(
    "compute_writer_sizing - padded keeps baseline regardless of stripe") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LUSTRE, 32 * MB, 8};
    auto s = compute_writer_sizing(info, 16, 12 * MB, 4 * MB,
                                   /*padded_layout=*/true);
    CHECK(s.num_workers == 16);  // not capped by stripe_count
    CHECK(s.flush_threshold == 32 * MB);
    CHECK(s.buffer_capacity == 36 * MB);
}

TEST_CASE("compute_writer_sizing - padded_layout without stripe is a no-op") {
    LayoutInfo info{FileLayout::STRIPED, FilesystemKind::LOCAL, 0, 0};
    auto s = compute_writer_sizing(info, 4, 12 * MB, 4 * MB,
                                   /*padded_layout=*/true);
    // No stripe known, fall back to default flush.
    CHECK(s.flush_threshold == 12 * MB);
}
