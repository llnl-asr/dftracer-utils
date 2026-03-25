#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/internal/utils.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft::internal;

TEST_SUITE("IsDataTransferOp") {
    TEST_CASE("POSIX read/write are data transfer") {
        CHECK(is_data_transfer_op("POSIX", "read"));
        CHECK(is_data_transfer_op("POSIX", "write"));
        CHECK(is_data_transfer_op("POSIX", "pread"));
        CHECK(is_data_transfer_op("POSIX", "pwrite"));
        CHECK(is_data_transfer_op("POSIX", "pread64"));
        CHECK(is_data_transfer_op("POSIX", "pwrite64"));
    }

    TEST_CASE("STDIO fread/fwrite are data transfer") {
        CHECK(is_data_transfer_op("STDIO", "fread"));
        CHECK(is_data_transfer_op("STDIO", "fwrite"));
    }

    TEST_CASE("POSIX non-transfer ops") {
        CHECK_FALSE(is_data_transfer_op("POSIX", "lseek64"));
        CHECK_FALSE(is_data_transfer_op("POSIX", "open"));
        CHECK_FALSE(is_data_transfer_op("POSIX", "close"));
        CHECK_FALSE(is_data_transfer_op("POSIX", "fork"));
        CHECK_FALSE(is_data_transfer_op("POSIX", "mkdir"));
        CHECK_FALSE(is_data_transfer_op("POSIX", "__fxstat64"));
    }

    TEST_CASE("non-POSIX/STDIO category") {
        CHECK_FALSE(is_data_transfer_op("USER", "read"));
        CHECK_FALSE(is_data_transfer_op("MPI", "write"));
        CHECK_FALSE(is_data_transfer_op("HDF5", "fread"));
    }

    TEST_CASE("empty category") {
        CHECK_FALSE(is_data_transfer_op("", "read"));
        CHECK_FALSE(is_data_transfer_op("", "write"));
    }

    TEST_CASE("network ops") {
        CHECK(is_data_transfer_op("POSIX", "recv"));
        CHECK(is_data_transfer_op("POSIX", "send"));
        CHECK(is_data_transfer_op("POSIX", "recvfrom"));
        CHECK(is_data_transfer_op("POSIX", "sendto"));
    }
}
