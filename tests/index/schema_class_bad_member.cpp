// SPDX-License-Identifier: MIT
// Must not compile: a schema class member that is not an trace::index::Field.
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/schema_class.h>

using namespace dftracer::utils;

struct BadMember {
    static constexpr dstd::StringView id = "bad_member";
    dstd::Vector<int> values;
};

int main() {
    return static_cast<int>(
        dftracer::utils::trace::index::schema_spec<BadMember>().fields.size());
}
