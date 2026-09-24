#include <dftracer/utils/core/common/error.h>
#include <dftracer/utils/core/common/filesystem.h>
#include <dftracer/utils/index/record_schema.h>
#include <dftracer/utils/json/json_escape.h>
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

namespace dftracer::utils::index {

namespace {

bool has_path(simdjson::dom::element root, std::string_view path) {
    simdjson::dom::element cur = root;
    while (!path.empty()) {
        const auto dot = path.find('.');
        const auto key = path.substr(0, dot);
        simdjson::dom::object obj;
        if (cur.get_object().get(obj) != simdjson::SUCCESS) return false;
        if (obj.at_key(key).get(cur) != simdjson::SUCCESS) return false;
        path = dot == std::string_view::npos ? std::string_view{}
                                             : path.substr(dot + 1);
    }
    return true;
}

// Up to `max_lines` lines from the start of a gzip or plain file.
std::vector<std::string> head_lines(const std::string& path,
                                    std::size_t max_lines) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw DFTUtilsException::cat(ErrorCode::IO,
                                     "cannot read trace for schema "
                                     "detection: ",
                                     path);
    constexpr std::size_t CHUNK = 64 * 1024;
    constexpr std::size_t MAX_TEXT = 16u * 1024 * 1024;
    std::string text;
    std::vector<char> comp(CHUNK);
    in.read(comp.data(), 2);
    const bool gz = in.gcount() == 2 &&
                    static_cast<unsigned char>(comp[0]) == 0x1f &&
                    static_cast<unsigned char>(comp[1]) == 0x8b;
    in.seekg(0);
    std::size_t newlines = 0;
    auto append = [&](const char* data, std::size_t n) {
        text.append(data, n);
        for (std::size_t i = 0; i < n; ++i) newlines += data[i] == '\n';
    };
    auto enough = [&] {
        return newlines >= max_lines || text.size() >= MAX_TEXT;
    };
    if (!gz) {
        while (!enough() && in.read(comp.data(), CHUNK).gcount() > 0)
            append(comp.data(), static_cast<std::size_t>(in.gcount()));
    } else {
        z_stream zs{};
        // Concatenated members decode as one stream (inflateReset at each
        // member end).
        if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
            throw DFTUtilsException::cat(ErrorCode::IO,
                                         "cannot inflate trace: ", path);
        std::vector<char> out(CHUNK);
        int rc = Z_OK;
        while (!enough()) {
            if (zs.avail_in == 0) {
                in.read(comp.data(), CHUNK);
                const auto got = in.gcount();
                if (got <= 0) break;
                zs.next_in = reinterpret_cast<Bytef*>(comp.data());
                zs.avail_in = static_cast<uInt>(got);
            }
            zs.next_out = reinterpret_cast<Bytef*>(out.data());
            zs.avail_out = static_cast<uInt>(out.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            append(out.data(), out.size() - zs.avail_out);
            if (rc == Z_STREAM_END) {
                if (inflateReset(&zs) != Z_OK) break;
            } else if (rc != Z_OK && rc != Z_BUF_ERROR) {
                break;
            }
        }
        inflateEnd(&zs);
    }
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < text.size() && lines.size() < max_lines) {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) break;
        lines.emplace_back(text, pos, nl - pos);
        pos = nl + 1;
    }
    return lines;
}

}  // namespace

bool Dictionary::has_field(std::string_view field) const {
    for (const auto& f : fields)
        if (f.first == field) return true;
    return false;
}

const Dictionary* RecordSchema::dictionary_of(
    std::string_view key_field) const {
    for (const auto& d : dictionaries)
        for (const auto& k : d.keys_in)
            if (k == key_field) return &d;
    return nullptr;
}

std::vector<std::string> RecordSchema::resolved_names() const {
    std::vector<std::string> out;
    for (const auto& d : dictionaries)
        for (const auto& k : d.keys_in)
            for (const auto& [field, path] : d.fields)
                out.push_back(std::string(RESOLVED_PREFIX) + k + "." + field);
    return out;
}

