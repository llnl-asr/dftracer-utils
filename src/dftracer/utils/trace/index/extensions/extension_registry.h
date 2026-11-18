// SPDX-License-Identifier: MIT
#ifndef DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_EXTENSION_REGISTRY_H
#define DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_EXTENSION_REGISTRY_H

#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/result.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/trace/index/store/file.h>

#include <cstdint>

namespace dftracer::utils::trace::index {
class FileBuild;
}

namespace dftracer::utils::trace::index::extensions {

/// Payload bytes an extension appends. Bytes past `cap` are dropped and set
/// `over`, so the payload is stored as nothing.
struct ExtensionOut {
    dstd::String bytes;
    std::uint64_t cap = 0;
    bool over = false;

    void append(dstd::StringView data) {
        if (over) return;
        if (data.size() > cap - bytes.size()) {
            over = true;
            return;
        }
        bytes.append(data.data(), data.size());
    }
};

/// Builds one file's payloads. Used by one thread at a time.
class ExtensionBuilder {
   public:
    virtual ~ExtensionBuilder() = default;

    /// The next record lines of the current chunk, in file order. The lines are
    /// borrowed for the call. A failure stores nothing for the chunk.
    virtual dstd::Result<void> step(
        dstd::Span<const dstd::StringView> lines) = 0;
    /// Ends chunk `granule` and appends its payload to `out`.
    virtual dstd::Result<void> finish_granule(std::uint64_t granule,
                                              ExtensionOut& out) = 0;
    /// False when the extension has no file payload.
    virtual bool has_finish_file() const = 0;
    /// Ends the file and appends its payload to `out`.
    virtual dstd::Result<void> finish_file(ExtensionOut& out) = 0;
};

/// The operator of a filter leaf.
enum class ExtensionOp {
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    In,
    NotIn,
    Like,
    Ilike,
    Regex,
    Iregex,
    Icontains,
};

/// One scalar of a filter leaf.
struct ExtensionValue {
    enum class Kind { Bool, Int64, Double, String };

    Kind kind = Kind::Int64;
    bool b = false;
    std::int64_t i = 0;
    double f = 0;
    /// The String kind borrows the string, which outlives the compile call.
    const dstd::String* str = nullptr;
};

/// One filter leaf: `path op value`. Borrowed for the compile call only.
struct ExtensionLeaf {
    dstd::StringView path;
    bool any = false;
    ExtensionOp op = ExtensionOp::Eq;
    /// A scalar; the pattern as a String for Like through Icontains; unused
    /// for In and NotIn.
    ExtensionValue value;
    /// The scalars of In and NotIn.
    dstd::Span<const ExtensionValue> items;
};

/// A leaf prepared for many payloads. May be used by several threads.
class ExtensionQuery {
   public:
    virtual ~ExtensionQuery() = default;

    /// False when chunk payload `payload` proves that no record in the chunk
    /// satisfies the leaf.
    virtual bool may_match(dstd::StringView payload) const = 0;
    /// As @ref may_match, for the file payload; only when the extension
    /// @ref Extension::has_file_check.
    virtual bool file_may_match(dstd::StringView payload) const = 0;
};

/// An index extension: builds payloads per chunk and file during an index
/// build, and judges them per filter leaf at query time. Methods may run on
/// any worker thread.
class Extension {
   public:
    virtual ~Extension() = default;

    virtual dstd::StringView name() const = 0;
    /// Data built with another version or params hash is not used.
    virtual std::uint32_t version() const = 0;
    virtual std::uint64_t params_hash() const = 0;
    /// The largest payload stored; 0 means 1 MiB.
    virtual std::uint64_t max_payload() const = 0;

    /// False when the extension judges no file payload.
    virtual bool has_file_check() const = 0;

    /// A builder for one file; on failure nothing is stored for the file.
    virtual dstd::Result<dstd::UniquePtr<ExtensionBuilder>> make_builder()
        const = 0;
    /// Prepares `leaf`. A null query means no evidence for the leaf.
    virtual dstd::Result<dstd::UniquePtr<ExtensionQuery>> compile(
        const ExtensionLeaf& leaf) const = 0;

    /// Built at this extension's version and params hash, and not failed.
    bool current(const store::ExtensionState& state) const {
        return state.ready && state.version == version() &&
               state.params_hash == params_hash();
    }
    /// max_payload, with 0 replaced by the default.
    std::uint64_t payload_cap() const;
};

using ExtensionPtr = dstd::SharedPtr<const Extension>;

/// Adds `extension` under its name. Fails when the name is taken.
dstd::Result<void> register_extension(ExtensionPtr extension);
void unregister_extension(dstd::StringView name);
/// The registered extensions, sorted by name. The entries stay valid after
/// unregistration.
dstd::Vector<ExtensionPtr> registered_extensions();
ExtensionPtr find_extension(dstd::StringView name);

/// One file's builders, one per extension, fed chunk by chunk.
class ExtensionBuilders {
   public:
    explicit ExtensionBuilders(dstd::Vector<ExtensionPtr> extensions);
    ~ExtensionBuilders();
    ExtensionBuilders(const ExtensionBuilders&) = delete;
    ExtensionBuilders& operator=(const ExtensionBuilders&) = delete;

    /// Record lines of chunk `granule`; a new granule ends the previous one.
    void step(std::uint64_t granule, dstd::Span<const dstd::StringView> lines);
    /// Ends the last chunk and the file.
    void finish();
    /// Writes each extension's data into `build`, which replaces the file's
    /// copy of it when committed.
    void write(FileBuild& build);

   private:
    struct Built {
        ExtensionPtr ext;
        dstd::UniquePtr<ExtensionBuilder> builder;
        bool chunk_failed = false;
        bool step_failed = false;
        bool reported = false;
        dstd::Vector<dstd::Pair<std::uint64_t, dstd::String>> granules;
        dstd::Optional<dstd::String> file;
    };
    void end_granule();
    static void report(Built& built, dstd::StringView callback,
                       const dstd::Error& error);

    dstd::Vector<Built> built_;
    dstd::Optional<std::uint64_t> granule_;
};

}  // namespace dftracer::utils::trace::index::extensions

#endif  // DFTRACER_UTILS_TRACE_INDEX_EXTENSIONS_EXTENSION_REGISTRY_H
