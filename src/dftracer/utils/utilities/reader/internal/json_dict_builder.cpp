#include <dftracer/utils/utilities/reader/internal/json_dict_builder.h>

namespace dftracer::utils::utilities::reader::internal {

void insert_simdjson_value(ArgsMap &map, std::string_view key,
                           simdjson::ondemand::value val) {
    auto type = val.type();
    if (type.error()) return;
    switch (type.value_unsafe()) {
        case simdjson::ondemand::json_type::string: {
            auto r = val.get_string();
            if (!r.error()) map.insert(key, std::string(r.value_unsafe()));
            break;
        }
        case simdjson::ondemand::json_type::number: {
            auto ri = val.get_int64();
            if (!ri.error()) {
                auto v = ri.value_unsafe();
                if (v >= 0)
                    map.insert(key, static_cast<std::uint64_t>(v));
                else
                    map.insert(key, v);
            } else {
                auto rd = val.get_double();
                if (!rd.error()) map.insert(key, rd.value_unsafe());
            }
            break;
        }
        case simdjson::ondemand::json_type::boolean: {
            auto r = val.get_bool();
            if (!r.error()) map.insert(key, r.value_unsafe());
            break;
        }
        default:
            break;
    }
}

void parse_json_to_event(common::json::JsonParser &parser, JsonDictEvent &ev) {
    ev.top.set_valid(true);
    parser.for_each_field(
        [&](std::string_view key, simdjson::ondemand::value val) {
            if (key == "args") {
                auto obj = val.get_object();
                if (!obj.error()) {
                    ev.args.set_valid(true);
                    for (auto field : obj.value_unsafe()) {
                        if (field.error()) continue;
                        auto fkey = field.unescaped_key();
                        if (fkey.error()) continue;
                        auto fval = field.value();
                        if (fval.error()) continue;
                        insert_simdjson_value(ev.args, fkey.value_unsafe(),
                                              fval.value_unsafe());
                    }
                }
            } else {
                insert_simdjson_value(ev.top, key, val);
            }
        });
}

}  // namespace dftracer::utils::utilities::reader::internal
