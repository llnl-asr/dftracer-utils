// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/fileio/parallel/layout.h>
#include <doctest/doctest.h>

using namespace dftracer::utils;

namespace {
constexpr std::size_t MB = 1024 * 1024;
}

TEST_CASE("compute_writer_sizing - local FS uses defaults") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Local, 0, 0};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 8);
    CHECK(s.flush_threshold == 12 * MB);
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - NFS keeps defaults (no stripe info)") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Sharded,
        dftracer::utils::common::FilesystemKind::Nfs, 0, 0};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 8);
    CHECK(s.flush_threshold == 12 * MB);
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - Lustre caps workers at stripe_count") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Lustre, 1 * MB, 4};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 4);
    CHECK(s.flush_threshold == 12 * MB);  // stripe 1MB < default 12MB
    CHECK(s.buffer_capacity == 16 * MB);
}

TEST_CASE("compute_writer_sizing - Lustre grows flush to stripe_size") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Lustre, 32 * MB, 2};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 2);
    CHECK(s.flush_threshold == 32 * MB);
    CHECK(s.buffer_capacity == 36 * MB);
}

TEST_CASE("compute_writer_sizing - baseline smaller than stripe_count wins") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Lustre, 4 * MB, 16};
    auto s = fileio::parallel::compute_writer_sizing(info, 4, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 4);
}

TEST_CASE("compute_writer_sizing - zero baseline coerced to one worker") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Local, 0, 0};
    auto s = fileio::parallel::compute_writer_sizing(info, 0, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 1);
}

TEST_CASE(
    "compute_writer_sizing - GPFS treated like Lustre when stripe given") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Gpfs, 8 * MB, 3};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB);
    CHECK(s.worker_count == 3);
    CHECK(s.flush_threshold == 12 * MB);
}

TEST_CASE("compute_writer_sizing - padded layout clamps flush to stripe") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Lustre, 4 * MB, 4};
    auto s = fileio::parallel::compute_writer_sizing(info, 8, 12 * MB, 4 * MB,
                                                     /*padded_layout=*/true);
    // Padded layout does not cap workers by stripe_count; the packer
    // serializes stripe assembly, so extra compression workers are useful.
    CHECK(s.worker_count == 8);
    CHECK(s.flush_threshold == 4 * MB);  // clamped to stripe, not default
    CHECK(s.buffer_capacity == 8 * MB);  // flush + headroom
}

TEST_CASE(
    "compute_writer_sizing - padded keeps baseline regardless of stripe") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Lustre, 32 * MB, 8};
    auto s = fileio::parallel::compute_writer_sizing(info, 16, 12 * MB, 4 * MB,
                                                     /*padded_layout=*/true);
    CHECK(s.worker_count == 16);  // not capped by stripe_count
    CHECK(s.flush_threshold == 32 * MB);
    CHECK(s.buffer_capacity == 36 * MB);
}

TEST_CASE("compute_writer_sizing - padded_layout without stripe is a no-op") {
    fileio::parallel::LayoutInfo info{
        fileio::parallel::FileLayout::Striped,
        dftracer::utils::common::FilesystemKind::Local, 0, 0};
    auto s = fileio::parallel::compute_writer_sizing(info, 4, 12 * MB, 4 * MB,
                                                     /*padded_layout=*/true);
    // No stripe known, fall back to default flush.
    CHECK(s.flush_threshold == 12 * MB);
}
