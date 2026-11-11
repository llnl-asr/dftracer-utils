// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/calendar.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/input_stat.h>
#include <dftracer/utils/dstd/ascii.h>
#include <dftracer/utils/dstd/cstring_view.h>
#include <dftracer/utils/dstd/fatal.h>
#include <dftracer/utils/dstd/file.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/map.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/pair.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/dstd/shutdown.h>
#include <dftracer/utils/dstd/singleton.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/evaluator.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/fileio/compress/zng_stream.h>
#include <dftracer/utils/fileio/hash/hasher_utility.h>
#include <dftracer/utils/json/dom_form.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/trace/index/record_schema.h>
#include <dftracer/utils/trace/index/record_schema_yaml.h>
#include <dftracer/utils/trace/index/source.h>
#include <fcntl.h>
#include <simdjson.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace dftracer::utils::trace::index {

namespace {

constexpr std::size_t CHUNK = 64 * 1024;
// Detection stops starting new lines past this many bytes of text.
constexpr std::size_t MAX_TEXT = 16u * 1024 * 1024;
// A longer line ends detection; no sample record is this large.
constexpr std::size_t MAX_RECORD = 256u * 1024 * 1024;

// Feeds `emit` the lines from the start of a gzip or plain file, the last one
// without its newline too, until it returns false or MAX_TEXT bytes were
// read.
template <typename Emit>
dstd::Result<void> for_each_head_line(const dstd::File& file,
                                      dstd::StringView path, Emit&& emit) {
    dstd::Vector<char> comp(CHUNK);
    std::uint64_t offset = 0;
    // Returns the byte count, 0 at the end and -1 on error.
    auto read_chunk = [&](std::size_t n) -> ssize_t {
        const auto got = file.read_at(
            offset, dstd::Span<std::uint8_t>(
                        reinterpret_cast<std::uint8_t*>(comp.data()), n));
        if (!got) return -1;
        offset += *got;
        return static_cast<ssize_t>(*got);
    };
    const bool gz = read_chunk(2) == 2 &&
                    static_cast<unsigned char>(comp[0]) == 0x1f &&
                    static_cast<unsigned char>(comp[1]) == 0x8b;
    offset = 0;
    dstd::String text;
    std::size_t consumed = 0;
    bool done = false;
    auto feed = [&](const char* data, std::size_t n) {
        // The text held from before has no newline.
        const std::size_t from = text.size();
        text.append(data, n);
        std::size_t pos = 0;
        for (;;) {
            const auto nl = text.find('\n', std::max(pos, from));
            if (nl == dstd::String::npos) break;
            consumed += nl + 1 - pos;
            if (!emit(dstd::StringView(text).substr(pos, nl - pos)) ||
                consumed >= MAX_TEXT) {
                done = true;
                return;
            }
            pos = nl + 1;
        }
        text.erase(0, pos);
        if (text.size() > MAX_RECORD) done = true;
    };
    if (!gz) {
        for (ssize_t got; !done && (got = read_chunk(CHUNK)) > 0;)
            feed(comp.data(), static_cast<std::size_t>(got));
    } else {
        zng_stream zs{};
        // Concatenated members decode as one stream (inflateReset at each
        // member end).
        if (fileio::compress::open_inflate_stream(zs, 16 + MAX_WBITS) != Z_OK)
            return common::make_error<"cannot inflate trace: {}">(
                common::ErrorCode::Io, path);
        dstd::Vector<char> out(CHUNK);
        bool eof = false;
        while (!done) {
            if (zs.avail_in == 0 && !eof) {
                const ssize_t got = read_chunk(CHUNK);
                eof = got <= 0;
                zs.next_in = reinterpret_cast<const std::uint8_t*>(comp.data());
                zs.avail_in = eof ? 0 : static_cast<std::uint32_t>(got);
            }
            zs.next_out = reinterpret_cast<std::uint8_t*>(out.data());
            zs.avail_out = static_cast<std::uint32_t>(out.size());
            const int rc = zng_inflate(&zs, Z_NO_FLUSH);
            const std::size_t got = out.size() - zs.avail_out;
            feed(out.data(), got);
            if (rc == Z_STREAM_END) {
                if ((eof && zs.avail_in == 0) || zng_inflateReset(&zs) != Z_OK)
                    break;
            } else if ((rc != Z_OK && rc != Z_BUF_ERROR) ||
                       (eof && zs.avail_in == 0 && got == 0)) {
                break;
            }
        }
        zng_inflateEnd(&zs);
    }
    if (!done && !text.empty()) emit(dstd::StringView(text));
    return {};
}

// Scores the registered schemas on lines fed one at a time. A schema judges
// only the records its `data` row set does not leave out, so metadata lines
// neither match nor miss it.
class Detector {
   public:
    Detector(dstd::Vector<const RecordSchema*> candidates,
             const RecordSchema* generic)
        : candidates_(std::move(candidates)), generic_(generic) {
        judged_.assign(candidates_.size(), 0);
        hits_.assign(candidates_.size(), 0);
        cond_of_.assign(candidates_.size(), NONE);
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            const RecordSchema* s = candidates_[i];
            if (s->data.empty()) continue;
            auto same =
                std::find_if(conds_.begin(), conds_.end(), [&](const Cond& c) {
                    return c.text == s->data &&
                           c.args_fallback == s->args_fallback;
                });
            if (same == conds_.end()) {
                auto parsed = duql::Query::from_string(s->data);
                if (!parsed)
                    dstd::fatal(
                        "a registered record schema data condition does not "
                        "parse",
                        parsed.error().message.c_str());
                conds_.push_back(
                    {s->data, s->args_fallback, std::move(*parsed), 0});
                same = std::prev(conds_.end());
            }
            cond_of_[i] = static_cast<std::size_t>(same - conds_.begin());
        }
        truth_.resize(conds_.size());
    }

