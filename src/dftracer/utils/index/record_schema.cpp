#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/core/env.h>
#include <dftracer/utils/duql/builder.h>
#include <dftracer/utils/duql/query.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/index/source.h>
#include <dftracer/utils/json/json_escape.h>
#include <dftracer/utils/json/json_value.h>
#include <dftracer/utils/json/line.h>
#include <dftracer/utils/json/record_parser.h>
#include <dftracer/utils/utilities/hash/hasher_utility.h>
#include <simdjson.h>
#include <yaml-cpp/yaml.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace dftracer::utils::index {

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
void for_each_head_line(const std::string& path, Emit&& emit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw DFTUtilsException::cat(ErrorCode::IO,
                                     "cannot read trace for schema "
                                     "detection: ",
                                     path);
    std::vector<char> comp(CHUNK);
    in.read(comp.data(), 2);
    const bool gz = in.gcount() == 2 &&
                    static_cast<unsigned char>(comp[0]) == 0x1f &&
                    static_cast<unsigned char>(comp[1]) == 0x8b;
    in.seekg(0);
    std::string text;
    std::size_t consumed = 0;
    bool done = false;
    auto feed = [&](const char* data, std::size_t n) {
        // The text held from before has no newline.
        const std::size_t from = text.size();
        text.append(data, n);
        std::size_t pos = 0;
        for (;;) {
            const auto nl = text.find('\n', std::max(pos, from));
            if (nl == std::string::npos) break;
            consumed += nl + 1 - pos;
            if (!emit(std::string_view(text).substr(pos, nl - pos)) ||
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
        while (!done && in.read(comp.data(), CHUNK).gcount() > 0)
            feed(comp.data(), static_cast<std::size_t>(in.gcount()));
    } else {
        z_stream zs{};
        // Concatenated members decode as one stream (inflateReset at each
        // member end).
        if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
            throw DFTUtilsException::cat(ErrorCode::IO,
                                         "cannot inflate trace: ", path);
        std::vector<char> out(CHUNK);
        bool eof = false;
        while (!done) {
            if (zs.avail_in == 0 && !eof) {
                in.read(comp.data(), CHUNK);
                const auto got = in.gcount();
                eof = got <= 0;
                zs.next_in = reinterpret_cast<Bytef*>(comp.data());
                zs.avail_in = eof ? 0 : static_cast<uInt>(got);
            }
            zs.next_out = reinterpret_cast<Bytef*>(out.data());
            zs.avail_out = static_cast<uInt>(out.size());
            const int rc = inflate(&zs, Z_NO_FLUSH);
            const std::size_t got = out.size() - zs.avail_out;
            feed(out.data(), got);
            if (rc == Z_STREAM_END) {
                if ((eof && zs.avail_in == 0) || inflateReset(&zs) != Z_OK)
                    break;
            } else if ((rc != Z_OK && rc != Z_BUF_ERROR) ||
                       (eof && zs.avail_in == 0 && got == 0)) {
                break;
            }
        }
        inflateEnd(&zs);
    }
    if (!done && !text.empty()) emit(std::string_view(text));
}

// Scores the registered schemas on lines fed one at a time. A schema judges
// only the records its `data` row set does not leave out, so metadata lines
// neither match nor miss it.
class Detector {
   public:
    Detector() : candidates_(registered_schemas()) {
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
                conds_.push_back({s->data, s->args_fallback,
                                  duql::parse_or_throw(s->data), 0});
                same = std::prev(conds_.end());
            }
            cond_of_[i] = static_cast<std::size_t>(same - conds_.begin());
        }
        truth_.resize(conds_.size());
    }

    // Whether more lines are wanted.
    bool add(std::string_view line) {
        const char* start = nullptr;
        std::size_t length = 0;
        if (!json::trim_and_validate_with_comma(line.data(), line.size(), start,
                                                length))
            return true;
        simdjson::dom::element root;
        if (parser_.parse(start, length).get(root) != simdjson::SUCCESS ||
            !root.is_object())
            return true;
        ++out_.objects;
        const json::JsonValue record(root);
        bool metadata = false;
        for (std::size_t k = 0; k < conds_.size(); ++k) {
            truth_[k] = duql::evaluate_truth(conds_[k].query.root(), record,
                                             conds_[k].args_fallback);
            if (truth_[k] == duql::Truth::NO) {
                metadata = true;
                ++conds_[k].left_out;
            }
        }
        out_.records += !metadata;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (cond_of_[i] != NONE && truth_[cond_of_[i]] == duql::Truth::NO)
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
        out_.chosen = best ? best : &get_schema("generic");
        return std::move(out_);
    }

   private:
    static constexpr std::size_t NONE = static_cast<std::size_t>(-1);

    struct Cond {
        std::string text;
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
            [&](const std::string& path) { return record.at(path).exists(); });
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

    std::vector<const RecordSchema*> candidates_;
    std::vector<Cond> conds_;
    std::vector<std::size_t> cond_of_;
    std::vector<duql::Truth> truth_;
    std::vector<std::size_t> judged_;
    std::vector<std::size_t> hits_;
    dftracer::utils::json::RecordParser parser_;
    SchemaDetection out_;
};

}  // namespace

