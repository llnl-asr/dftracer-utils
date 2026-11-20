// SPDX-License-Identifier: MIT
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/hash_map.h>
#include <dftracer/utils/dstd/path.h>
#include <dftracer/utils/runtime/fs/fs.h>
#include <dftracer/utils/runtime/fs/inputs.h>

#include <algorithm>
#include <utility>

namespace dftracer::utils::runtime::fs {

namespace {

using FileKind = dstd::fs::FileKind;
using Failure = dstd::Unexpected<dstd::Error>;
using Done = dstd::Expected<void, dstd::Error>;

constexpr const char* ANY_DEPTH = "**";

// One file an input names. A file of a path the user named has its stat
// already; the others get one in a batch.
struct Candidate {
    dstd::String path;
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    bool known = false;
    bool missing = false;
};

dstd::String join(dstd::StringView dir, dstd::StringView name) {
    if (dir.empty()) return dstd::String(name);
    if (dir.back() == '/') return dstd::fmt<"{}{}">(dir, name);
    return dstd::fmt<"{}/{}">(dir, name);
}

bool is_staging(dstd::StringView name) {
    return dstd::fs::match_name(".*.tmp-*", name) ||
           dstd::fs::match_name(".*.old-*", name);
}

bool is_skipped(dstd::StringView name) {
    for (const char* pattern : common::constants::indexer::SCAN_SKIP_DIRS)
        if (dstd::fs::match_name(pattern, name)) return true;
    return false;
}

FindOptions scan_options(bool every_file) {
    FindOptions options;
    if (!every_file)
        options.patterns =
            names_of(common::constants::indexer::INPUT_FILE_PATTERNS);
    options.skip_dirs = names_of(common::constants::indexer::SCAN_SKIP_DIRS);
    options.with_stat = false;
    options.skip_unreadable = true;
    return options;
}

// The files below a directory, sorted by path, appended to `into`.
coro::CoroTask<Done> scan_directory(coro::Scope& scope, dstd::String dir,
                                    bool every_file,
                                    dstd::Vector<dstd::String>& into) {
    auto scanning =
        find_all(scope, dstd::PathView(dir), scan_options(every_file));
    auto found = co_await std::move(scanning);
    if (!found) co_return Failure(found.error());
    for (auto& e : *found) {
        if (is_staging(e.path.view().filename())) continue;
        into.emplace_back(e.path.str());
    }
    co_return Done();
}

enum class Kind { Missing, Directory, File };

Kind kind_of(dstd::StringView path, FileKind reported) {
    if (reported == FileKind::Directory) return Kind::Directory;
    if (reported == FileKind::File) return Kind::File;
    if (reported == FileKind::Other) return Kind::Missing;
    const dstd::PathView view(path);
    const auto dir = dstd::fs::is_directory(view);
    if (dir.has_value() && *dir) return Kind::Directory;
    const auto file = dstd::fs::is_regular_file(view);
    return file.has_value() && *file ? Kind::File : Kind::Missing;
}

struct Step {
    dstd::String dir;
    std::size_t at = 0;
};

// The files of every path that matches `pattern`, sorted by path.
coro::CoroTask<dstd::Expected<dstd::Vector<dstd::String>, dstd::Error>>
expand_glob(coro::Scope& scope, dstd::String pattern) {
    dstd::Vector<dstd::String> parts;
    {
        dstd::StringView rest(pattern);
        while (!rest.empty()) {
            const std::size_t cut = rest.find('/');
            const dstd::StringView head = rest.substr(0, cut);
            if (!head.empty()) parts.emplace_back(head);
            rest.remove_prefix(cut == dstd::StringView::npos ? rest.size()
                                                             : cut + 1);
        }
    }
    const bool rooted = !pattern.empty() && pattern.front() == '/';
    std::size_t literal = 0;
    while (literal < parts.size() && !dstd::fs::has_glob_chars(parts[literal]))
        ++literal;

    dstd::String root = rooted ? dstd::String("/") : dstd::String();
    for (std::size_t i = 0; i < literal; ++i) root = join(root, parts[i]);

    dstd::Vector<dstd::String> matched;
    dstd::Vector<Step> work;
    work.push_back(Step{std::move(root), literal});
    while (!work.empty()) {
        Step step = std::move(work.back());
        work.pop_back();
        const dstd::StringView part = parts[step.at];
        const bool last = step.at + 1 == parts.size();
        const bool every_depth = part == ANY_DEPTH;
        const dstd::String where =
            step.dir.empty() ? dstd::String(".") : step.dir;

        if (every_depth && last) {
            if (kind_of(where, FileKind::Unknown) == Kind::Directory) {
                auto scanned = scan_directory(scope, where, true, matched);
                auto done = co_await std::move(scanned);
                if (!done) co_return Failure(done.error());
            }
            continue;
        }
        if (every_depth) work.push_back(Step{step.dir, step.at + 1});

        if (!every_depth && !dstd::fs::has_glob_chars(part)) {
            const dstd::String child = join(step.dir, part);
            if (!last) {
                work.push_back(Step{child, step.at + 1});
                continue;
            }
            const Kind kind = kind_of(child, FileKind::Unknown);
            if (kind == Kind::Directory) {
                auto scanned = scan_directory(scope, child, false, matched);
                auto done = co_await std::move(scanned);
                if (!done) co_return Failure(done.error());
            } else if (kind == Kind::File) {
                matched.push_back(child);
            }
            continue;
        }

        auto listing = list_dir(dstd::PathView(where));
        auto entries = co_await std::move(listing);
        // An unreadable or missing directory is left out, as in a scan.
        if (!entries) continue;
        for (const Entry& entry : *entries) {
            const dstd::StringView name = entry.path.view().filename();
            if (is_staging(name) || dstd::fs::match_name(".dftindex*", name))
                continue;
            const dstd::String child = join(step.dir, name);
            if (every_depth) {
                if (entry.kind == FileKind::Directory && !is_skipped(name))
                    work.push_back(Step{child, step.at});
                continue;
            }
            if (!dstd::fs::glob_match_name(part, name)) continue;
            const Kind kind = kind_of(child, entry.kind);
            if (!last) {
                if (kind == Kind::Directory)
                    work.push_back(Step{child, step.at + 1});
            } else if (kind == Kind::Directory) {
                auto scanned = scan_directory(scope, child, false, matched);
                auto done = co_await std::move(scanned);
                if (!done) co_return Failure(done.error());
            } else if (kind == Kind::File) {
                matched.push_back(child);
            }
        }
    }
    std::sort(matched.begin(), matched.end());
    matched.erase(std::unique(matched.begin(), matched.end()), matched.end());
    co_return matched;
}

}  // namespace

coro::CoroTask<dstd::Expected<ExpandedInputs, dstd::Error>> expand_inputs(
    coro::Scope& scope, dstd::Span<const dstd::String> inputs) {
    dstd::Vector<Candidate> candidates;
    for (const dstd::String& input : inputs) {
        const auto stat = dstd::fs::stat(dstd::PathView(input));
        const bool found = stat.has_value();
        const bool wildcard = dstd::fs::has_glob_chars(input);
        if (found ||
            (!wildcard && !stat.error().is(dstd::Condition::NotFound))) {
            if (found && stat->kind == FileKind::Directory) {
                dstd::Vector<dstd::String> files;
                auto scanned = scan_directory(scope, input, false, files);
                auto done = co_await std::move(scanned);
                if (!done) co_return Failure(done.error());
                for (auto& path : files)
                    candidates.push_back(Candidate{std::move(path)});
            } else {
                Candidate one{input};
                if (found) {
                    one.dev = stat->dev;
                    one.ino = stat->ino;
                    one.known = true;
                }
                candidates.push_back(std::move(one));
            }
            continue;
        }
        if (!wildcard) {
            Candidate gone{input};
            gone.missing = true;
            candidates.push_back(std::move(gone));
            continue;
        }
        auto expanding = expand_glob(scope, input);
        auto matched = co_await std::move(expanding);
        if (!matched) co_return Failure(matched.error());
        for (auto& path : *matched)
            candidates.push_back(Candidate{std::move(path)});
    }

    ExpandedInputs out;
    out.files.reserve(candidates.size());
    // One input names no file twice, except through links, which are read
    // as many times as they are named; only several inputs are checked.
    const bool check_overlap = inputs.size() > 1;
    if (check_overlap) {
        dstd::Vector<dstd::Path> unknown;
        for (const Candidate& c : candidates)
            if (!c.known && !c.missing) unknown.emplace_back(c.path);
        auto statting = stat_many(
            dstd::Span<const dstd::Path>(unknown.data(), unknown.size()));
        auto stats = co_await std::move(statting);
        std::size_t next = 0;
        for (Candidate& c : candidates) {
            if (c.known || c.missing) continue;
            const auto& st = stats[next++];
            if (st) {
                c.dev = st->dev;
                c.ino = st->ino;
                c.known = true;
            }
        }
    }
    dstd::HashSet<dstd::String> seen;
    for (Candidate& c : candidates) {
        if (c.missing) {
            if (seen.insert(c.path).second) {
                out.missing.push_back(c.path);
                out.files.push_back(std::move(c.path));
            }
            continue;
        }
        if (check_overlap) {
            dstd::String key =
                c.known ? dstd::fmt<"{}:{}">(c.dev, c.ino) : c.path;
            if (!seen.insert(std::move(key)).second) continue;
        }
        out.files.push_back(std::move(c.path));
    }
    co_return out;
}

}  // namespace dftracer::utils::runtime::fs