    // Whether more lines are wanted.
    bool add(dstd::StringView line) {
        const char* start = nullptr;
        std::size_t length = 0;
        if (!json::trim_and_validate_with_comma(line.data(), line.size(), start,
                                                length))
            return true;
        simdjson::dom::element root;
        if (parser_.parse(start, length).get(root) != simdjson::SUCCESS &&
            json::parse_refused(parser_, dstd::StringView(start, length))
                    .get(root) != simdjson::SUCCESS)
            return true;
        if (!root.is_object()) return true;
        ++out_.objects;
        const json::JsonValue record(root);
        bool metadata = false;
        for (std::size_t k = 0; k < conds_.size(); ++k) {
            truth_[k] = duql::evaluate_truth(
                duql::detail::QueryAccess::root(conds_[k].query), record,
                conds_[k].args_fallback);
            if (truth_[k] == duql::Truth::No) {
                metadata = true;
                ++conds_[k].left_out;
            }
        }
        out_.records += !metadata;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (cond_of_[i] != NONE && truth_[cond_of_[i]] == duql::Truth::No)
                continue;
            ++judged_[i];
            hits_[i] += matches(*candidates_[i], record);
        }
        return out_.records < SCHEMA_SAMPLE_LINES;
    }

    SchemaDetection finish() {
        const RecordSchema* best = nullptr;
        double best_share = 0;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            const RecordSchema* c = candidates_[i];
            const double share = judged_[i]
                                     ? static_cast<double>(hits_[i]) /
                                           static_cast<double>(judged_[i])
                                     : 0.0;
            out_.scores.push_back({c, share});
            if (c->fields.empty() || judged_[i] == 0 ||
                static_cast<double>(hits_[i]) <
                    SCHEMA_MATCH_SHARE * static_cast<double>(judged_[i]))
                continue;
            if (!best || more_specific(*c, share, *best, best_share)) {
                best = c;
                best_share = share;
            }
        }
        if (!best && out_.records == 0) best = metadata_owner();
        out_.chosen = best ? best : generic_;
        return std::move(out_);
    }

   private:
    static constexpr std::size_t NONE = static_cast<std::size_t>(-1);

    struct Cond {
        dstd::String text;
        bool args_fallback = false;
        duql::Query query;
        std::size_t left_out = 0;
    };

    // Every required path present; for a schema whose fields are all
    // optional, any declared path present.
    static bool matches(const RecordSchema& s, const json::JsonValue& record) {
        if (s.require.empty())
            return std::any_of(
                s.fields.begin(), s.fields.end(),
                [&](const FieldSpec& f) { return record.at(f.path).exists(); });
        return std::all_of(
            s.require.begin(), s.require.end(),
            [&](const dstd::String& path) { return record.at(path).exists(); });
    }

    // More required paths, then a user schema over a built-in, then the
    // higher share, then the lower id.
    static bool more_specific(const RecordSchema& a, double a_share,
                              const RecordSchema& b, double b_share) {
        if (a.require.size() != b.require.size())
            return a.require.size() > b.require.size();
        if (a.builtin != b.builtin) return !a.builtin;
        if (a_share != b_share) return a_share > b_share;
        return a.id < b.id;
    }

    // With only metadata sampled, the least specific schema whose `data`
    // row set leaves out every object: no record says which child it is.
    const RecordSchema* metadata_owner() const {
        const RecordSchema* best = nullptr;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            const RecordSchema* c = candidates_[i];
            if (cond_of_[i] == NONE || out_.objects == 0 ||
                conds_[cond_of_[i]].left_out != out_.objects)
                continue;
            if (!best || c->require.size() < best->require.size() ||
                (c->require.size() == best->require.size() && c->builtin &&
                 !best->builtin))
                best = c;
        }
        return best;
    }

    dstd::Vector<const RecordSchema*> candidates_;
    const RecordSchema* generic_;
    dstd::Vector<Cond> conds_;
    dstd::Vector<std::size_t> cond_of_;
    dstd::Vector<duql::Truth> truth_;
    dstd::Vector<std::size_t> judged_;
    dstd::Vector<std::size_t> hits_;
    dftracer::utils::json::RecordParser parser_;
    SchemaDetection out_;
};

}  // namespace

double micros_per(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::Ns:
            return 1e-3;
        case TimeUnit::Us:
            return 1;
        case TimeUnit::Ms:
            return 1e3;
        case TimeUnit::S:
            return 1e6;
    }
    return 1;
}

