// SPDX-License-Identifier: MIT
// dftracer_plugin: make a plugin project and check a built one. A check runs
// in a child process of this program, so a plugin that crashes or leaks takes
// only the child down and this tool still reports what happened.
#include <dftracer/utils/cli/cli_args.h>
#include <dftracer/utils/cli/common_cli.h>
#include <dftracer/utils/cli/plugin_golden.h>
#include <dftracer/utils/cli/plugin_info.h>
#include <dftracer/utils/cli/plugin_templates.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/error.h>
#include <dftracer/utils/common/logging.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/memory/detail/constants.h>
#include <dftracer/utils/plugins/host/plugins.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

using namespace dftracer::utils;

namespace {

const char* const KIND_NAMES[] = {"fold",   "udf",  "aggregate",
                                  "format", "sink", "node"};
constexpr const char* CHECK_CHILD_FLAG = "--check-child";

dstd::StringView text_of(const cli::PluginTemplate& file) {
    return dstd::StringView(file.data, file.size);
}

/// A short name a C compiler accepts and a plugin's file names accept too.
bool is_c_identifier(dstd::StringView name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool is_digit = c >= '0' && c <= '9';
        if (i == 0 ? !(is_alpha || c == '_')
                   : !(is_alpha || is_digit || c == '_'))
            return false;
    }
    return true;
}

bool is_kind(dstd::StringView kind) {
    for (const char* known : KIND_NAMES)
        if (kind == known) return true;
    return false;
}

/// Whether @p relative (a template path, such as "fold/plugin.cpp") is part of
/// the scaffold of @p kind. The sample trace is shared by every kind.
bool belongs_to(dstd::StringView relative, dstd::StringView kind) {
    if (relative.starts_with("sample/")) return true;
    if (!relative.starts_with(kind)) return false;
    return relative.size() > kind.size() && relative[kind.size()] == '/';
}

/// Where one template file lands inside the new project: the sample trace is
/// shared by every kind and goes to `data/`.
dstd::String target_path(dstd::StringView relative) {
    constexpr dstd::StringView SAMPLE_PATH = "sample/sample.pfw.gz";
    if (relative == SAMPLE_PATH) return dstd::String("data/sample.pfw.gz");
    return dstd::String(relative);
}

/// The template's bytes with every @NAME@ replaced by @p name (text only).
dstd::String substituted(const cli::PluginTemplate& file,
                         dstd::StringView name) {
    dstd::String out;
    if (!file.is_text) {
        out.assign(file.data, file.size);
        return out;
    }
    const dstd::StringView text = text_of(file);
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t token = text.find("@NAME@", at);
        if (token == dstd::StringView::npos) {
            out.append(text.substr(at).data(), text.size() - at);
            break;
        }
        out.append(text.substr(at, token - at).data(), token - at);
        out.append(name.data(), name.size());
        at = token + 6;
    }
    return out;
}

/// Write one file, creating the folders above it.
dstd::Result<void> write_project_file(const dstd::String& path,
                                      const dstd::String& data) {
    const dstd::StringView view(path);
    const std::size_t slash = view.find_last_of('/');
    if (slash != dstd::StringView::npos) {
        auto made =
            dstd::fs::create_directories(dstd::PathView(view.substr(0, slash)));
        if (!made.has_value()) return made.error();
    }
    return dstd::fs::write_file(dstd::PathView(path), data);
}

int run_new(const dstd::String& kind, const dstd::String& name,
            dstd::StringView dir) {
    const dstd::String root =
        dir.empty() ? dstd::String(".") : dstd::String(dir);
    const dstd::String project = dstd::fmt<"{}/{}">(root, name);
    auto exists = dstd::fs::exists(dstd::PathView(project));
    if (!exists.has_value()) {
        DFTRACER_UTILS_LOG_ERROR("Error: {}", exists.error().format());
        return 1;
    }
    if (*exists) {
        DFTRACER_UTILS_LOG_ERROR(
            "Error: {} already exists; move it away or pick another name",
            project);
        return 1;
    }
    dstd::String listing;
    for (std::size_t i = 0; i < cli::PLUGIN_TEMPLATE_COUNT; ++i) {
        const cli::PluginTemplate& file = cli::PLUGIN_TEMPLATES[i];
        const dstd::StringView relative(file.path);
        if (!belongs_to(relative, kind)) continue;
        const dstd::StringView leaf = relative.starts_with("sample/")
                                          ? relative
                                          : relative.substr(kind.size() + 1);
        const dstd::String path =
            dstd::fmt<"{}/{}">(project, target_path(leaf));
        auto written = write_project_file(path, substituted(file, name));
        if (!written.has_value()) {
            DFTRACER_UTILS_LOG_ERROR("Error: {}", written.error().format());
            return 1;
        }
        listing += "  ";
        listing += path;
        listing += '\n';
    }
    if (listing.empty()) {
        DFTRACER_UTILS_LOG_ERROR("Error: no template for kind '{}'", kind);
        return 1;
    }
    std::printf("made a %s plugin in %s:\n%s", kind.c_str(), project.c_str(),
                listing.c_str());
    return 0;
}

