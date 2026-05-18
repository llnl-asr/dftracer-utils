#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H

#include <dftracer/utils/utilities/common/json/json_value.h>
#include <dftracer/utils/utilities/common/json/parser.h>
#include <dftracer/utils/utilities/composites/dft/args_map.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dftracer::utils::utilities::composites::dft {

using common::json::JsonValue;

struct DFTracerEvent {
    std::uint64_t id = 0;
    std::string_view name;
    std::string_view cat;
    std::string_view ph;
    std::uint64_t pid = 0;
    std::uint64_t tid = 0;
    std::uint64_t ts = 0;
    std::uint64_t dur = 0;

    ArgsMap args;

    bool is_metadata() const { return ph == "M"; }
    bool is_counter() const { return ph == "C"; }
    bool is_profile() const { return ph == "C" && cat != "sys"; }
    bool is_system() const { return ph == "C" && cat == "sys"; }
    bool is_event() const { return !is_metadata() && !is_counter(); }
    bool is_complete() const { return ph == "X"; }
    bool has_id() const { return id != 0; }

    static bool parse(const JsonValue& json, DFTracerEvent& out) {
        auto ph_val = json["ph"];
        if (!ph_val.exists()) return false;

        out.ph = ph_val.get<std::string_view>();

        auto id_val = json["id"];
        if (id_val.exists()) out.id = id_val.get<std::uint64_t>();

        auto name_val = json["name"];
        if (name_val.exists()) out.name = name_val.get<std::string_view>();

        auto cat_val = json["cat"];
        if (cat_val.exists()) out.cat = cat_val.get<std::string_view>();

        auto pid_val = json["pid"];
        if (pid_val.exists()) out.pid = pid_val.get<std::uint64_t>();

        auto tid_val = json["tid"];
        if (tid_val.exists()) out.tid = tid_val.get<std::uint64_t>();

        auto ts_val = json["ts"];
        if (ts_val.exists()) out.ts = ts_val.get<std::uint64_t>();

        auto dur_val = json["dur"];
        if (dur_val.exists()) out.dur = dur_val.get<std::uint64_t>();

        auto args_val = json["args"];
        if (args_val.exists() && args_val.is_object()) {
            out.args.set_valid(true);
            args_val.for_each_member([&](std::string_view k, JsonValue v) {
                if (v.is_string()) {
                    out.args.insert(k, std::string(v.get<std::string_view>()));
                } else if (v.is_uint()) {
                    out.args.insert(k, v.get<std::uint64_t>());
                } else if (v.is_int()) {
                    out.args.insert(k, v.get<std::int64_t>());
                } else if (v.is_number()) {
                    out.args.insert(k, v.get<double>());
                } else if (v.is_bool()) {
                    out.args.insert(k, v.get<bool>());
                }
            });
        }

        return true;
    }

    static bool parse_scalars(simdjson::dom::element root, DFTracerEvent& out,
                              simdjson::dom::element& out_args,
                              bool& out_has_args) {
        out_has_args = false;
        if (!root.is_object()) return false;

        bool has_ph = false;
        for (auto field : root.get_object()) {
            std::string_view key = field.key;
            simdjson::dom::element val = field.value;
            switch (key.size()) {
                case 2:
                    if (key == "ph") {
                        if (val.is_string()) {
                            out.ph = val.get_string().value_unsafe();
                            has_ph = true;
                        }
                    } else if (key == "id") {
                        if (val.is_uint64())
                            out.id = val.get_uint64().value_unsafe();
                    } else if (key == "ts") {
                        if (val.is_uint64())
                            out.ts = val.get_uint64().value_unsafe();
                    }
                    break;
                case 3:
                    if (key == "pid") {
                        if (val.is_uint64())
                            out.pid = val.get_uint64().value_unsafe();
                    } else if (key == "tid") {
                        if (val.is_uint64())
                            out.tid = val.get_uint64().value_unsafe();
                    } else if (key == "cat") {
                        if (val.is_string())
                            out.cat = val.get_string().value_unsafe();
                    } else if (key == "dur") {
                        if (val.is_uint64())
                            out.dur = val.get_uint64().value_unsafe();
                    }
                    break;
                case 4:
                    if (key == "name") {
                        if (val.is_string())
                            out.name = val.get_string().value_unsafe();
                    } else if (key == "args") {
                        if (val.is_object()) {
                            out_args = val;
                            out_has_args = true;
                        }
                    }
                    break;
                default:
                    break;
            }
        }
        return has_ph;
    }

    static bool parse_ondemand(common::json::JsonParser& parser,
                               DFTracerEvent& out) {
        bool has_ph = false;
        parser.for_each_field([&](std::string_view key,
                                  simdjson::ondemand::value val) {
            if (key == "ph") {
                auto r = val.get_string();
                if (!r.error()) {
                    out.ph = r.value_unsafe();
                    has_ph = true;
                }
            } else if (key == "id") {
                auto r = val.get_uint64();
                if (!r.error()) out.id = r.value_unsafe();
            } else if (key == "name") {
                auto r = val.get_string();
                if (!r.error()) out.name = r.value_unsafe();
            } else if (key == "cat") {
                auto r = val.get_string();
                if (!r.error()) out.cat = r.value_unsafe();
            } else if (key == "pid") {
                auto r = val.get_uint64();
                if (!r.error()) out.pid = r.value_unsafe();
            } else if (key == "tid") {
                auto r = val.get_uint64();
                if (!r.error()) out.tid = r.value_unsafe();
            } else if (key == "ts") {
                auto r = val.get_uint64();
                if (!r.error()) out.ts = r.value_unsafe();
            } else if (key == "dur") {
                auto r = val.get_uint64();
                if (!r.error()) out.dur = r.value_unsafe();
            } else if (key == "args") {
                auto obj = val.get_object();
                if (!obj.error()) {
                    out.args.set_valid(true);
                    for (auto field : obj.value_unsafe()) {
                        if (field.error()) continue;
                        auto fkey = field.unescaped_key();
                        if (fkey.error()) continue;
                        auto fval = field.value();
                        if (fval.error()) continue;

                        auto type = fval.type();
                        if (type.error()) continue;

                        switch (type.value_unsafe()) {
                            case simdjson::ondemand::json_type::string: {
                                auto r = fval.get_string();
                                if (!r.error())
                                    out.args.insert(
                                        fkey.value_unsafe(),
                                        std::string(r.value_unsafe()));
                                break;
                            }
                            case simdjson::ondemand::json_type::number: {
                                auto ri = fval.get_int64();
                                if (!ri.error()) {
                                    auto v = ri.value_unsafe();
                                    if (v >= 0)
                                        out.args.insert(
                                            fkey.value_unsafe(),
                                            static_cast<std::uint64_t>(v));
                                    else
                                        out.args.insert(fkey.value_unsafe(), v);
                                } else {
                                    auto rd = fval.get_double();
                                    if (!rd.error())
                                        out.args.insert(fkey.value_unsafe(),
                                                        rd.value_unsafe());
                                }
                                break;
                            }
                            case simdjson::ondemand::json_type::boolean: {
                                auto r = fval.get_bool();
                                if (!r.error())
                                    out.args.insert(fkey.value_unsafe(),
                                                    r.value_unsafe());
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
            }
        });
        return has_ph;
    }
};

}  // namespace dftracer::utils::utilities::composites::dft

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_EVENT_H