const FieldSpec* RecordSchema::field_at(dstd::StringView path) const {
    const auto it = std::lower_bound(
        fields_by_path.begin(), fields_by_path.end(), path,
        [](const auto& e, dstd::StringView p) { return e.first < p; });
    return it != fields_by_path.end() && it->first == path ? &fields[it->second]
                                                           : nullptr;
}

std::uint64_t RecordSchema::params_hash() const {
    fileio::hash::HasherUtility hasher;
    auto text = [&](dstd::StringView s) {
        hasher.update(s);
        hasher.update(std::uint8_t{0});
    };
    auto hash_evidence_overrides = [&] {
        hasher.update(static_cast<std::uint8_t>(path_budget.has_value()));
        hasher.update(path_budget.value_or(0));
        if (stats_share) {
            hasher.update(std::uint8_t{1});
            hasher.update(*stats_share);
        }
    };
    if (builtin) {
        // The layout the built-ins' indexes were recorded with.
        text(id);
        // Role paths are hashed below, so requiring one keeps the hash.
        for (const auto& r : require)
            if (r != roles.time && r != roles.duration && r != roles.entity)
                text(r);
        hasher.update(std::uint8_t{1});
        for (const auto* r :
             {&roles.time, &roles.duration, &roles.entity, &roles.phase})
            text(*r);
        hasher.update(static_cast<std::uint8_t>(decoder));
        if (!source.empty()) text(source);
        if (path_budget || stats_share) hash_evidence_overrides();
        return hasher.hash().value;
    }
    hasher.update(std::uint8_t{3});
    text(id);
    hasher.update(static_cast<std::uint8_t>(decoder));
    dstd::Vector<const FieldSpec*> sorted;
    for (const auto& f : fields) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(),
              [](const FieldSpec* a, const FieldSpec* b) {
                  return a->name < b->name;
              });
    for (const FieldSpec* f : sorted) {
        text(f->name);
        text(f->path);
        hasher.update(static_cast<std::uint8_t>(f->type));
        hasher.update(static_cast<std::uint8_t>(f->optional));
        hasher.update(static_cast<std::uint8_t>(f->role));
        hasher.update(
            static_cast<std::uint8_t>(f->unit.value_or(TimeUnit::Us)));
        hasher.update(static_cast<std::uint8_t>(f->always_index));
    }
    text(roles.phase);
    if (!source.empty()) text(source);
    for (const auto& path : never_index) text(path);
    hash_evidence_overrides();
    return hasher.hash().value;
}