// ---------------------------------------------------------------- child ----

/// A folder this run owns for the index its golden queries build, removed when
/// it goes away. A default-constructed one has no folder when the temporary
/// directory cannot be made.
class TempIndexRoot {
   public:
    TempIndexRoot() {
        static int counter = 0;
        const dstd::Path temp = dstd::fs::temp_directory();
        const dstd::String root(temp.str());
        path_ = dstd::fmt<"{}/dftracer_plugin_{}_{}">(
            root, static_cast<long long>(::getpid()), ++counter);
        auto made = dstd::fs::create_directories(dstd::PathView(path_));
        if (!made.has_value()) path_.clear();
    }
    TempIndexRoot(const TempIndexRoot&) = delete;
    TempIndexRoot& operator=(const TempIndexRoot&) = delete;
    ~TempIndexRoot() {
        if (path_.empty()) return;
        // Nothing can report a folder that stays behind: the run is over.
        [[maybe_unused]] auto removed =
            dstd::fs::remove_all(dstd::PathView(path_));
    }

    dstd::StringView path() const { return dstd::StringView(path_); }

   private:
    dstd::String path_;
};

void print_line(const char* prefix, dstd::StringView text) {
    std::printf("%s%.*s\n", prefix, static_cast<int>(text.size()), text.data());
}

int report_golden(dstd::StringView tests_dir, bool record,
                  const plugins::host::Plugins* folds) {
    auto files = cli::golden_test_files(tests_dir);
    if (!files.has_value()) {
        print_line("golden: ", files.error().message());
        return 1;
    }
    if (files->empty()) {
        std::printf("golden: no .test file in %.*s\n",
                    static_cast<int>(tests_dir.size()), tests_dir.data());
        return 0;
    }
    // The queries build an index of the inputs they read. It goes in a folder
    // this run owns and removes, so no `.dftindex` lands beside a user's trace.
    const TempIndexRoot index_root;
    if (index_root.path().empty()) {
        std::printf("golden: no writable temporary folder for the index\n");
        return 1;
    }
    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const dstd::String& path : *files) {
        const dstd::StringView path_view(path);
        auto text = dstd::fs::read_file(dstd::PathView(path));
        if (!text.has_value()) {
            print_line("golden: ", text.error().message());
            ++failed;
            continue;
        }
        auto parsed =
            cli::parse_golden_file(dstd::StringView(*text), path_view);
        if (!parsed.has_value()) {
            print_line("golden: ", parsed.error().message());
            ++failed;
            continue;
        }
        const std::size_t slash = path_view.find_last_of('/');
        const dstd::String base = slash == dstd::StringView::npos
                                      ? dstd::String(".")
                                      : path.substr(0, slash);
        auto ran = cli::run_golden_file(*parsed, dstd::StringView(base),
                                        index_root.path(), record, folds);
        if (!ran.has_value()) {
            print_line("golden: ", ran.error().message());
            ++failed;
            continue;
        }
        for (std::size_t i = 0; i < ran->blocks.size(); ++i) {
            const cli::GoldenOutcome& outcome = ran->blocks[i];
            const cli::GoldenBlock& block = parsed->blocks[i];
            std::printf("%.*s:%zu: %s\n", static_cast<int>(path_view.size()),
                        path_view.data(), outcome.line,
                        outcome.ok ? (record ? "RECORDED" : "PASS") : "FAIL");
            if (outcome.ok) {
                ++passed;
                continue;
            }
            ++failed;
            std::printf("--- expected\n+++ actual\n");
            if (!block.error_text.empty())
                std::printf(" expected an error holding '%s'\n",
                            block.error_text.c_str());
            const dstd::StringView expected(block.expected);
            std::size_t at = 0;
            while (at < expected.size()) {
                const std::size_t nl = expected.find('\n', at);
                const std::size_t end =
                    nl == dstd::StringView::npos ? expected.size() : nl;
                print_line("- ", expected.substr(at, end - at));
                at = nl == dstd::StringView::npos ? expected.size() : nl + 1;
            }
            const dstd::StringView actual(outcome.actual);
            at = 0;
            while (at < actual.size()) {
                const std::size_t nl = actual.find('\n', at);
                const std::size_t end =
                    nl == dstd::StringView::npos ? actual.size() : nl;
                print_line("+ ", actual.substr(at, end - at));
                at = nl == dstd::StringView::npos ? actual.size() : nl + 1;
            }
        }
    }
    std::printf("golden: %zu passed, %zu failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

int report_plugin(const dstd::String& lib, dstd::StringView tests_dir,
                  bool record) {
    int code = 0;
    std::printf("checking: %s\n", lib.c_str());
    {
        auto builder = plugins::host::Plugins::builder();
        builder.add(lib);
        auto loaded = builder.build();
        if (!loaded.has_value()) {
            print_line("load error: ", loaded.error().message());
            return 1;
        }
        std::size_t folds = 0;
        for (const auto& info : loaded->describe()) {
            cli::print_plugin_info(stdout, info);
            if (info.has_fold) ++folds;
        }
        // A plugin that carries a fold runs it over every trace input, so a
        // fold plugin is checked by its own golden test.
        if (!tests_dir.empty())
            code = report_golden(tests_dir, record,
                                 folds == 0 ? nullptr : &*loaded);
        // Leaving this scope unloads the plugin: its destroy runs and the
        // memory exit check reports what it left allocated.
    }
    std::printf("plugin unloaded\n");
    std::fflush(stdout);
    return code;
}

// --------------------------------------------------------------- parent ----

dstd::String self_path(const char* argv0) {
    char path[4096];
#if defined(__APPLE__)
    std::uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) return dstd::String(path);
#endif
    const ssize_t got = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (got > 0) return dstd::String(dstd::StringView(path, got));
    return dstd::String(argv0 == nullptr ? "dftracer_plugin" : argv0);
}