double micros_per(TimeUnit unit) {
    switch (unit) {
        case TimeUnit::NS:
            return 1e-3;
        case TimeUnit::US:
            return 1;
        case TimeUnit::MS:
            return 1e3;
        case TimeUnit::S:
            return 1e6;
    }
    return 1;
}

const FieldSpec* RecordSchema::field_at(std::string_view path) const {
    const auto it = std::lower_bound(
        fields_by_path.begin(), fields_by_path.end(), path,
        [](const auto& e, std::string_view p) { return e.first < p; });
    return it != fields_by_path.end() && it->first == path ? &fields[it->second]
                                                           : nullptr;
}

std::uint64_t RecordSchema::params_hash() const {
    utilities::hash::HasherUtility hasher;
    auto text = [&](std::string_view s) {
        hasher.update(s);
        hasher.update(std::uint8_t{0});
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
        return hasher.get_hash().value;
    }
    hasher.update(std::uint8_t{3});
    text(id);
    hasher.update(static_cast<std::uint8_t>(decoder));
    std::vector<const FieldSpec*> sorted;
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
            static_cast<std::uint8_t>(f->unit.value_or(TimeUnit::US)));
        hasher.update(static_cast<std::uint8_t>(f->always_index));
    }
    text(roles.phase);
    if (!source.empty()) text(source);
    hasher.update(static_cast<std::uint8_t>(path_budget.has_value()));
    hasher.update(path_budget.value_or(0));
    return hasher.get_hash().value;
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
        [](const FieldSpec& f) { return f.type == FieldType::JSON; });
    for (std::size_t i = 0; i < s.fields.size(); ++i)
        s.fields_by_path.emplace_back(s.fields[i].path, i);
    std::sort(s.fields_by_path.begin(), s.fields_by_path.end());
    const std::string phase = s.roles.phase;
    s.roles = Roles{};
    s.roles.phase = phase;
    auto add = [](std::vector<std::string>& to, const std::string& path) {
        if (std::find(to.begin(), to.end(), path) == to.end())
            to.push_back(path);
    };
    for (const auto& f : s.fields) {
        if (!f.optional) add(s.require, f.path);
        if (f.always_index) add(s.always_index, f.path);
        switch (f.role) {
            case Role::NONE:
                break;
            case Role::TIME:
                s.roles.time = f.path;
                s.roles.time_unit = f.unit.value_or(TimeUnit::US);
                if (s.decoder == Decoder::PATH) add(s.always_index, f.path);
                break;
            case Role::DURATION:
                s.roles.duration = f.path;
                s.roles.duration_unit = f.unit.value_or(TimeUnit::US);
                if (s.decoder == Decoder::PATH) add(s.always_index, f.path);
                break;
            case Role::ENTITY:
                s.roles.entity = f.path;
                if (s.decoder == Decoder::PATH) add(s.always_index, f.path);
                break;
            case Role::LANE:
                s.roles.lane = f.path;
                if (s.decoder == Decoder::PATH) add(s.always_index, f.path);
                break;
            case Role::NAME:
                s.roles.name = f.path;
                if (s.decoder == Decoder::PATH) add(s.always_index, f.path);
                break;
        }
    }
}