namespace {

// Sets the data a schema's fields imply: required paths, roles and the
// paths indexed past the budget.
void derive(RecordSchema& s) {
    s.require.clear();
    s.always_index.clear();
    s.fields_by_path.clear();
    s.has_json = std::any_of(
        s.fields.begin(), s.fields.end(),
        [](const FieldSpec& f) { return f.type == FieldType::Json; });
    for (std::size_t i = 0; i < s.fields.size(); ++i)
        s.fields_by_path.emplace_back(s.fields[i].path, i);
    std::sort(s.fields_by_path.begin(), s.fields_by_path.end());
    const dstd::String phase = s.roles.phase;
    s.roles = Roles{};
    s.roles.phase = phase;
    auto add = [](dstd::Vector<dstd::String>& to, const dstd::String& path) {
        if (std::find(to.begin(), to.end(), path) == to.end())
            to.push_back(path);
    };
    for (const auto& f : s.fields) {
        if (!f.optional) add(s.require, f.path);
        if (f.always_index) add(s.always_index, f.path);
        switch (f.role) {
            case Role::None:
                break;
            case Role::Time:
                s.roles.time = f.path;
                s.roles.time_unit = f.unit.value_or(TimeUnit::Us);
                if (s.decoder == Decoder::Path) add(s.always_index, f.path);
                break;
            case Role::Duration:
                s.roles.duration = f.path;
                s.roles.duration_unit = f.unit.value_or(TimeUnit::Us);
                if (s.decoder == Decoder::Path) add(s.always_index, f.path);
                break;
            case Role::Entity:
                s.roles.entity = f.path;
                if (s.decoder == Decoder::Path) add(s.always_index, f.path);
                break;
            case Role::Lane:
                s.roles.lane = f.path;
                if (s.decoder == Decoder::Path) add(s.always_index, f.path);
                break;
            case Role::Name:
                s.roles.name = f.path;
                if (s.decoder == Decoder::Path) add(s.always_index, f.path);
                break;
        }
    }
}

FieldSpec builtin_field(dstd::String name, FieldType type, bool optional,
                        Role role) {
    FieldSpec f;
    f.path = name;
    f.name = std::move(name);
    f.type = type;
    f.optional = optional;
    f.role = role;
    if (role == Role::Time || role == Role::Duration) f.unit = TimeUnit::Us;
    return f;
}

dstd::String built_source(const duql::Source& source) {
    auto text = source.text();
    if (!text)
        dstd::fatal("a built-in record schema source does not build",
                    text.error().message.c_str());
    return std::move(*text);
}

dstd::String built_merge(const duql::Source& source, dstd::StringView origin) {
    auto merged = merge_source({}, built_source(source), origin);
    if (!merged)
        dstd::fatal("a built-in record schema source does not merge",
                    merged.error().c_str());
    return std::move(*merged);
}

dstd::Vector<RecordSchema> builtins() {
    RecordSchema dft;
    dft.id = "dftracer";
    dft.decoder = Decoder::Dftracer;
    dft.builtin = true;
    dft.fields = {
        builtin_field("ph", FieldType::String, false, Role::None),
        builtin_field("name", FieldType::String, false, Role::Name),
        builtin_field("ts", FieldType::Int, false, Role::Time),
        builtin_field("dur", FieldType::Int, true, Role::Duration),
        builtin_field("pid", FieldType::Int, true, Role::Entity),
        builtin_field("tid", FieldType::Int, true, Role::Lane),
    };
    dft.roles.phase = "ph";
    {
        using namespace duql;
        const Col meta = c("ph").is_in(dstd::Vector<Col>{"M", 4});
        const auto dict = [&](const char* tag) {
            return Pipe().where(meta && c("name") == tag);
        };
        dft.source = built_merge(
            Source()
                .rowset("data",
                        Pipe().where(c("ph").not_in(dstd::Vector<Col>{"M", 4})))
                .rowset("files", dict("FH")
                                     .select({{"fhash", c("args.value")},
                                              {"path", c("args.name")}})
                                     .distinct())
                .rowset("hosts", dict("HH")
                                     .select({{"hhash", c("args.value")},
                                              {"name", c("args.name")}})
                                     .distinct())
                .rowset("strings", dict("SH")
                                       .select({{"shash", c("args.value")},
                                                {"value", c("args.name")}})
                                       .distinct())
                .rowset("ranks", Pipe()
                                     .where(meta && c("name") == "PR" &&
                                            c("args.name") == "rank")
                                     .select({"pid", {"rank", c("args.value")}})
                                     .distinct())
                .flag("args_fallback", true),
            "dftracer");
    }
    dft.data = data_condition(dft);
    dft.args_fallback = source_args_fallback(dft);
    derive(dft);

    RecordSchema generic;
    generic.id = "generic";
    generic.builtin = true;
    derive(generic);

    RecordSchema genesis;
    genesis.id = "genesis";
    genesis.builtin = true;
    genesis.never_index = {"dur.sketch", "v.sketch"};
    genesis.fields = {
        builtin_field("gtype", FieldType::String, false, Role::None),
        builtin_field("run", FieldType::String, false, Role::None),
        builtin_field("ts", FieldType::Int, true, Role::Time),
    };
    {
        using namespace duql;
        genesis.source = built_merge(
            Source()
                .rowset("data", Pipe().where(c("gtype") != "run"))
                .rowset("runs",
                        Pipe()
                            .where(c("gtype") == "run")
                            .select({"run", "app", "system", "unique_input",
                                     "nodes", "ppn", "papi_set", "method",
                                     "sketch_accuracy", "leaf"})
                            .distinct()),
            "genesis");
    }
    genesis.data = data_condition(genesis);
    genesis.args_fallback = source_args_fallback(genesis);
    derive(genesis);
    return {std::move(dft), std::move(generic), std::move(genesis)};
}

struct Entry {
    dstd::UniquePtr<RecordSchema> schema;
    dstd::String source;
};

dstd::Unexpected<dstd::Error> spec_error(const dstd::String& source,
                                         dstd::StringView what) {
    return common::make_error<"record schema spec {}: {}">(
        common::ErrorCode::InvalidArgument, source, what);
}

class Registry {
   public:
    Registry() {
        for (auto& s : builtins())
            entries_.push_back(
                {dstd::make_unique<RecordSchema>(std::move(s)), "built-in"});
    }

    const RecordSchema* find(dstd::StringView id) const {
        std::shared_lock lock(mu_);
        return find_locked(id);
    }

    std::size_t size() const {
        std::shared_lock lock(mu_);
        return entries_.size();
    }

    dstd::Vector<const RecordSchema*> all() const {
        std::shared_lock lock(mu_);
        dstd::Vector<const RecordSchema*> out;
        out.reserve(entries_.size());
        for (const auto& e : entries_) out.push_back(e.schema.get());
        return out;
    }

    dstd::Result<const RecordSchema*> add(const SchemaSpec& spec,
                                          const dstd::String& source);

    // Whether `path` was loaded before; marks it loaded.
    bool first_load(dstd::StringView path) {
        std::lock_guard lock(loaded_mu_);
        return loaded_.emplace(path).second;
    }
    void forget_load(dstd::StringView path) {
        std::lock_guard lock(loaded_mu_);
        const auto found = loaded_.find(path);
        if (found != loaded_.end()) loaded_.erase(found);
    }
    // Frees every schema so pool memory is not alive at exit.
    void release() {
        {
            std::lock_guard lock(loaded_mu_);
            loaded_.clear();
        }
        std::unique_lock lock(mu_);
        dstd::Vector<Entry> released;
        entries_.swap(released);
    }

