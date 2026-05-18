#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_merge_operator.h>
#include <dftracer/utils/utilities/composites/dft/aggregators/system_metrics_serialization.h>
#include <doctest/doctest.h>
#include <rocksdb/slice.h>

#include <memory>
#include <string>
#include <vector>

using namespace dftracer::utils::utilities::composites::dft::aggregators;

TEST_SUITE("SystemMetricsMergeOperator") {
    TEST_CASE("Name returns correct identifier") {
        SystemMetricsMergeOperator op;
        CHECK(std::string(op.Name()) == "SystemMetricsMergeOperator");
    }

    TEST_CASE("PartialMerge combines two operands") {
        SystemMetricsMergeOperator op;

        // Create two system metrics
        SystemAggregationMetrics left;
        left.count = 2;
        left.ts = 100;
        left.te = 200;
        left.update_metric("cpu_user", 40.0);
        left.update_metric("cpu_user", 50.0);

        SystemAggregationMetrics right;
        right.count = 1;
        right.ts = 150;
        right.te = 250;
        right.update_metric("cpu_user", 60.0);
        right.update_metric("memory", 1000.0);

        std::string left_serialized = serialize_system_value(left);
        std::string right_serialized = serialize_system_value(right);

        std::string new_value;
        ::rocksdb::Slice key("test_key");
        ::rocksdb::Slice left_slice(left_serialized);
        ::rocksdb::Slice right_slice(right_serialized);

        bool result =
            op.PartialMerge(key, left_slice, right_slice, &new_value, nullptr);
        CHECK(result);

        auto merged = deserialize_system_value(new_value);
        CHECK(merged.count == 3);
        CHECK(merged.ts == 100);
        CHECK(merged.te == 250);
        REQUIRE(merged.metrics != nullptr);
        CHECK(merged.metrics->size() == 2);
        CHECK(merged.metrics->at("cpu_user").count == 3);
        CHECK(merged.metrics->at("memory").count == 1);
    }

    TEST_CASE("FullMergeV2 merges existing value with operands") {
        SystemMetricsMergeOperator op;

        // Existing value
        SystemAggregationMetrics existing;
        existing.count = 1;
        existing.ts = 50;
        existing.te = 100;
        existing.update_metric("cpu", 30.0);
        std::string existing_serialized = serialize_system_value(existing);

        // First operand
        SystemAggregationMetrics op1;
        op1.count = 1;
        op1.ts = 100;
        op1.te = 150;
        op1.update_metric("cpu", 40.0);
        std::string op1_serialized = serialize_system_value(op1);

        // Second operand
        SystemAggregationMetrics op2;
        op2.count = 1;
        op2.ts = 150;
        op2.te = 200;
        op2.update_metric("cpu", 50.0);
        std::string op2_serialized = serialize_system_value(op2);

        ::rocksdb::Slice key("test_key");
        ::rocksdb::Slice existing_slice(existing_serialized);
        std::vector<::rocksdb::Slice> operands = {
            ::rocksdb::Slice(op1_serialized), ::rocksdb::Slice(op2_serialized)};

        ::rocksdb::MergeOperator::MergeOperationInput merge_in(
            key, &existing_slice, operands, nullptr);
        std::string new_value;
        ::rocksdb::Slice existing_operand;
        ::rocksdb::MergeOperator::MergeOperationOutput merge_out(
            new_value, existing_operand);

        bool result = op.FullMergeV2(merge_in, &merge_out);
        CHECK(result);

        auto merged = deserialize_system_value(new_value);
        CHECK(merged.count == 3);
        CHECK(merged.ts == 50);
        CHECK(merged.te == 200);
        REQUIRE(merged.metrics != nullptr);
        CHECK(merged.metrics->at("cpu").count == 3);
        CHECK(merged.metrics->at("cpu").mean == doctest::Approx(40.0));
    }

    TEST_CASE("FullMergeV2 handles null existing value") {
        SystemMetricsMergeOperator op;

        // First operand
        SystemAggregationMetrics op1;
        op1.count = 2;
        op1.ts = 100;
        op1.te = 200;
        op1.update_metric("memory", 1000.0);
        std::string op1_serialized = serialize_system_value(op1);

        // Second operand
        SystemAggregationMetrics op2;
        op2.count = 3;
        op2.ts = 200;
        op2.te = 300;
        op2.update_metric("memory", 2000.0);
        std::string op2_serialized = serialize_system_value(op2);

        ::rocksdb::Slice key("test_key");
        std::vector<::rocksdb::Slice> operands = {
            ::rocksdb::Slice(op1_serialized), ::rocksdb::Slice(op2_serialized)};

        ::rocksdb::MergeOperator::MergeOperationInput merge_in(
            key, nullptr, operands, nullptr);
        std::string new_value;
        ::rocksdb::Slice existing_operand;
        ::rocksdb::MergeOperator::MergeOperationOutput merge_out(
            new_value, existing_operand);

        bool result = op.FullMergeV2(merge_in, &merge_out);
        CHECK(result);

        auto merged = deserialize_system_value(new_value);
        CHECK(merged.count == 5);
        CHECK(merged.ts == 100);
        CHECK(merged.te == 300);
        REQUIRE(merged.metrics != nullptr);
        CHECK(merged.metrics->at("memory").count == 2);
    }

    TEST_CASE("FullMergeV2 handles single operand") {
        SystemMetricsMergeOperator op;

        SystemAggregationMetrics op1;
        op1.count = 5;
        op1.ts = 100;
        op1.te = 500;
        op1.update_metric("disk_io", 100.0);
        std::string op1_serialized = serialize_system_value(op1);

        ::rocksdb::Slice key("test_key");
        std::vector<::rocksdb::Slice> operands = {
            ::rocksdb::Slice(op1_serialized)};

        ::rocksdb::MergeOperator::MergeOperationInput merge_in(
            key, nullptr, operands, nullptr);
        std::string new_value;
        ::rocksdb::Slice existing_operand;
        ::rocksdb::MergeOperator::MergeOperationOutput merge_out(
            new_value, existing_operand);

        bool result = op.FullMergeV2(merge_in, &merge_out);
        CHECK(result);

        auto merged = deserialize_system_value(new_value);
        CHECK(merged.count == 5);
        CHECK(merged.ts == 100);
        CHECK(merged.te == 500);
        REQUIRE(merged.metrics != nullptr);
        CHECK(merged.metrics->at("disk_io").count == 1);
    }
}