FieldSpec builtin_field(std::string name, FieldType type, bool optional,
                        Role role) {
    FieldSpec f;
    f.path = name;
    f.name = std::move(name);
    f.type = type;
    f.optional = optional;
    f.role = role;
    if (role == Role::TIME || role == Role::DURATION) f.unit = TimeUnit::US;
    return f;
}

std::vector<RecordSchema> builtins() {
    RecordSchema dft;
    dft.id = "dftracer";
    dft.decoder = Decoder::DFTRACER;
    dft.builtin = true;
    dft.fields = {
        builtin_field("ph", FieldType::STRING, false, Role::NONE),
        builtin_field("name", FieldType::STRING, false, Role::NAME),
        builtin_field("ts", FieldType::INT, false, Role::TIME),
        builtin_field("dur", FieldType::INT, true, Role::DURATION),
        builtin_field("pid", FieldType::INT, true, Role::ENTITY),
        builtin_field("tid", FieldType::INT, true, Role::LANE),
    };
    dft.roles.phase = "ph";
    {
        using namespace duql;
        const Col meta = c("ph").is_in(std::vector<Col>{"M", 4});
        const auto dict = [&](const char* tag) {
            return Pipe().where(meta && c("name") == tag);
        };
        dft.source = merge_source(
            {},
            Source()
                .rowset("data",
                        Pipe().where(c("ph").not_in(std::vector<Col>{"M", 4})))
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
                .flag("args_fallback", true)
                .text(),
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
    genesis.fields = {
        builtin_field("gtype", FieldType::STRING, false, Role::NONE),
        builtin_field("run", FieldType::STRING, false, Role::NONE),
        builtin_field("ts", FieldType::INT, true, Role::TIME),
    };
    {
        using namespace duql;
        genesis.source = merge_source(
            {},
            Source()
                .rowset("data", Pipe().where(c("gtype") != "run"))
                .rowset("runs",
                        Pipe()
                            .where(c("gtype") == "run")
                            .select({"run", "app", "system", "unique_input",
                                     "nodes", "ppn", "papi_set", "method",
                                     "sketch_accuracy", "leaf"})
                            .distinct())
                .text(),
            "genesis");
    }
    genesis.data = data_condition(genesis);
    genesis.args_fallback = source_args_fallback(genesis);
    derive(genesis);
    return {std::move(dft), std::move(generic), std::move(genesis)};
}

struct Entry {
    std::unique_ptr<const RecordSchema> schema;
    std::string source;
};

[[noreturn]] void spec_error(const std::string& source,
                             const std::string& what) {
    throw DFTUtilsException::cat(ErrorCode::INVALID_ARGUMENT,
                                 "record schema spec ", source, ": ", what);
}

class Registry {
   public:
    Registry() {
        for (auto& s : builtins())
            entries_.push_back(
                {std::make_unique<RecordSchema>(std::move(s)), "built-in"});
    }

    const RecordSchema* find(std::string_view id) const {
        std::shared_lock lock(mu_);
        return find_locked(id);
    }

    std::size_t size() const {
        std::shared_lock lock(mu_);
        return entries_.size();
    }

    std::vector<const RecordSchema*> all() const {
        std::shared_lock lock(mu_);
        std::vector<const RecordSchema*> out;
        out.reserve(entries_.size());
        for (const auto& e : entries_) out.push_back(e.schema.get());
        return out;
    }

    const RecordSchema& add(const SchemaSpec& spec, const std::string& source);

    // Whether `path` was loaded before; marks it loaded.
    bool first_load(const std::string& path) {
        std::lock_guard lock(loaded_mu_);
        return loaded_.insert(path).second;
    }
    void forget_load(const std::string& path) {
        std::lock_guard lock(loaded_mu_);
        loaded_.erase(path);
    }

   private:
    const RecordSchema* find_locked(std::string_view id) const {
        const Entry* e = entry_locked(id);
        return e ? e->schema.get() : nullptr;
    }
    const Entry* entry_locked(std::string_view id) const {
        for (const auto& e : entries_)
            if (e.schema->id == id) return &e;
        return nullptr;
    }

    mutable std::shared_mutex mu_;
    std::vector<Entry> entries_;
    std::mutex loaded_mu_;
    std::set<std::string> loaded_;
};

std::string_view role_name(Role role) {
    switch (role) {
        case Role::NONE:
            return "none";
        case Role::TIME:
            return "time";
        case Role::DURATION:
            return "duration";
        case Role::ENTITY:
            return "entity";
        case Role::LANE:
            return "lane";
        case Role::NAME:
            return "name";
    }
    return "none";
}

const RecordSchema& Registry::add(const SchemaSpec& spec,
                                  const std::string& source) {
    if (spec.id.empty()) spec_error(source, "missing id");
    std::unique_lock lock(mu_);
    if (const Entry* e = entry_locked(spec.id); e && e->source == "built-in")
        spec_error(source,
                   "built-in schema " + spec.id + " cannot be redefined");
    if (spec.extends == spec.id)
        spec_error(source, "schema " + spec.id + " extends itself");
    const RecordSchema* parent = find_locked(spec.extends);
    if (!parent) spec_error(source, "unknown extends " + spec.extends);
    RecordSchema s = *parent;
    s.id = spec.id;
    s.builtin = false;
    for (FieldSpec f : spec.fields) {
        if (f.name.empty()) spec_error(source, "a field has no name");
        const std::string where = "fields." + f.name;
        if (f.path.empty()) f.path = f.name;
        const bool timed = f.role == Role::TIME || f.role == Role::DURATION;
        if (f.unit && !timed)
            spec_error(source, where + ".unit needs the time or duration role");
        if (timed) {
            // A string time is ISO-8601 text.
            const bool iso =
                f.role == Role::TIME && f.type == FieldType::STRING;
            if (f.type != FieldType::INT && f.type != FieldType::FLOAT && !iso)
                spec_error(source,
                           where + " has the " +
                               std::string(role_name(f.role)) +
                               (f.role == Role::TIME
                                    ? " role and must be int, float or string"
                                    : " role and must be int or float"));
            if (iso && f.unit)
                spec_error(source, where +
                                       ".unit does not apply to an "
                                       "ISO-8601 string time");
            f.unit = f.unit.value_or(TimeUnit::US);
        }
        if ((f.role == Role::ENTITY || f.role == Role::LANE ||
             f.role == Role::NAME) &&
            f.type != FieldType::INT && f.type != FieldType::STRING)
            spec_error(source, where + " has the " +
                                   std::string(role_name(f.role)) +
                                   " role and must be int or string");
        if (f.type == FieldType::JSON && s.decoder != Decoder::PATH)
            spec_error(source, where +
                                   " is json, which needs a schema that "
                                   "decodes records by path");
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
                spec_error(source, "fields." + a.name + " and fields." +
                                       b.name + " share the path " + a.path);
            if (a.role != Role::NONE && a.role == b.role)
                spec_error(source, "fields." + a.name + " and fields." +
                                       b.name + " both have the " +
                                       std::string(role_name(a.role)) +
                                       " role");
        }
    if (spec.path_budget) s.path_budget = spec.path_budget;
    s.source =
        merge_source(s.source, spec.source, source + ", schema " + spec.id);
    s.data = data_condition(s);
    s.args_fallback = source_args_fallback(s);
    derive(s);
    check_source(s, source);

    if (const Entry* e = entry_locked(s.id)) {
        if (e->schema->params_hash() == s.params_hash()) return *e->schema;
        spec_error(source, "schema " + s.id + " is already registered from " +
                               e->source + " with another definition");
    }
    entries_.push_back({std::make_unique<RecordSchema>(std::move(s)), source});
    return *entries_.back().schema;
}

void check_keys(const YAML::Node& map, std::initializer_list<const char*> keys,
                const std::string& where, const std::string& source) {
    if (!map.IsMap()) spec_error(source, where + " must be a mapping");
    for (const auto& kv : map) {
        const auto key = kv.first.as<std::string>();
        if (std::none_of(keys.begin(), keys.end(),
                         [&](const char* k) { return key == k; }))
            spec_error(source, "unknown key " + where + key);
    }
}

std::string text_of(const YAML::Node& n, const std::string& key,
                    const std::string& source) {
    if (!n.IsScalar()) spec_error(source, key + " must be a string");
    return n.as<std::string>();
}

bool flag_of(const YAML::Node& n, const std::string& key,
             const std::string& source) {
    try {
        return n.as<bool>();
    } catch (const YAML::Exception&) {
        spec_error(source, key + " must be true or false");
    }
}

template <typename E, std::size_t N>
E choice_of(const YAML::Node& n, const std::string& key,
            const std::array<std::pair<const char*, E>, N>& names,
            const std::string& source) {
    const std::string v = text_of(n, key, source);
    for (const auto& [name, value] : names)
        if (v == name) return value;
    std::string all;
    for (const auto& [name, value] : names) {
        if (!all.empty()) all += ", ";
        all += name;
    }
    spec_error(source, key + " is " + v + "; expected one of " + all);
}

constexpr std::array<std::pair<const char*, FieldType>, 5> TYPE_NAMES = {{
    {"bool", FieldType::BOOL},
    {"int", FieldType::INT},
    {"float", FieldType::FLOAT},
    {"string", FieldType::STRING},
    {"json", FieldType::JSON},
}};
constexpr std::array<std::pair<const char*, Role>, 5> ROLE_NAMES = {{
    {"time", Role::TIME},
    {"duration", Role::DURATION},
    {"entity", Role::ENTITY},
    {"lane", Role::LANE},
    {"name", Role::NAME},
}};
constexpr std::array<std::pair<const char*, TimeUnit>, 4> UNIT_NAMES = {{
    {"ns", TimeUnit::NS},
    {"us", TimeUnit::US},
    {"ms", TimeUnit::MS},
    {"s", TimeUnit::S},
}};

SchemaSpec parse_spec(const YAML::Node& spec, const std::string& source) {
    check_keys(spec, {"id", "extends", "fields", "index", "source"}, "",
               source);
    SchemaSpec out;
    if (!spec["id"]) spec_error(source, "missing id");
    out.id = text_of(spec["id"], "id", source);
    if (spec["extends"])
        out.extends = text_of(spec["extends"], "extends", source);
    if (const auto fs = spec["fields"]) {
        if (!fs.IsMap())
            spec_error(source, "fields must map field names to settings");
        for (const auto& kv : fs) {
            FieldSpec f;
            f.name = text_of(kv.first, "fields", source);
            const std::string where = "fields." + f.name;
            const YAML::Node n = kv.second;
            check_keys(
                n, {"type", "path", "optional", "role", "unit", "always_index"},
                where + ".", source);
            if (!n["type"]) spec_error(source, where + ".type is required");
            f.type = choice_of(n["type"], where + ".type", TYPE_NAMES, source);
            if (n["path"]) f.path = text_of(n["path"], where + ".path", source);
            if (n["optional"])
                f.optional =
                    flag_of(n["optional"], where + ".optional", source);
            if (n["role"])
                f.role =
                    choice_of(n["role"], where + ".role", ROLE_NAMES, source);
            if (n["unit"])
                f.unit =
                    choice_of(n["unit"], where + ".unit", UNIT_NAMES, source);
            if (n["always_index"])
                f.always_index =
                    flag_of(n["always_index"], where + ".always_index", source);
            out.fields.push_back(std::move(f));
        }
    }
    if (const auto ix = spec["index"]) {
        check_keys(ix, {"path_budget"}, "index.", source);
        if (ix["path_budget"]) {
            try {
                out.path_budget = ix["path_budget"].as<std::size_t>();
            } catch (const YAML::Exception&) {
                spec_error(source, "index.path_budget must be a count");
            }
        }
    }
    if (spec["source"]) out.source = text_of(spec["source"], "source", source);
    return out;
}

bool spec_file(const fs::path& p) {
    const auto ext = p.extension().string();
    return ext == ".yaml" || ext == ".yml" || ext == ".json";
}

const RecordSchema& register_into(Registry& reg, std::string_view text,
                                  const std::string& source) {
    YAML::Node spec;
    try {
        spec = YAML::Load(std::string(text));
    } catch (const YAML::Exception& e) {
        spec_error(source, e.what());
    }
    return reg.add(parse_spec(spec, source), source);
}

void load_into(Registry& reg, const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::path(path), ec);
    const std::string key = ec ? path : p.string();
    if (!reg.first_load(key)) return;
    try {
        std::vector<fs::path> files;
        if (fs::is_directory(p)) {
            for (const auto& e : fs::directory_iterator(p))
                if (e.is_regular_file() && spec_file(e.path()))
                    files.push_back(e.path());
            std::sort(files.begin(), files.end());
        } else {
            files.push_back(p);
        }
        for (const auto& f : files) {
            std::ifstream in(f, std::ios::binary);
            if (!in)
                throw DFTUtilsException::cat(ErrorCode::IO,
                                             "cannot read record schema spec ",
                                             f.string());
            std::stringstream text;
            text << in.rdbuf();
            register_into(reg, text.str(), f.string());
        }
    } catch (...) {
        reg.forget_load(key);
        throw;
    }
}

// The registry, with $DFTRACER_SCHEMA_PATH loaded on first use; a failed
// load throws here and is retried by the next access.
Registry& registry() {
    static Registry r;
    static std::once_flag env;
    std::call_once(env, [] {
        if (const auto paths = Env::get("DFTRACER_SCHEMA_PATH")) {
            std::stringstream ss{std::string(*paths)};
            for (std::string p; std::getline(ss, p, ':');)
                if (!p.empty()) load_into(r, p);
        }
    });
    return r;
}

template <typename E, std::size_t N>
const char* name_of(E value,
                    const std::array<std::pair<const char*, E>, N>& names) {
    for (const auto& [name, v] : names)
        if (v == value) return name;
    return "";
}

}  // namespace