   private:
    const RecordSchema* find_locked(dstd::StringView id) const {
        const Entry* e = entry_locked(id);
        return e ? e->schema.get() : nullptr;
    }
    const Entry* entry_locked(dstd::StringView id) const {
        for (const auto& e : entries_)
            if (e.schema->id == id) return &e;
        return nullptr;
    }

    mutable std::shared_mutex mu_;
    dstd::Vector<Entry> entries_;
    std::mutex loaded_mu_;
    dstd::Set<dstd::String> loaded_;
};

dstd::StringView role_name(Role role) {
    switch (role) {
        case Role::None:
            return "none";
        case Role::Time:
            return "time";
        case Role::Duration:
            return "duration";
        case Role::Entity:
            return "entity";
        case Role::Lane:
            return "lane";
        case Role::Name:
            return "name";
    }
    return "none";
}

dstd::Result<const RecordSchema*> Registry::add(const SchemaSpec& spec,
                                                const dstd::String& source) {
    if (spec.id.empty()) return spec_error(source, "missing id");
    std::unique_lock lock(mu_);
    if (const Entry* e = entry_locked(spec.id); e && e->source == "built-in")
        return spec_error(
            source,
            dstd::fmt<"built-in schema {} cannot be redefined">(spec.id));
    if (spec.extends == spec.id)
        return spec_error(source,
                          dstd::fmt<"schema {} extends itself">(spec.id));
    const RecordSchema* parent = find_locked(spec.extends);
    if (!parent)
        return spec_error(source,
                          dstd::fmt<"unknown extends {}">(spec.extends));
    RecordSchema s = *parent;
    s.id = spec.id;
    s.builtin = false;
    for (FieldSpec f : spec.fields) {
        if (f.name.empty()) return spec_error(source, "a field has no name");
        const dstd::String where = dstd::fmt<"fields.{}">(f.name);
        if (f.path.empty()) f.path = f.name;
        const bool timed = f.role == Role::Time || f.role == Role::Duration;
        if (f.unit && !timed)
            return spec_error(
                source,
                dstd::fmt<"{}.unit needs the time or duration role">(where));
        if (timed) {
            // A string time is ISO-8601 text.
            const bool iso =
                f.role == Role::Time && f.type == FieldType::String;
            if (f.type != FieldType::Int && f.type != FieldType::Float && !iso)
                return spec_error(
                    source,
                    f.role == Role::Time
                        ? dstd::fmt<"{} has the {} role and must be int, float "
                                    "or string">(where, role_name(f.role))
                        : dstd::fmt<"{} has the {} role and must be int or "
                                    "float">(where, role_name(f.role)));
            if (iso && f.unit)
                return spec_error(
                    source, dstd::fmt<"{}.unit does not apply to an ISO-8601 "
                                      "string time">(where));
            f.unit = f.unit.value_or(TimeUnit::Us);
        }
        if ((f.role == Role::Entity || f.role == Role::Lane ||
             f.role == Role::Name) &&
            f.type != FieldType::Int && f.type != FieldType::String)
            return spec_error(
                source,
                dstd::fmt<"{} has the {} role and must be int or string">(
                    where, role_name(f.role)));
        if (f.type == FieldType::Json && s.decoder != Decoder::Path)
            return spec_error(source,
                              dstd::fmt<"{} is json, which needs a schema that "
                                        "decodes records by path">(where));
        auto same =
            std::find_if(s.fields.begin(), s.fields.end(),
                         [&](const FieldSpec& x) { return x.name == f.name; });
        if (same != s.fields.end())
            *same = std::move(f);
        else
            s.fields.push_back(std::move(f));
    }
    for (std::size_t i = 0; i < s.fields.size(); ++i)
        for (std::size_t j = i + 1; j < s.fields.size(); ++j) {
            const FieldSpec& a = s.fields[i];
            const FieldSpec& b = s.fields[j];
            if (a.path == b.path)
                return spec_error(
                    source, dstd::fmt<"fields.{} and fields.{} share the path "
                                      "{}">(a.name, b.name, a.path));
            if (a.role != Role::None && a.role == b.role)
                return spec_error(
                    source,
                    dstd::fmt<"fields.{} and fields.{} both have the {} role">(
                        a.name, b.name, role_name(a.role)));
        }
    for (const auto& path : spec.never_index) {
        const auto it =
            std::find_if(s.fields.begin(), s.fields.end(),
                         [&](const FieldSpec& x) { return x.path == path; });
        const FieldSpec* f = it == s.fields.end() ? nullptr : &*it;
        if (f && f->role != Role::None)
            return spec_error(
                source,
                dstd::fmt<"index.never_index lists {}, which has the {} role">(
                    path, role_name(f->role)));
        if (f && f->always_index)
            return spec_error(source,
                              dstd::fmt<"index.never_index lists {}, which is "
                                        "always_index">(path));
        if (s.decoder != Decoder::Path)
            return spec_error(source,
                              "index.never_index needs a schema that decodes "
                              "records by path");
        s.never_index.push_back(path);
    }
    std::sort(s.never_index.begin(), s.never_index.end());
    s.never_index.erase(std::unique(s.never_index.begin(), s.never_index.end()),
                        s.never_index.end());
    if (spec.path_budget) s.path_budget = spec.path_budget;
    if (spec.stats_share) s.stats_share = spec.stats_share;
    auto merged = merge_source(s.source, spec.source,
                               dstd::fmt<"{}, schema {}">(source, spec.id));
    if (!merged) return merged.error();
    s.source = std::move(*merged);
    s.data = data_condition(s);
    if (!s.data.empty()) {
        const auto parsed = duql::Query::from_string(s.data);
        if (!parsed)
            return spec_error(
                source,
                dstd::fmt<"schema {}: the data row set is not a condition "
                          "detection can evaluate: {}">(
                    spec.id, parsed.error().message));
    }
    s.args_fallback = source_args_fallback(s);
    derive(s);
    if (auto checked = check_source(s, source); !checked)
        return checked.error();

    if (const Entry* e = entry_locked(s.id)) {
        if (e->schema->params_hash() == s.params_hash()) return e->schema.get();
        return spec_error(
            source, dstd::fmt<"schema {} is already registered from {} with "
                              "another definition">(s.id, e->source));
    }
    entries_.push_back({dstd::make_unique<RecordSchema>(std::move(s)), source});
    return entries_.back().schema.get();
}

bool spec_file(dstd::PathView p) {
    const dstd::StringView ext = p.extension();
    return ext == ".yaml" || ext == ".yml" || ext == ".json";
}

dstd::Result<const RecordSchema*> register_into(Registry& reg,
                                                dstd::StringView text,
                                                const dstd::String& source) {
    auto spec = parse_schema_spec(text, source);
    if (!spec) return spec.error();
    return reg.add(*spec, source);
}

dstd::Result<void> load_files(Registry& reg, const dstd::Path& p) {
    dstd::Vector<dstd::Path> files;
    auto is_dir = dstd::fs::is_directory(p.view());
    if (!is_dir.has_value()) return is_dir.error();
    if (*is_dir) {
        auto dir = dstd::fs::Dir::open(p.view());
        if (!dir) return dir.error();
        auto listing = dir->entries();
        if (!listing) return listing.error();
        for (const dstd::fs::DirEntry& e : *listing) {
            if (!spec_file(dstd::PathView(e.name))) continue;
            const auto st = dir->stat(e.name);
            if (st && st->kind == dstd::fs::FileKind::File) {
                files.push_back(p.view() / e.name);
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const dstd::Path& a, const dstd::Path& b) {
                      return a.str() < b.str();
                  });
    } else {
        files.push_back(p);
    }
    for (const auto& f : files) {
        auto text = dstd::fs::read_file(f.view());
        if (!text)
            return common::make_error<"cannot read record schema spec {}">(
                common::ErrorCode::Io, f.str());
        auto added = register_into(reg, *text, dstd::String(f.str()));
        if (!added) return added.error();
    }
    return {};
}

dstd::Result<void> load_into(Registry& reg, dstd::StringView path) {
    const auto canonical = dstd::fs::weakly_canonical(dstd::PathView(path));
    const dstd::Path p = canonical ? *canonical : dstd::Path(path);
    const dstd::StringView key = p.str();
    if (!reg.first_load(key)) return {};
    auto loaded = load_files(reg, p);
    if (!loaded) reg.forget_load(key);
    return loaded;
}

struct RegistryState {
    std::mutex mu;
    bool env_loaded = false;
};

// Loads $DFTRACER_UTILS_SCHEMA_PATH on first use. A failed load is returned
// and the next access retries.
dstd::Result<Registry*> try_registry() {
    Registry& r = dstd::Singleton<Registry>::get();
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            dstd::Singleton<Registry>::get().release();
        });
        return true;
    }();
    static RegistryState state;
    std::lock_guard lock(state.mu);
    if (state.env_loaded) return &r;
    if (const auto paths = common::Env::get("DFTRACER_UTILS_SCHEMA_PATH")) {
        dstd::StringView rest = *paths;
        while (!rest.empty()) {
            const std::size_t colon = rest.find(':');
            const dstd::StringView part = rest.substr(0, colon);
            rest.remove_prefix(colon == dstd::StringView::npos ? rest.size()
                                                               : colon + 1);
            if (part.empty()) continue;
            if (auto loaded = load_into(r, part); !loaded)
                return loaded.error();
        }
    }
    state.env_loaded = true;
    return &r;
}

