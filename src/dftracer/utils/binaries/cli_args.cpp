// SPDX-License-Identifier: MIT
#include <dftracer/utils/binaries/cli_args.h>
#include <dftracer/utils/common/str_format.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace dftracer::utils::binaries {

namespace {

using dstd::Optional;
using dstd::String;
using dstd::StringView;
using dstd::Vector;

String join_names(const Vector<StringView>& names, StringView separator) {
    String out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i != 0) out += separator;
        out += names[i];
    }
    return out;
}

String quoted(StringView text) {
    String out = "\"";
    out += text;
    out += "\"";
    return out;
}

StringView longest_flag(const Vector<StringView>& names) {
    for (const auto& name : names) {
        if (name.size() > 2 && name[0] == '-' && name[1] == '-') return name;
    }
    return names.front();
}

}  // namespace

ArgSpec& ArgSpec::required() {
    is_required_ = true;
    has_default_ = false;
    return *this;
}

ArgSpec& ArgSpec::append() {
    kind_ = Kind::Value;
    is_append_ = true;
    return *this;
}

ArgSpec& ArgSpec::at_least_one() {
    min_one_ = true;
    return *this;
}

ArgParser::ArgParser(String program, String version, String description)
    : program_(std::move(program)),
      version_(std::move(version)),
      description_(std::move(description)) {
    auto make = [this](Names names, StringView text) {
        auto spec = dstd::make_unique<ArgSpec>();
        spec->kind_ = ArgSpec::Kind::Flag;
        for (const auto& name : names) spec->names_.emplace_back(name);
        spec->help_ = text;
        specs_.push_back(std::move(spec));
    };
    make({"-h", "--help"}, "shows help message and exits");
    make({"-v", "--version"}, "prints version information and exits");
}

ArgParser::~ArgParser() = default;

void ArgParser::group(StringView title) { groups_.emplace_back(title); }

ArgSpec& ArgParser::add_value(Names names, StringView help, String default_repr,
                              bool has_default) {
    auto spec = dstd::make_unique<ArgSpec>();
    spec->kind_ = ArgSpec::Kind::Value;
    for (const auto& name : names) spec->names_.emplace_back(name);
    spec->help_ = help;
    spec->default_repr_ = std::move(default_repr);
    spec->has_default_ = has_default;
    spec->group_ = groups_.size();
    specs_.push_back(std::move(spec));
    return *specs_.back();
}

ArgSpec& ArgParser::add_converted(Names names, StringView help,
                                  StringView default_text,
                                  ArgSpec::Setter convert) {
    ArgSpec& spec = add_value(names, help, String(), true);
    spec.default_view_ = default_text;
    spec.quote_default_ = true;
    spec.set_ = std::move(convert);
    if (auto error = spec.set_(default_text)) {
        std::fprintf(stderr, "bad default for %s: %s\n",
                     String(spec.names_.front()).c_str(), error->c_str());
        std::abort();
    }
    return spec;
}

String ArgParser::number_text(double value) {
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%g", value);
    return String(buf, static_cast<std::size_t>(n));
}

String ArgParser::number_error(StringView name, StringView text) {
    return common::str_cat("Invalid number for ", name, ": '", text, "'");
}

StringView ArgParser::long_name(Names names) {
    return longest_flag(Vector<StringView>(names.begin(), names.end()));
}

ArgSpec& ArgParser::flag(bool& target, Names names, StringView help) {
    auto spec = dstd::make_unique<ArgSpec>();
    spec->kind_ = ArgSpec::Kind::Flag;
    for (const auto& name : names) spec->names_.emplace_back(name);
    spec->help_ = help;
    spec->group_ = groups_.size();
    spec->set_ = [&target](StringView) -> Optional<String> {
        target = true;
        return dstd::nullopt;
    };
    specs_.push_back(std::move(spec));
    return *specs_.back();
}

ArgSpec& ArgParser::option(StringView& target, Names names, StringView help) {
    ArgSpec& spec = add_value(names, help, String(), true);
    spec.default_view_ = target;
    spec.quote_default_ = true;
    spec.set_ = [&target](StringView text) -> Optional<String> {
        target = text;
        return dstd::nullopt;
    };
    return spec;
}

ArgSpec& ArgParser::option(Vector<StringView>& target, Names names,
                           StringView help) {
    String repr = "{";
    for (std::size_t i = 0; i < target.size(); ++i) {
        if (i != 0) repr += ", ";
        repr += quoted(target[i]);
    }
    repr += "}";
    ArgSpec& spec = add_value(names, help, std::move(repr), true);
    spec.kind_ = ArgSpec::Kind::Many;
    spec.set_ = [&target](StringView text) -> Optional<String> {
        target.push_back(text);
        return dstd::nullopt;
    };
    return spec;
}