std::string_view field_type_name(FieldType type) {
    return name_of(type, TYPE_NAMES);
}

std::vector<const RecordSchema*> registered_schemas() {
    return registry().all();
}

const RecordSchema* find_schema(std::string_view id) {
    return registry().find(id);
}

const RecordSchema& get_schema(std::string_view id) {
    if (const RecordSchema* s = find_schema(id)) return *s;
    std::string ids;
    for (const RecordSchema* s : registered_schemas()) {
        if (!ids.empty()) ids += ", ";
        ids += s->id;
    }
    throw DFTUtilsException::cat(
        ErrorCode::INVALID_ARGUMENT, "unknown record schema ", std::string(id),
        " (registered: ", ids,
        "); user schemas load from $DFTRACER_SCHEMA_PATH and "
        "<index_dir>/schemas/");
}

const RecordSchema& register_schema(const SchemaSpec& spec,
                                    std::string_view source) {
    return registry().add(spec, std::string(source));
}

const RecordSchema& register_schema(std::string_view text,
                                    std::string_view source) {
    return register_into(registry(), text, std::string(source));
}

void load_schemas(const std::string& path) { load_into(registry(), path); }

std::string schemas_json() {
    std::string out = "[";
    auto quoted = [&](std::string_view s) {
        out += '"';
        json::append_json_escaped(out, s);
        out += '"';
    };
    auto list = [&](const std::vector<std::string>& xs) {
        out += '[';
        for (std::size_t i = 0; i < xs.size(); ++i) {
            if (i) out += ',';
            quoted(xs[i]);
        }
        out += ']';
    };
    bool first = true;
    for (const RecordSchema* s : registered_schemas()) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":";
        quoted(s->id);
        out += ",\"decoder\":";
        quoted(s->decoder == Decoder::DFTRACER ? "dftracer" : "path");
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
            if (f.role == Role::NONE)
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
        out += s->path_budget ? std::to_string(*s->path_budget) : "null";
        out += ",\"source\":";
        quoted(s->source);
        out += '}';
    }
    out += ']';
    return out;
}