template <typename E, std::size_t N>
const char* name_of(E value,
                    const std::array<dstd::Pair<const char*, E>, N>& names) {
    for (const auto& [name, v] : names)
        if (v == value) return name;
    return "";
}

}  // namespace

dstd::StringView field_type_name(FieldType type) {
    return name_of(type, TYPE_NAMES);
}

dstd::Result<dstd::Vector<const RecordSchema*>> registered_schemas() {
    auto reg = try_registry();
    if (!reg) return reg.error();
    return (*reg)->all();
}

dstd::Result<const RecordSchema*> find_schema(dstd::StringView id) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    return (*reg)->find(id);
}

dstd::Result<const RecordSchema*> get_schema(dstd::StringView id) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    if (const RecordSchema* s = (*reg)->find(id)) return s;
    dstd::String ids;
    for (const RecordSchema* s : (*reg)->all()) {
        if (!ids.empty()) ids += ", ";
        ids += s->id;
    }
    return common::make_error<
        "unknown record schema {} (registered: {}); user schemas load from "
        "$DFTRACER_UTILS_SCHEMA_PATH and <index_dir>/schemas/">(
        common::ErrorCode::InvalidArgument, id, ids);
}

dstd::Result<const RecordSchema*> register_schema(const SchemaSpec& spec,
                                                  dstd::StringView source) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    return (*reg)->add(spec, dstd::String(source));
}