ArgSpec& ArgParser::add_bytes(Names names, StringView help,
                              StringView default_text, bool unit_required,
                              dstd::Function<void(std::uint64_t)> store) {
    const StringView name = long_name(names);
    return add_converted(
        names, help, default_text,
        [store = std::move(store), name,
         unit_required](StringView text) mutable -> Optional<String> {
            auto parsed = common::parse_bytes(text);
            const auto parts = common::detail::split_value_unit(text);
            if (unit_required && (!parsed || !parts || parts->second.empty())) {
                return common::str_cat(
                    "Invalid byte size for ", name, ": '", text,
                    "' (a unit is required, use e.g. 4MB or 512KB)");
            }
            if (!parsed) {
                return common::str_cat("Invalid byte size for ", name, ": '",
                                       text, "' (use e.g. 65536, 64KB, 1.5GB)");
            }
            store(*parsed);
            return dstd::nullopt;
        });
}

ArgSpec& ArgParser::bytes_signed(std::int64_t& target, Names names,
                                 StringView help, StringView default_text) {
    const StringView name = long_name(names);
    return add_converted(names, help, default_text,
                         [&target, name](StringView text) -> Optional<String> {
                             const auto trimmed = common::detail::trim_ws(text);
                             if (!trimmed.empty() && trimmed.front() == '-') {
                                 const String owned(text);
                                 target = static_cast<std::int64_t>(
                                     std::strtoll(owned.c_str(), nullptr, 10));
                                 return dstd::nullopt;
                             }
                             auto parsed = common::parse_bytes(text);
                             if (!parsed) {
                                 return common::str_cat(
                                     "Invalid byte size for ", name, ": '",
                                     text, "' (use e.g. 65536, 64KB, 1.5GB)");
                             }
                             target = static_cast<std::int64_t>(*parsed);
                             return dstd::nullopt;
                         });
}

ArgSpec& ArgParser::seconds(double& target, Names names, StringView help,
                            StringView default_text) {
    const StringView name = long_name(names);
    return add_converted(names, help, default_text,
                         [&target, name](StringView text) -> Optional<String> {
                             auto parsed = common::parse_duration_seconds(text);
                             if (!parsed) {
                                 return common::str_cat(
                                     "Invalid duration for ", name, ": '", text,
                                     "' (use e.g. 30, 30s, 5m, 1.5h)");
                             }
                             target = *parsed;
                             return dstd::nullopt;
                         });
}

ArgSpec& ArgParser::duration(double& target, double native_per_second,
                             Names names, StringView help,
                             StringView default_text) {
    const StringView name = long_name(names);
    return add_converted(
        names, help, default_text,
        [&target, native_per_second,
         name](StringView text) -> Optional<String> {
            auto parsed = common::parse_duration_as(text, native_per_second);
            if (!parsed) {
                return common::str_cat("Invalid duration for ", name, ": '",
                                       text,
                                       "' (use e.g. 5000, 5s, 500ms, 1.5h)");
            }
            target = *parsed;
            return dstd::nullopt;
        });
}

ArgSpec& ArgParser::optional_duration(Optional<double>& target,
                                      double native_per_second, Names names,
                                      StringView help) {
    const StringView name = long_name(names);
    ArgSpec& spec = add_value(names, help, String(), true);
    spec.quote_default_ = true;
    spec.set_ = [&target, native_per_second,
                 name](StringView text) -> Optional<String> {
        if (text.empty()) {
            target = dstd::nullopt;
            return dstd::nullopt;
        }
        auto parsed = common::parse_duration_as(text, native_per_second);
        if (!parsed) {
            return common::str_cat("Invalid duration for ", name, ": '", text,
                                   "' (use e.g. 5000, 5s, 500ms, 1.5h)");
        }
        target = *parsed;
        return dstd::nullopt;
    };
    return spec;
}

ArgSpec& ArgParser::positional(Vector<StringView>& target, StringView name,
                               StringView help) {
    auto spec = dstd::make_unique<ArgSpec>();
    spec->kind_ = ArgSpec::Kind::Positional;
    spec->names_.emplace_back(name);
    spec->help_ = help;
    spec->set_ = [&target](StringView text) -> Optional<String> {
        target.push_back(text);
        return dstd::nullopt;
    };
    specs_.push_back(std::move(spec));
    return *specs_.back();
}

