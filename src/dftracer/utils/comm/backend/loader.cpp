// SPDX-License-Identifier: MIT
#include <dftracer/utils/comm/backend/loader.h>
#include <dftracer/utils/common/constants.h>
#include <dftracer/utils/common/env.h>
#include <dftracer/utils/common/versions.h>
#include <dftracer/utils/dstd/error.h>
#include <dftracer/utils/dstd/fmt.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/vector.h>
#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>

#ifndef DFTRACER_UTILS_COMM_BUILD_STAMP
#error "the build must define DFTRACER_UTILS_COMM_BUILD_STAMP for comm"
#endif

namespace dftracer::utils::comm::backend {

namespace {

bool is_set(dstd::StringView name) {
    return common::Env::get<dstd::StringView>(name).has_value();
}

/// The number of tasks a variable names, or 0 when it is unset or no number.
int tasks_of(dstd::StringView name) {
    return common::Env::get<int>(name).value_or(0);
}

dstd::String directory_of_comm() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&launched_by_mpi), &info) == 0 ||
        info.dli_fname == nullptr)
        return {};
    const dstd::StringView path(info.dli_fname);
    const std::size_t at = path.rfind('/');
    if (at == dstd::StringView::npos) return {};
    return dstd::String(path.substr(0, at));
}

}  // namespace

const char* build_stamp() noexcept { return DFTRACER_UTILS_COMM_BUILD_STAMP; }

bool launched_by_mpi() {
    // A launcher counts only when it runs more than one task: a job of one
    // rank, such as mpirun -n 1, runs the tool as it runs without a launcher.
    static const char* const WORLD_SIZES[] = {
        "OMPI_COMM_WORLD_SIZE", "PMIX_SIZE",       "PMI_SIZE",
        "MV2_COMM_WORLD_SIZE",  "MPI_LOCALNRANKS",
    };
    for (const char* name : WORLD_SIZES)
        if (tasks_of(name) > 1) return true;
    if (tasks_of("FLUX_JOB_SIZE") > 1 || tasks_of("FLUX_TASK_COUNT") > 1)
        return is_set("FLUX_TASK_RANK");
    return tasks_of("SLURM_STEP_NUM_TASKS") > 1 && is_set("SLURM_PROCID");
}

BackendKind resolve_kind(BackendKind kind) {
    if (kind != BackendKind::Auto) return kind;
    return launched_by_mpi() ? BackendKind::Mpi : BackendKind::Local;
}

dstd::Result<Providers> load_backend(const StartOptions& options,
                                     const char* symbol) {
    dstd::Vector<dstd::String> candidates;
#if defined(__APPLE__)
    const char* const name = "libdftracer_utils_comm_mpi.dylib";
#else
    const char* const name = "libdftracer_utils_comm_mpi.so";
#endif
    if (auto dir = common::Env::get<dstd::StringView>(
            common::constants::comm_settings::COMM_PATH_ENV);
        dir && !dir->empty()) {
        // An explicit directory is the only place searched.
        candidates.push_back(dstd::fmt<"{}/{}">(*dir, dstd::StringView(name)));
    } else {
        if (dstd::String here = directory_of_comm(); !here.empty())
            candidates.push_back(
                dstd::fmt<"{}/{}">(here, dstd::StringView(name)));
        candidates.push_back(dstd::String(name));
    }

    dstd::String tried;
    void* handle = nullptr;
    for (const dstd::String& path : candidates) {
        // Global, so libmpi and the plugins it loads resolve their symbols
        // against each other, as mpi4py does.
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle != nullptr) break;
        const char* why = dlerror();
        if (!tried.empty()) tried += "; ";
        tried += dstd::fmt<"{}: {}">(
            path, dstd::StringView(why != nullptr ? why : "unknown error"));
    }
    if (handle == nullptr) {
        return dstd::Unexpected(
            dstd::Error::from_format<
                "the MPI backend library was not found or did "
                "not load; tried {}">(dstd::Condition::NotFound, tried));
    }
    using EntryFn = const BackendEntry* (*)();
    void* found = dlsym(handle, symbol);
    if (found == nullptr) {
        const char* why = dlerror();
        dlclose(handle);
        return dstd::Unexpected(
            dstd::Error::from_format<"the backend library has no entry {}: {}">(
                dstd::Condition::Unsupported, dstd::StringView(symbol),
                dstd::StringView(why != nullptr ? why : "unknown error")));
    }
    const BackendEntry* entry = reinterpret_cast<EntryFn>(found)();
    if (entry == nullptr || entry->api_version == nullptr ||
        entry->build_stamp == nullptr || entry->start == nullptr) {
        dlclose(handle);
        return dstd::Unexpected(
            dstd::Error::from_format<"the backend library gave no entry">(
                dstd::Condition::Unsupported));
    }
    const dstd::StringView api(entry->api_version);
    const dstd::StringView stamp(entry->build_stamp);
    if (api != dstd::StringView(DFTRACER_UTILS_API_VERSION_STRING) ||
        stamp != dstd::StringView(build_stamp())) {
        dstd::Error why = dstd::Error::from_format<
            "the backend library was built for api {} stamp {} but comm is api "
            "{} stamp {}">(dstd::Condition::Unsupported, api, stamp,
                           dstd::StringView(DFTRACER_UTILS_API_VERSION_STRING),
                           dstd::StringView(build_stamp()));
        dlclose(handle);
        return dstd::Unexpected(std::move(why));
    }
    // The library stays mapped: the providers it made run its code until they
    // are gone, which is the end of the process for a tool.
    return entry->start(options);
}

}  // namespace dftracer::utils::comm::backend