void load_index_schemas(const std::string& index_path) {
    const fs::path dir = fs::path(index_path).parent_path() / "schemas";
    if (fs::is_directory(dir)) load_schemas(dir.string());
}

SchemaDetection explain_schema(std::span<const std::string_view> lines) {
    Detector d;
    for (auto line : lines) d.add(line);
    return d.finish();
}

SchemaDetection explain_file_schema(const std::string& file_path) {
    struct Cached {
        fs::file_time_type mtime;
        std::uintmax_t size = 0;
        std::size_t schemas = 0;
        SchemaDetection detection;
    };
    static std::mutex mu;
    static std::unordered_map<std::string, Cached> cache;
    std::error_code ec;
    const auto mtime = fs::last_write_time(file_path, ec);
    const auto size = ec ? 0 : fs::file_size(file_path, ec);
    const std::size_t schemas = registry().size();
    if (!ec) {
        std::lock_guard lock(mu);
        const auto it = cache.find(file_path);
        if (it != cache.end() && it->second.mtime == mtime &&
            it->second.size == size && it->second.schemas == schemas)
            return it->second.detection;
    }
    Detector d;
    for_each_head_line(file_path,
                       [&](std::string_view line) { return d.add(line); });
    SchemaDetection out = d.finish();
    if (!ec) {
        std::lock_guard lock(mu);
        cache.insert_or_assign(file_path, Cached{mtime, size, schemas, out});
    }
    return out;
}

