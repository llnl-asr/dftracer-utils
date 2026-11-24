// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_PYTHON_FORMATS_EMIT_H
#define DFTRACER_UTILS_PYTHON_FORMATS_EMIT_H

#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/python/formats/format.h>

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace dftracer::utils::python::formats {

/// The events of a format writer, with the open containers kept, so a record
/// that fails half way is closed before the writer is destroyed: a format
/// writer ends the process in a debug build when it stops with a container
/// open. A NaN or an infinity is written as a string, as TraceWriter writes it.
template <class F>
class Emitter {
   public:
    Emitter(dstd::String &out, std::uint8_t indent)
        : writer_(F::make_writer(out, indent)) {}
    Emitter(const Emitter &) = delete;
    Emitter &operator=(const Emitter &) = delete;
    ~Emitter() { abort(); }

    void begin_object() {
        before_value();
        writer_.begin_object();
        frames_.push_back({false, false});
    }
    void begin_array() {
        before_value();
        writer_.begin_array();
        frames_.push_back({true, false});
    }
    void end() {
        const Frame frame = frames_.back();
        frames_.pop_back();
        if (frame.is_array)
            writer_.end_array();
        else
            writer_.end_object();
    }
    void key(dstd::StringView k) {
        writer_.key(k);
        frames_.back().key_pending = true;
    }
    void null() {
        before_value();
        writer_.null();
    }
    void boolean(bool v) {
        before_value();
        writer_.value(v);
    }
    void integer(std::int64_t v) {
        before_value();
        writer_.value(v);
    }
    void unsigned_integer(std::uint64_t v) {
        before_value();
        writer_.value(v);
    }
    void real(double v) {
        before_value();
        if (std::isnan(v))
            writer_.value("NaN");
        else if (std::isinf(v))
            writer_.value(dstd::StringView(v > 0 ? "Infinity" : "-Infinity"));
        else
            writer_.value(v);
    }
    void str(dstd::StringView v) {
        before_value();
        writer_.value(v);
    }
    /// One JSON value written as its source text; only a JSON writer has it.
    void raw_json(dstd::StringView text) {
        before_value();
        if constexpr (std::is_same_v<F, JsonFormat>) writer_.raw_value(text);
    }

    /// Closes every open container; a member whose value was not written gets
    /// a null.
    void abort() {
        while (!frames_.empty()) {
            if (!frames_.back().is_array && frames_.back().key_pending)
                writer_.null();
            frames_.back().key_pending = false;
            end();
        }
    }

   private:
    struct Frame {
        bool is_array;
        bool key_pending;
    };

    void before_value() {
        if (!frames_.empty()) frames_.back().key_pending = false;
    }

    typename F::Writer writer_;
    dstd::Vector<Frame> frames_;
};

/// The deepest nesting of a Python value or a parsed value that is written.
inline constexpr int MAX_EMIT_DEPTH = 1000;

/// Writes a parsed value of format S, a container or a scalar, to `e`.
template <class S, class D>
void emit_value(Emitter<D> &e, const typename S::Val &v);

/// Writes what a Python object holds to `e`: None, a bool, an int, a float, a
/// str, a Mapping with str keys, a Sequence that is not text, a Value of either
/// format or a MutableValue of either. False with a Python error set (TypeError
/// for any other object).
template <class D>
bool emit_py(Emitter<D> &e, PyObject *o, int depth);

}  // namespace dftracer::utils::python::formats

#endif  // DFTRACER_UTILS_PYTHON_FORMATS_EMIT_H
