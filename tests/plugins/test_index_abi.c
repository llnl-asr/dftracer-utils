// SPDX-License-Identifier: MIT
/* The plugin index ABI (dftracer/utils/plugins/abi/index.h) as plain C: the
 * header compiles, and every field this slice added sits after what a plugin
 * built before it knows, so that plugin still reads the old offsets. */

#include <dftracer/utils/plugins/abi.h>
#include <stddef.h>
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_extension_fields_are_added_at_the_end(void) {
    TEST_ASSERT_EQUAL_size_t(0, offsetof(dftu_index_extension, struct_size));
    TEST_ASSERT_EQUAL_size_t(4, offsetof(dftu_index_extension, version));
    TEST_ASSERT_EQUAL_size_t(32, offsetof(dftu_index_extension, step));
    TEST_ASSERT_EQUAL_size_t(88, offsetof(dftu_index_extension, release));
    TEST_ASSERT_EQUAL_size_t(96, offsetof(dftu_index_extension, columns));
    TEST_ASSERT_EQUAL_size_t(104, offsetof(dftu_index_extension, step_column));
    TEST_ASSERT_EQUAL_size_t(112, sizeof(dftu_index_extension));
}

static void test_leaf_type_is_added_at_the_end(void) {
    TEST_ASSERT_EQUAL_size_t(24, offsetof(dftu_index_leaf, value));
    TEST_ASSERT_EQUAL_size_t(32, offsetof(dftu_index_leaf, type));
}

static void test_service_hash_fields_are_added_at_the_end(void) {
    TEST_ASSERT_EQUAL_size_t(8, offsetof(dftu_svc_index, register_extension));
    TEST_ASSERT_TRUE(offsetof(dftu_svc_index, hash_version) >=
                     offsetof(dftu_svc_index, register_extension) +
                         sizeof(void*));
    TEST_ASSERT_TRUE(offsetof(dftu_svc_index, hash_value) >
                     offsetof(dftu_svc_index, hash_version));
    TEST_ASSERT_TRUE(offsetof(dftu_svc_index, hash_series) >
                     offsetof(dftu_svc_index, hash_value));
    TEST_ASSERT_EQUAL_size_t(
        sizeof(dftu_svc_index),
        offsetof(dftu_svc_index, hash_series) + sizeof(void*));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_extension_fields_are_added_at_the_end);
    RUN_TEST(test_leaf_type_is_added_at_the_end);
    RUN_TEST(test_service_hash_fields_are_added_at_the_end);
    return UNITY_END();
}