String ArgParser::help() const {
    auto label = [](const ArgSpec& spec) {
        String out = join_names(spec.names_, ", ");
        if (spec.kind_ == ArgSpec::Kind::Value) out += " VALUE";
        if (spec.kind_ == ArgSpec::Kind::Many) out += " VALUE...";
        return out;
    };
    std::size_t width = 0;
    for (const auto& spec : specs_) {
        if (spec->kind_ != ArgSpec::Kind::Positional)
            width = std::max(width, label(*spec).size());
    }

    auto line = [&](const ArgSpec& spec) {
        String out = "  ";
        String text = label(spec);
        text.resize(std::max(width, text.size()), ' ');
        out += text;
        out += "  ";
        out += spec.help_;
        if (spec.kind_ != ArgSpec::Kind::Flag && spec.has_default_) {
            out += " [default: ";
            if (spec.quote_default_)
                out += quoted(spec.default_view_);
            else
                out += spec.default_repr_;
            out += "]";
        } else if (spec.is_required_) {
            out += " [required]";
        }
        if (spec.is_append_) out += " [repeatable]";
        out += '\n';
        return out;
    };

    String out = "Usage: ";
    out += program_;
    out += " [options]";
    for (const auto& spec : specs_) {
        if (spec->kind_ != ArgSpec::Kind::Positional) continue;
        out += spec->min_one_ ? " " : " [";
        out += spec->names_.front();
        out += spec->min_one_ ? "..." : "...]";
    }
    out += "\n\n";
    if (!description_.empty()) {
        out += description_;
        out += "\n\n";
    }
    for (const auto& spec : specs_) {
        if (spec->kind_ != ArgSpec::Kind::Positional) continue;
        out += "Inputs:\n  ";
        out += spec->names_.front();
        out += "  ";
        out += spec->help_;
        out += "\n\n";
    }
    out += "Options:\n";
    for (const auto& spec : specs_) {
        if (spec->kind_ != ArgSpec::Kind::Positional && spec->group_ == 0)
            out += line(*spec);
    }
    for (std::size_t g = 0; g < groups_.size(); ++g) {
        out += '\n';
        out += groups_[g];
        out += ":\n";
        for (const auto& spec : specs_) {
            if (spec->kind_ != ArgSpec::Kind::Positional &&
                spec->group_ == g + 1)
                out += line(*spec);
        }
    }
    return out;
}

ParseOutcome ArgParser::parse(const Vector<StringView>& args) {
    auto fail = [](String message) {
        ParseOutcome outcome;
        outcome.exit_code = 1;
        outcome.err = std::move(message);
        return outcome;
    };
    auto find = [this](StringView name) -> ArgSpec* {
        if (name.size() < 2 || name[0] != '-') return nullptr;
        for (auto& spec : specs_) {
            for (const auto& candidate : spec->names_) {
                if (candidate == name) return spec.get();
            }
        }
        return nullptr;
    };

    for (auto& spec : specs_) spec->used_ = false;
    ArgSpec* positional = nullptr;
    for (auto& spec : specs_) {
        if (spec->kind_ == ArgSpec::Kind::Positional) positional = spec.get();
    }

    bool options_done = false;
    std::size_t i = 0;
    while (i < args.size()) {
        const StringView token = args[i];
        const bool option_like =
            !options_done && token.size() > 1 && token[0] == '-';
        if (!option_like) {
            if (!positional) {
                return fail(common::str_cat("Unexpected argument: ", token));
            }
            positional->set_(token);
            positional->used_ = true;
            ++i;
            continue;
        }
        if (token == "--") {
            options_done = true;
            ++i;
            continue;
        }
        StringView name = token;
        Optional<StringView> inline_value;
        const auto eq = token.find('=');
        if (token[1] == '-' && eq != StringView::npos) {
            name = token.substr(0, eq);
            inline_value = token.substr(eq + 1);
        }
        ArgSpec* spec = find(name);
        if (!spec) return fail(common::str_cat("Unknown option: ", name));
        ++i;
        spec->used_ = true;
        if (spec == specs_[0].get()) {
            ParseOutcome outcome;
            outcome.exit_code = 0;
            outcome.out = help();
            return outcome;
        }
        if (spec == specs_[1].get()) {
            ParseOutcome outcome;
            outcome.exit_code = 0;
            outcome.out = version_ + "\n";
            return outcome;
        }
        if (spec->kind_ == ArgSpec::Kind::Flag) {
            if (inline_value) {
                return fail(
                    common::str_cat("Option ", name, " takes no value"));
            }
            spec->set_(StringView());
            continue;
        }
        Vector<StringView> values;
        if (inline_value) {
            values.push_back(*inline_value);
        } else if (i < args.size()) {
            values.push_back(args[i]);
            ++i;
        }
        if (spec->kind_ == ArgSpec::Kind::Many && !spec->is_append_) {
            while (i < args.size() && !find(args[i]) &&
                   !(args[i].size() > 1 && args[i][0] == '-' &&
                     args[i][1] == '-')) {
                values.push_back(args[i]);
                ++i;
            }
        }
        if (values.empty()) {
            return fail(common::str_cat("Missing value for ", name));
        }
        for (const auto& value : values) {
            if (auto error = spec->set_(value)) return fail(std::move(*error));
        }
    }

    for (const auto& spec : specs_) {
        if (spec->kind_ == ArgSpec::Kind::Positional) {
            if (spec->min_one_ && !spec->used_) {
                return fail(common::str_cat("Missing required input: ",
                                            spec->names_.front()));
            }
        } else if (spec->is_required_ && !spec->used_) {
            return fail(common::str_cat("Missing required option: ",
                                        longest_flag(spec->names_)));
        }
    }
    return ParseOutcome{};
}

ParseOutcome ArgParser::parse(int argc, char** argv) {
    Vector<StringView> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 1; i < argc; ++i) args.push_back(StringView(argv[i]));
    return parse(args);
}

}  // namespace dftracer::utils::binaries