ResolvedColumn RecordSchema::resolved_column(std::string_view name) const {
    if (name.starts_with(RESOLVED_PREFIX)) {
        const std::string_view rest = name.substr(RESOLVED_PREFIX.size());
        const std::size_t dot = rest.find('.');
        if (dot != std::string_view::npos) {
            const std::string_view key = rest.substr(0, dot);
            const std::string_view field = rest.substr(dot + 1);
            const Dictionary* d = dictionary_of(key);
            if (d && d->has_field(field))
                return {std::string(key), d, std::string(field)};
        }
    }
    std::string names;
    for (const auto& n : resolved_names()) {
        if (!names.empty()) names += ", ";
        names += n;
    }
    throw DFTUtilsException::cat(
        ErrorCode::INVALID_ARGUMENT, "unknown resolved column ",
        std::string(name), " for schema ", id,
        "; available: ", names.empty() ? std::string("none") : names);
}

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

namespace {

void hash_dictionaries(const std::vector<Dictionary>& dictionaries,
                       auto&& text) {
    for (const auto& d : dictionaries) {
        text(d.name);
        text(d.rows);
        text(d.key);
        for (const auto& [name, path] : d.fields) {
            text(name);
            text(path);
        }
        for (const auto& k : d.keys_in) text(k);
    }
}

}  // namespace

std::uint64_t RecordSchema::params_hash() const {
    utilities::hash::HasherUtility hasher;
    auto text = [&](std::string_view s) {
        hasher.update(s);
        hasher.update(std::uint8_t{0});
    };
    if (builtin) {
        // The layout the built-ins' indexes were recorded with.
        text(id);
        for (const auto& r : require) text(r);
        hasher.update(std::uint8_t{1});
        for (const auto* r :
             {&roles.time, &roles.duration, &roles.entity, &roles.phase})
            text(*r);
        hasher.update(static_cast<std::uint8_t>(decoder));
        hash_dictionaries(dictionaries, text);
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
    hash_dictionaries(dictionaries, text);
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
                break;
            case Role::ENTITY:
                s.roles.entity = f.path;
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
        builtin_field("name", FieldType::STRING, false, Role::NONE),
        builtin_field("ts", FieldType::INT, true, Role::TIME),
        builtin_field("dur", FieldType::INT, true, Role::DURATION),
        builtin_field("pid", FieldType::INT, true, Role::ENTITY),
    };
    dft.roles.phase = "ph";
    dft.dictionaries = {
        {"file", "FH", "args.value", {{"path", "args.name"}}, {"fhash", "cwd"}},
        {"host", "HH", "args.value", {{"name", "args.name"}}, {"hhash"}},
        {"string",
         "SH",
         "args.value",
         {{"value", "args.name"}},
         {"exec_hash", "cmd_hash"}}};
    derive(dft);

    RecordSchema generic;
    generic.id = "generic";
    generic.builtin = true;
    derive(generic);

    // dftracer_genesis_gen_dist output: call-path records keyed by run, and
    // one RUN metadata line per run holding its keys.
    RecordSchema genesis = dft;
    genesis.id = "genesis";
    for (auto [name, type] : {std::pair{"run", FieldType::STRING},
                              std::pair{"path", FieldType::STRING},
                              std::pair{"depth", FieldType::INT},
                              std::pair{"count", FieldType::INT}}) {
        FieldSpec f = builtin_field(name, type, false, Role::NONE);
        f.path = std::string("args.") + name;
        genesis.fields.push_back(std::move(f));
    }
    Dictionary run{"run", "RUN", "args.run", {}, {"run"}};
    for (const char* key : {"app", "system", "unique_input", "nodes", "ppn",
                            "papi_set", "method", "sketch_accuracy", "leaf"})
        run.fields.emplace_back(key, std::string("args.") + key);
    genesis.dictionaries.push_back(std::move(run));
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
            if (f.type != FieldType::INT && f.type != FieldType::FLOAT)
                spec_error(source, where + " has the " +
                                       std::string(role_name(f.role)) +
                                       " role and must be int or float");
            f.unit = f.unit.value_or(TimeUnit::US);
        }
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
    for (const auto& d : spec.dictionaries) {
        auto same =
            std::find_if(s.dictionaries.begin(), s.dictionaries.end(),
                         [&](const Dictionary& x) { return x.name == d.name; });
        if (same != s.dictionaries.end())
            *same = d;
        else
            s.dictionaries.push_back(d);
    }
    derive(s);

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