std::string to_json(const SchemaDetection& d) {
    std::string out = "{\"chosen\":\"";
    json::append_json_escaped(out, d.chosen ? d.chosen->id : "");
    out += "\",\"objects\":" + std::to_string(d.objects) +
           ",\"records\":" + std::to_string(d.records) + ",\"scores\":[";
    for (std::size_t i = 0; i < d.scores.size(); ++i) {
        const auto& s = d.scores[i];
        if (i) out += ',';
        out += "{\"id\":\"";
        json::append_json_escaped(out, s.schema->id);
        out += "\",\"required\":" + std::to_string(s.schema->require.size()) +
               ",\"share\":" + std::to_string(s.share) + '}';
    }
    out += "]}";
    return out;
}

const RecordSchema& detect_schema(std::span<const std::string_view> lines) {
    return *explain_schema(lines).chosen;
}

const RecordSchema& detect_file_schema(const std::string& file_path) {
    return *explain_file_schema(file_path).chosen;
}

std::optional<std::int64_t> iso8601_micros(std::string_view s) {
    auto digits = [&](std::size_t at, std::size_t n) -> std::optional<int> {
        if (at + n > s.size()) return std::nullopt;
        int v = 0;
        for (std::size_t i = at; i < at + n; ++i) {
            if (s[i] < '0' || s[i] > '9') return std::nullopt;
            v = v * 10 + (s[i] - '0');
        }
        return v;
    };
    const auto y = digits(0, 4), mo = digits(5, 2), dd = digits(8, 2);
    if (!y || !mo || !dd || s.size() < 19 || s[4] != '-' || s[7] != '-' ||
        (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
        return std::nullopt;
    const auto hh = digits(11, 2), mi = digits(14, 2), ss = digits(17, 2);
    if (!hh || !mi || !ss || *mo < 1 || *mo > 12 || *dd < 1 || *dd > 31)
        return std::nullopt;
    std::size_t at = 19;
    std::int64_t frac = 0;
    if (at < s.size() && s[at] == '.') {
        std::int64_t scale = 100000;
        for (++at; at < s.size() && s[at] >= '0' && s[at] <= '9'; ++at) {
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
        if (!oh || !om) return std::nullopt;
        offset = (s[at] == '-' ? -1 : 1) * (*oh * 3600 + *om * 60);
        at += colon ? 6 : 5;
    }
    if (at != s.size()) return std::nullopt;
    // Days from 1970-01-01 of a civil date (Howard Hinnant, public domain).
    const int yy = *y - (*mo <= 2);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy =
        (153 * static_cast<unsigned>(*mo + (*mo > 2 ? -3 : 9)) + 2) / 5 +
        static_cast<unsigned>(*dd) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days =
        static_cast<std::int64_t>(era) * 146097 + doe - 719468;
    const std::int64_t secs =
        days * 86400 + *hh * 3600 + *mi * 60 + *ss - offset;
    return secs * 1000000 + frac;
}

}  // namespace dftracer::utils::index
