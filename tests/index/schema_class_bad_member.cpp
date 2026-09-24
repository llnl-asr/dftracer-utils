// Must not compile: a schema class member that is not an index::Field.
#include <dftracer/utils/index/schema_class.h>

#include <vector>

struct BadMember {
    static constexpr std::string_view id = "bad_member";
    std::vector<int> values;
};

int main() {
    return static_cast<int>(
        dftracer::utils::index::schema_spec<BadMember>().fields.size());
}