std::vector<std::string> list_of(const YAML::Node& n, const std::string& key,
                                 const std::string& source) {
    if (!n.IsSequence()) spec_error(source, key + " must be a list of strings");
    std::vector<std::string> out;
    for (const auto& e : n) out.push_back(text_of(e, key, source));
    return out;
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
constexpr std::array<std::pair<const char*, Role>, 3> ROLE_NAMES = {{
    {"time", Role::TIME},
    {"duration", Role::DURATION},
    {"entity", Role::ENTITY},
}};
constexpr std::array<std::pair<const char*, TimeUnit>, 4> UNIT_NAMES = {{
    {"ns", TimeUnit::NS},
    {"us", TimeUnit::US},
    {"ms", TimeUnit::MS},
    {"s", TimeUnit::S},
}};

SchemaSpec parse_spec(const YAML::Node& spec, const std::string& source) {
    check_keys(spec, {"id", "extends", "fields", "index", "dictionaries"}, "",
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
    if (const auto ds = spec["dictionaries"]) {
        if (!ds.IsSequence()) spec_error(source, "dictionaries must be a list");
        for (const auto& dn : ds) {
            check_keys(dn, {"name", "rows", "key", "fields", "keys_in"},
                       "dictionaries.", source);
            Dictionary d;
            for (auto [key, slot] :
                 {std::pair{"name", &d.name}, std::pair{"rows", &d.rows},
                  std::pair{"key", &d.key}}) {
                if (!dn[key])
                    spec_error(source, std::string("dictionaries.") + key +
                                           " is required");
                *slot = text_of(dn[key], std::string("dictionaries.") + key,
                                source);
            }
            if (!dn["fields"] || !dn["fields"].IsMap())
                spec_error(source,
                           "dictionaries.fields must map field names to paths");
            for (const auto& f : dn["fields"])
                d.fields.emplace_back(
                    text_of(f.first, "dictionaries.fields", source),
                    text_of(f.second, "dictionaries.fields", source));
            if (dn["keys_in"])
                d.keys_in =
                    list_of(dn["keys_in"], "dictionaries.keys_in", source);
            out.dictionaries.push_back(std::move(d));
        }
    }
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
        if (const char* paths = std::getenv("DFTRACER_SCHEMA_PATH")) {
            std::stringstream ss(paths);
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
        out += ",\"dictionaries\":";
        std::vector<std::string> names;
        for (const auto& d : s->dictionaries) names.push_back(d.name);
        list(names);
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
    const auto candidates = registered_schemas();
    simdjson::dom::parser parser;
    std::vector<std::size_t> hits(candidates.size(), 0);
    SchemaDetection out;
    for (auto line : lines) {
        simdjson::dom::element root;
        if (parser.parse(simdjson::padded_string(line)).get(root) !=
                simdjson::SUCCESS ||
            !root.is_object())
            continue;
        ++out.objects;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            bool all = true;
            for (const auto& path : candidates[i]->require)
                if (!has_path(root, path)) {
                    all = false;
                    break;
                }
            hits[i] += all;
        }
    }
    const RecordSchema* best = &get_schema("generic");
    std::size_t best_hits = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const RecordSchema* c = candidates[i];
        out.scores.push_back({c, out.objects
                                     ? static_cast<double>(hits[i]) /
                                           static_cast<double>(out.objects)
                                     : 0.0});
        if (out.objects == 0 || c->require.empty()) continue;
        if (static_cast<double>(hits[i]) <
            SCHEMA_MATCH_SHARE * static_cast<double>(out.objects))
            continue;
        const bool better = c->require.size() != best->require.size()
                                ? c->require.size() > best->require.size()
                                : (hits[i] != best_hits ? hits[i] > best_hits
                                                        : c->id < best->id);
        if (better) {
            best = c;
            best_hits = hits[i];
        }
    }
    out.chosen = best;
    return out;
}

SchemaDetection explain_file_schema(const std::string& file_path) {
    const auto lines = head_lines(file_path, SCHEMA_SAMPLE_LINES);
    std::vector<std::string_view> views(lines.begin(), lines.end());
    return explain_schema(views);
}

std::string to_json(const SchemaDetection& d) {
    std::string out = "{\"chosen\":\"";
    json::append_json_escaped(out, d.chosen ? d.chosen->id : "");
    out += "\",\"objects\":" + std::to_string(d.objects) + ",\"scores\":[";
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

}  // namespace dftracer::utils::index