const char* signal_name(int number) {
    switch (number) {
        case SIGABRT:
            return "SIGABRT";
        case SIGSEGV:
            return "SIGSEGV";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGKILL:
            return "SIGKILL";
        case SIGTERM:
            return "SIGTERM";
        case SIGPIPE:
            return "SIGPIPE";
        case SIGTRAP:
            return "SIGTRAP";
        default:
            return "unknown signal";
    }
}

/// `K=V` entries for the child: the memory debug mode, the exit check and the
/// plugin frame check, all on. A value the caller set for one of the three goes
/// away first, so the child always starts with all three on.
dstd::Vector<dstd::String> child_env() {
    const dstd::StringView names[] = {memory::detail::constants::DEBUG_ENV,
                                      memory::detail::constants::EXIT_CHECK_ENV,
                                      common::constants::plugins::CHECK_ENV};
    dstd::Vector<dstd::String> env;
    for (char** it = environ; *it != nullptr; ++it) {
        const dstd::StringView entry(*it);
        bool replaced = false;
        for (const dstd::StringView name : names) {
            if (entry.size() > name.size() && entry[name.size()] == '=' &&
                entry.starts_with(name))
                replaced = true;
        }
        if (!replaced) env.emplace_back(entry);
    }
    for (const dstd::StringView name : names)
        env.emplace_back(dstd::fmt<"{}={}">(name, "1"));
    return env;
}