dstd::Result<const RecordSchema*> register_schema(dstd::StringView text,
                                                  dstd::StringView source) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    return register_into(**reg, text, dstd::String(source));
}

dstd::Result<void> load_schemas(dstd::StringView path) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    return load_into(**reg, path);
}

dstd::Result<dstd::String> schemas_json() {
    const auto all = registered_schemas();
    if (!all) return all.error();
    dstd::String out = "[";
    auto quoted = [&](dstd::StringView s) {
        out += '"';
        json::append_json_escaped(out, s);
        out += '"';
    };
    auto list = [&](const dstd::Vector<dstd::String>& xs) {
        out += '[';
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i) out += ',';
            quoted(xs[i]);
        }
        out += ']';
    };
    bool first = true;
    for (const RecordSchema* s : *all) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":";
        quoted(s->id);
        out += ",\"decoder\":";
        quoted(s->decoder == Decoder::Dftracer ? "dftracer" : "path");
        out += ",\"fields\":[";
        for (std::size_t i = 0; i < s->fields.size(); ++i) {
            const FieldSpec& f = s->fields[i];
            if (i) out += ',';
            out += "{\"name\":";
            quoted(f.name);
            out += ",\"path\":";
            quoted(f.path);
            out += ",\"type\":";
            quoted(name_of(f.type, TYPE_NAMES));
            out += ",\"optional\":";
            out += f.optional ? "true" : "false";
            out += ",\"role\":";
            if (f.role == Role::None)
                out += "null";
            else
                quoted(role_name(f.role));
            out += ",\"unit\":";
            if (f.unit)
                quoted(name_of(*f.unit, UNIT_NAMES));
            else
                out += "null";
            out += ",\"always_index\":";
            out += f.always_index ? "true" : "false";
            out += '}';
        }
        out += "],\"require\":";
        list(s->require);
        out += ",\"path_budget\":";
        out += s->path_budget ? dstd::to_string(*s->path_budget) : "null";
        out += ",\"stats_share\":";
        out += s->stats_share ? dstd::to_string(*s->stats_share) : "null";
        out += ",\"never_index\":";
        list(s->never_index);
        out += ",\"source\":";
        quoted(s->source);
        out += '}';
    }
    out += ']';
    return dstd::String(out);
}

dstd::Result<void> load_index_schemas(dstd::StringView index_path) {
    const dstd::Path dir = dstd::PathView(index_path).parent() / "schemas";
    const auto is_dir = dstd::fs::is_directory(dir.view());
    if (!is_dir.has_value()) return is_dir.error();
    if (*is_dir) return load_schemas(dir.str());
    return {};
}

dstd::Result<SchemaDetection> explain_schema(
    dstd::Span<const dstd::StringView> lines) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    Detector d((*reg)->all(), (*reg)->find("generic"));
    for (auto line : lines) d.add(line);
    return d.finish();
}

namespace {

struct CachedDetection {
    std::int64_t mtime_ns = 0;
    std::uint64_t size = 0;
    std::size_t schemas = 0;
    SchemaDetection detection;
};

using DetectionMap = dstd::HashMap<dstd::String, CachedDetection>;

struct DetectionCache {
    std::mutex mu;
    dstd::UniquePtr<DetectionMap> map;
};

DetectionCache& detection_cache() {
    // Pool memory alive at exit would fail the memory exit check.
    [[maybe_unused]] static const bool hooked = [] {
        dstd::add_shutdown_hook(dstd::ShutdownStage::FreeCaches, []() noexcept {
            auto& cache = dstd::Singleton<DetectionCache>::get();
            std::lock_guard lock(cache.mu);
            cache.map.reset();
        });
        return true;
    }();
    return dstd::Singleton<DetectionCache>::get();
}

dstd::Optional<SchemaDetection> cached_detection(Registry& registry,
                                                 dstd::StringView file_path,
                                                 std::uint64_t size,
                                                 std::int64_t mtime_ns) {
    const std::size_t schemas = registry.size();
    std::lock_guard lock(detection_cache().mu);
    const auto& map = detection_cache().map;
    if (!map) return dstd::nullopt;
    const auto it = map->find(file_path);
    if (it != map->end() && it->second.mtime_ns == mtime_ns &&
        it->second.size == size && it->second.schemas == schemas)
        return it->second.detection;
    return dstd::nullopt;
}

}  // namespace

