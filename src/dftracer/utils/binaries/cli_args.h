// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_BINARIES_CLI_ARGS_H
#define DFTRACER_UTILS_BINARIES_CLI_ARGS_H

#include <dftracer/utils/dstd/function.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace dftracer::utils::binaries {

/// One registered option or positional. Created by @ref ArgParser, which owns
/// it. The chained calls refine the help text and the parse rules.
class ArgSpec {
   public:
    enum class Kind { Flag, Value, Many, Positional };

    /// The option must appear and has no default.
    ArgSpec& required();

    /// A Vector option takes one value per use and collects them in order.
    ArgSpec& append();

    /// A Many option or a positional needs at least one value.
    ArgSpec& at_least_one();

   private:
    friend class ArgParser;
    using Setter =
        dstd::Function<dstd::Optional<dstd::String>(dstd::StringView)>;

    Kind kind_ = Kind::Value;
    dstd::Vector<dstd::StringView> names_;
    dstd::StringView help_;
    dstd::String default_repr_;
    dstd::StringView default_view_;
    bool quote_default_ = false;
    Setter set_;
    std::size_t group_ = 0;
    bool has_default_ = false;
    bool is_required_ = false;
    bool is_append_ = false;
    bool min_one_ = false;
    bool used_ = false;
};

/// What a parse ended in. When `exit_code` holds a value the caller prints
/// `out` to stdout and `err` to stderr, then exits with that code.
struct ParseOutcome {
    dstd::Optional<int> exit_code;
    dstd::String out;
    dstd::String err;
};

/// Command line parser for the CLI tools. Every option binds to a variable of
/// the caller, which keeps its own value as the default. A parse reports a
/// failure as a @ref ParseOutcome and never throws. Registration order sets
/// the usage line and the help text. A list option takes `--x a --x b` and
/// `--x a b`, the second form up to the next known option.
class ArgParser {
   public:
    using Names = std::initializer_list<dstd::StringView>;

    ArgParser(dstd::String program, dstd::String version,
              dstd::String description);
    ~ArgParser();

    ArgParser(const ArgParser&) = delete;
    ArgParser& operator=(const ArgParser&) = delete;

    /// Start a help section. Options registered after it list under it.
    void group(dstd::StringView title);

    /// A switch. @p target becomes true when the flag appears.
    ArgSpec& flag(bool& target, Names names, dstd::StringView help);

    /// A text option. @p target views the argv text, which the parser never
    /// copies and which must outlive it. Its first value is the default and
    /// should be a string literal.
    ArgSpec& option(dstd::StringView& target, Names names,
                    dstd::StringView help);

    /// A list option. Without @ref ArgSpec::append it takes every following
    /// value.
    ArgSpec& option(dstd::Vector<dstd::StringView>& target, Names names,
                    dstd::StringView help);

    /// An integer or floating point option.
    template <class T>
        requires dstd::IS_CHARS_NUMBER<T>
    ArgSpec& option(T& target, Names names, dstd::StringView help) {
        ArgSpec& spec =
            add_value(names, help, number_text(target), /*has_default=*/true);
        spec.set_ = [&target, name = long_name(names)](
                        dstd::StringView text) -> dstd::Optional<dstd::String> {
            const auto parsed = dstd::from_chars<T>(text);
            if (!parsed) return number_error(name, text);
            target = *parsed;
            return dstd::nullopt;
        };
        return spec;
    }

    /// A byte size such as 64KB. @p default_text is parsed into @p target and
    /// shown as the default.
    template <class T>
        requires std::is_unsigned_v<T>
    ArgSpec& bytes(T& target, Names names, dstd::StringView help,
                   dstd::StringView default_text) {
        return add_bytes(
            names, help, default_text, /*unit_required=*/false,
            [&target](std::uint64_t value) { target = static_cast<T>(value); });
    }

    /// As @ref bytes, but the value must carry a unit.
    template <class T>
        requires std::is_unsigned_v<T>
    ArgSpec& bytes_with_unit(T& target, Names names, dstd::StringView help,
                             dstd::StringView default_text) {
        return add_bytes(
            names, help, default_text, /*unit_required=*/true,
            [&target](std::uint64_t value) { target = static_cast<T>(value); });
    }

    /// As @ref bytes, but a negative number passes through as a sentinel.
    ArgSpec& bytes_signed(std::int64_t& target, Names names,
                          dstd::StringView help, dstd::StringView default_text);

    /// A duration in seconds such as 30s or 1.5h. A bare number is seconds.
    ArgSpec& seconds(double& target, Names names, dstd::StringView help,
                     dstd::StringView default_text);

    /// A duration in the option's own unit, @p native_per_second of them in
    /// a second. A bare number keeps that unit.
    ArgSpec& duration(double& target, double native_per_second, Names names,
                      dstd::StringView help, dstd::StringView default_text);

    /// As @ref duration, but an empty value leaves @p target empty.
    ArgSpec& optional_duration(dstd::Optional<double>& target,
                               double native_per_second, Names names,
                               dstd::StringView help);

    /// Trailing values. The usage line shows the name, the help lists it.
    ArgSpec& positional(dstd::Vector<dstd::StringView>& target,
                        dstd::StringView name, dstd::StringView help);

    /// The usage line, description and option list.
    dstd::String help() const;

    /// Parse the arguments after the program name in @p argv.
    ParseOutcome parse(int argc, char** argv);

    /// Parse @p args, which hold no program name. Text bound by the parse
    /// views into the strings @p args views.
    ParseOutcome parse(const dstd::Vector<dstd::StringView>& args);

   private:
    ArgSpec& add_value(Names names, dstd::StringView help,
                       dstd::String default_repr, bool has_default);
    ArgSpec& add_bytes(Names names, dstd::StringView help,
                       dstd::StringView default_text, bool unit_required,
                       dstd::Function<void(std::uint64_t)> store);
    ArgSpec& add_converted(Names names, dstd::StringView help,
                           dstd::StringView default_text,
                           ArgSpec::Setter convert);
    static dstd::String number_text(double value);
    template <class T>
    static dstd::String number_text(T value) {
        if constexpr (std::is_floating_point_v<T>) {
            return number_text(static_cast<double>(value));
        } else {
            return dstd::int_text(value);
        }
    }
    static dstd::String number_error(dstd::StringView name,
                                     dstd::StringView text);
    static dstd::StringView long_name(Names names);

    dstd::String program_;
    dstd::String version_;
    dstd::String description_;
    dstd::Vector<dstd::String> groups_;
    dstd::Vector<dstd::UniquePtr<ArgSpec>> specs_;
};

/// The NUL-terminated text of a view the parser produced. Every such view ends
/// at the end of an argv string, so data() is a valid C string.
inline const char* c_str(dstd::StringView view) { return view.data(); }

}  // namespace dftracer::utils::binaries

#endif  // DFTRACER_UTILS_BINARIES_CLI_ARGS_H