int run_check(const char* argv0, const dstd::String& lib,
              dstd::StringView tests, bool record) {
    dstd::Vector<dstd::String> owned;
    owned.push_back(self_path(argv0));
    owned.push_back(dstd::String(CHECK_CHILD_FLAG));
    owned.push_back(lib);
    if (!tests.empty()) {
        owned.push_back(dstd::String("--tests"));
        owned.push_back(dstd::String(tests));
    }
    if (record) owned.push_back(dstd::String("--record"));
    dstd::Vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (dstd::String& arg : owned) argv.push_back(arg.data());
    argv.push_back(nullptr);

    dstd::Vector<dstd::String> env = child_env();
    dstd::Vector<char*> envp;
    envp.reserve(env.size() + 1);
    for (dstd::String& entry : env) envp.push_back(entry.data());
    envp.push_back(nullptr);

    int fds[2];
    if (::pipe(fds) != 0) {
        DFTRACER_UTILS_LOG_ERROR("Error: cannot make a pipe: {}",
                                 std::strerror(errno));
        return 1;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        DFTRACER_UTILS_LOG_ERROR("Error: cannot fork: {}",
                                 std::strerror(errno));
        return 1;
    }
    if (child == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        ::execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }
    ::close(fds[1]);
    dstd::String captured;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(fds[0], buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        captured.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    // The child runs with the debug allocator, which ranks its live sites at
    // exit. Those lines say nothing about the plugin, so they stay out of the
    // report; every other line the child wrote is shown as it came.
    const dstd::StringView all(captured);
    std::size_t at = 0;
    while (at < all.size()) {
        const std::size_t nl = all.find('\n', at);
        const std::size_t end = nl == dstd::StringView::npos ? all.size() : nl;
        const dstd::StringView line = all.substr(at, end - at);
        const bool allocator_noise =
            line.starts_with("dftracer-utils memory:") &&
            line.find("exit check") == dstd::StringView::npos &&
            line.find("memory: leak:") == dstd::StringView::npos;
        if (!allocator_noise) print_line("", line);
        at = nl == dstd::StringView::npos ? all.size() : nl + 1;
    }

    if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        std::printf("crash: the child died from signal %d (%s)\n", number,
                    signal_name(number));
        return 1;
    }
    const bool leaks =
        captured.find("exit check failed") != dstd::String::npos ||
        captured.find("memory: leak:") != dstd::String::npos;
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (leaks)
        std::printf("leaks: the child reported memory the plugin kept\n");
    else
        std::printf("no leaks\n");
    if (code == 0) {
        std::printf("check: ok\n");
        return 0;
    }
    std::printf("check: failed (child exit %d)\n", code);
    return 1;
}

class PluginArgParse : public cli::ArgParse {
   public:
    dstd::Vector<dstd::StringView> words;
    dstd::StringView dir;
    dstd::StringView tests;
    bool record = false;
    dstd::StringView lib;

    explicit PluginArgParse(cli::ArgParser& p) : ArgParse(p) {}

   protected:
    void register_args() override {
        parser().positional(words, "<subcommand> [<kind> <name> | <library>]",
                            "new <kind> <name> or check <library>");
        parser().option(dir, {"--dir"}, "Folder to make the project in (new)");
        parser().option(tests, {"--tests"},
                        "Folder of *.test golden tests to run (check)");
        parser().flag(record, {"--record"},
                      "Rewrite each expected block with what the query gave");
        parser().option(lib, {CHECK_CHILD_FLAG},
                        "Internal: the child process of check");
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* argv0 = argc > 0 ? argv[0] : "dftracer_plugin";
    return cli::cli_main<PluginArgParse>(
        argc, argv, "dftracer_plugin",
        "Make a dftracer-utils plugin project, and check a built one. `new "
        "<kind> <name> [--dir DIR]` writes one standalone CMake project for a "
        "fold, udf, aggregate, format, sink or node plugin, with a README and "
        "a "
        "golden test. `check <lib> [--tests DIR] [--record]` loads the plugin "
        "in a child process with the memory exit check on, prints what it "
        "provides, runs the golden tests of DIR and reports its leaks or a "
        "crash.",
        [argv0](PluginArgParse& cli) -> int {
            if (!cli.lib.empty())
                return report_plugin(dstd::String(cli.lib), cli.tests,
                                     cli.record);
            if (cli.words.empty()) {
                DFTRACER_UTILS_LOG_ERROR(
                    "Error: name a subcommand: new <kind> <name>, or check "
                    "<library>");
                return 1;
            }
            const dstd::StringView command = cli.words[0];
            if (command == "new") {
                if (cli.words.size() != 3) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Error: new takes a kind and a name: new <kind> <name> "
                        "[--dir DIR]");
                    return 1;
                }
                const dstd::StringView kind = cli.words[1];
                const dstd::StringView name = cli.words[2];
                if (!is_kind(kind)) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Error: kind '{}' is not one of fold, udf, aggregate, "
                        "format, sink, node",
                        dstd::String(kind));
                    return 1;
                }
                if (!is_c_identifier(name)) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Error: name '{}' is not a C identifier",
                        dstd::String(name));
                    return 1;
                }
                return run_new(dstd::String(kind), dstd::String(name), cli.dir);
            }
            if (command == "check") {
                if (cli.words.size() != 2) {
                    DFTRACER_UTILS_LOG_ERROR(
                        "Error: check takes one library: check <library> "
                        "[--tests DIR] [--record]");
                    return 1;
                }
                return run_check(argv0, dstd::String(cli.words[1]), cli.tests,
                                 cli.record);
            }
            DFTRACER_UTILS_LOG_ERROR("Error: unknown subcommand '{}'",
                                     dstd::String(command));
            return 1;
        });
}