dstd::Result<SchemaDetection> explain_file_schema(dstd::CStringView file_path,
                                                  const dstd::File& file,
                                                  std::uint64_t size,
                                                  std::int64_t mtime_ns) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    if (const auto hit = cached_detection(**reg, file_path, size, mtime_ns))
        return *hit;
    Detector d((*reg)->all(), (*reg)->find("generic"));
    if (auto read = for_each_head_line(
            file, file_path,
            [&](dstd::StringView line) { return d.add(line); });
        !read)
        return read.error();
    SchemaDetection out = d.finish();
    {
        std::lock_guard lock(detection_cache().mu);
        auto& map = detection_cache().map;
        if (!map) map = dstd::make_unique<DetectionMap>();
        map->insert_or_assign(
            dstd::String(file_path.view()),
            CachedDetection{mtime_ns, size, (*reg)->size(), out});
    }
    return out;
}

dstd::Result<SchemaDetection> explain_file_schema(dstd::CStringView file_path) {
    auto reg = try_registry();
    if (!reg) return reg.error();
    common::InputFileStat st;
    if (!common::stat_input(file_path, st))
        return common::make_error<"cannot read trace for schema detection: {}">(
            common::ErrorCode::Io, file_path);
    if (const auto hit =
            cached_detection(**reg, file_path, st.size, st.mtime_ns))
        return *hit;
    auto file = dstd::File::open(dstd::PathView(file_path), {});
    if (!file)
        return common::make_error<"cannot read trace for schema detection: {}">(
            common::ErrorCode::Io, file_path);
    return explain_file_schema(file_path, *file, st.size, st.mtime_ns);
}

dstd::String to_json(const SchemaDetection& d) {
    dstd::String out = "{\"chosen\":\"";
    json::append_json_escaped(out, d.chosen ? d.chosen->id : "");
    out += "\",\"objects\":" + dstd::to_string(d.objects) +
           ",\"records\":" + dstd::to_string(d.records) + ",\"scores\":[";
    for (std::size_t i = 0; i < d.scores.size(); ++i) {
        const auto& s = d.scores[i];
        if (i) out += ',';
        out += "{\"id\":\"";
        json::append_json_escaped(out, s.schema->id);
        out += "\",\"required\":" + dstd::to_string(s.schema->require.size()) +
               ",\"share\":" + dstd::to_string(s.share) + '}';
    }
    out += "]}";
    return dstd::String(out);
}

dstd::Result<const RecordSchema*> detect_schema(
    dstd::Span<const dstd::StringView> lines) {
    auto detection = explain_schema(lines);
    if (!detection) return detection.error();
    return detection->chosen;
}

dstd::Result<const RecordSchema*> detect_file_schema(
    dstd::CStringView file_path) {
    auto detection = explain_file_schema(file_path);
    if (!detection) return detection.error();
    return detection->chosen;
}

dstd::Result<const RecordSchema*> detect_file_schema(
    dstd::CStringView file_path, const dstd::File& file, std::uint64_t size,
    std::int64_t mtime_ns) {
    auto detection = explain_file_schema(file_path, file, size, mtime_ns);
    if (!detection) return detection.error();
    return detection->chosen;
}

dstd::Optional<std::int64_t> iso8601_micros(dstd::StringView s) {
    auto digits = [&](std::size_t at, std::size_t n) -> dstd::Optional<int> {
        if (at + n > s.size()) return dstd::nullopt;
        int v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return dstd::nullopt;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    const auto y = digits(0, 4), mo = digits(5, 2), dd = digits(8, 2);
    if (!y || !mo || !dd || s.size() < 19 || s[4] != '-' || s[7] != '-' ||
        (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
        return dstd::nullopt;
    const auto hh = digits(11, 2), mi = digits(14, 2), ss = digits(17, 2);
    if (!hh || !mi || !ss || *mo < 1 || *mo > 12 || *dd < 1 || *dd > 31)
        return dstd::nullopt;
    std::size_t at = 19;
    std::int64_t frac = 0;
    if (at < s.size() && s[at] == '.') {
        std::int64_t scale = 100000;
        for (++at; at < s.size() && dstd::is_ascii_digit(s[at]); ++at) {
            frac += (s[at] - '0') * scale;
            scale /= 10;
        }
    }
    std::int64_t offset = 0;
    if (at < s.size() && (s[at] == 'Z' || s[at] == 'z')) {
        ++at;
    } else if (at < s.size() && (s[at] == '+' || s[at] == '-')) {
        const auto oh = digits(at + 1, 2);
        const bool colon = at + 3 < s.size() && s[at + 3] == ':';
        const auto om = digits(at + (colon ? 4 : 3), 2);
        if (!oh || !om) return dstd::nullopt;
        offset = (s[at] == '-' ? -1 : 1) * (*oh * 3600 + *om * 60);
        at += colon ? 6 : 5;
    }
    if (at != s.size()) return dstd::nullopt;
    const std::int64_t days = common::days_from_civil(*y, *mo, *dd);
    const std::int64_t secs =
        days * 86400 + *hh * 3600 + *mi * 60 + *ss - offset;
    return secs * 1000000 + frac;
}

}  // namespace dftracer::utils::trace::index
