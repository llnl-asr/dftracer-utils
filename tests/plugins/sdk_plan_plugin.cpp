// SPDX-License-Identifier: MIT
// A source and a file format written with the C++ SDK that have plan hooks, so
// the SDK sets the plan and plan_duql entries and a token the plan gave reaches
// scan().
//
//   sdk_plan.src      rows id 1..3 and `tok`, the token the plan gave to scan
//                     (-1 when there is none); plan: every filter INEXACT,
//                     token 42, est_rows 3, cost 7.5
//   sdk_plan.dq       the same rows; plan_duql keeps the duql text it got and
//                     its explain hook prints it; est_rows is the number of
//                     filters in the text, token the length of the text
//   sdk_plan.fmt      a file that starts with the line "SDKP", then one integer
//                     per line, in the column "id", one chunk; plan gives
//                     token 7 and est_rows the rows, and scan counts `token`
//                     with the token it got; plan_duql gives est_rows 1000 plus
//                     the filters in the text

#include <dftracer/utils/dataframe/c_api.h>
#include <dftracer/utils/dstd/fs.h>
#include <dftracer/utils/dstd/memory.h>
#include <dftracer/utils/dstd/optional.h>
#include <dftracer/utils/dstd/span.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/dstd/to_chars.h>
#include <dftracer/utils/dstd/vector.h>
#include <dftracer/utils/plugins/plugin.h>
#include <dftracer/utils/plugins/plugin/file_format.h>

#include <cstdint>

using namespace dftracer::utils;
using namespace dftracer::utils::plugins;

namespace {

dstd::String g_last_duql;

dstd::Vector<Pushed> all_inexact(const ScanView& view) {
    return dstd::Vector<Pushed>(view.filters.size(), Pushed::Inexact);
}

class RowsCursor {
   public:
    explicit RowsCursor(std::int64_t token) : token_(token) {}
    dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) {
        if (done_) return dstd::nullopt;
        done_ = true;
        FrameBuilder builder;
        builder.add("id", Column::from_int64({1, 2, 3}));
        builder.add("tok", Column::from_int64({token_, token_, token_}));
        auto built = builder.build();
        if (!built) return std::move(built).error();
        return dstd::Optional<OwnedDataFrame>(std::move(*built));
    }

   private:
    std::int64_t token_;
    bool done_ = false;
};

dstd::Result<ScanResult<RowsCursor>> rows_scan(const ScanView& view) {
    const std::int64_t token =
        view.plan ? static_cast<std::int64_t>(view.plan->token) : -1;
    return ScanResult<RowsCursor>{dstd::make_unique<RowsCursor>(token), {}};
}

class SdkPlanSource {
   public:
    dstd::Vector<dstd::String> names() const { return {"id", "tok"}; }

    dstd::Result<ScanResult<RowsCursor>> scan(const ScanView& view,
                                              const Host&) const {
        return rows_scan(view);
    }

    dstd::Result<PlanResult> plan(const ScanView& view) const {
        PlanResult plan;
        plan.votes = all_inexact(view);
        plan.cost = 7.5;
        plan.est_rows = 3;
        plan.token = 42;
        return plan;
    }
};

class SdkDuqlSource {
   public:
    dstd::Vector<dstd::String> names() const { return {"id", "tok"}; }

    dstd::Result<ScanResult<RowsCursor>> scan(const ScanView& view,
                                              const Host&) const {
        return rows_scan(view);
    }

    dstd::Result<PlanResult> plan_duql(
        const ScanView& view, dstd::StringView text,
        dstd::Span<const std::int32_t> index) const {
        g_last_duql = dstd::String(text);
        PlanResult plan;
        plan.votes = all_inexact(view);
        plan.est_rows = static_cast<std::int64_t>(index.size());
        plan.token = text.size();
        return plan;
    }

    dstd::Result<void> explain(const ScanView&, const ExplainOut& out) const {
        out.add("duql", g_last_duql);
        return ok();
    }
};

class SdkPlainSource {
   public:
    dstd::Vector<dstd::String> names() const { return {"id", "tok"}; }

    dstd::Result<ScanResult<RowsCursor>> scan(const ScanView& view,
                                              const Host&) const {
        return rows_scan(view);
    }
};

class SdkCursor : public FormatCursor {
   public:
    explicit SdkCursor(dstd::Vector<std::int64_t> ids) : ids_(std::move(ids)) {}

    dstd::Result<dstd::Optional<OwnedDataFrame>> next(std::int64_t) override {
        if (done_) return dstd::Optional<OwnedDataFrame>();
        done_ = true;
        FrameBuilder builder;
        builder.add("id", Column::from_int64(std::move(ids_)));
        auto built = builder.build();
        if (!built)
            return dstd::Unexpected(
                invalid_argument("sdk_plan.fmt: cannot build a frame"));
        return dstd::Optional<OwnedDataFrame>(std::move(*built));
    }

   private:
    dstd::Vector<std::int64_t> ids_;
    bool done_ = false;
};

class SdkFile : public FormatFile {
   public:
    explicit SdkFile(dstd::Vector<std::int64_t> ids) : ids_(std::move(ids)) {}

    void schema(SchemaBuilder& out) const override {
        out.add("id", TypeId::Int64, false);
    }

    std::uint64_t chunks() const override { return 1; }

    dstd::Result<PlanResult> plan(const ScanView& view) {
        g_last_duql = "plan";
        PlanResult plan;
        plan.votes.assign(view.filters.size(), Pushed::No);
        plan.est_rows = static_cast<std::int64_t>(ids_.size());
        plan.cost = static_cast<double>(ids_.size());
        plan.token = 7;
        return plan;
    }

    dstd::Result<PlanResult> plan_duql(const ScanView& view,
                                       dstd::StringView text,
                                       dstd::Span<const std::int32_t> index) {
        g_last_duql = dstd::String(text);
        PlanResult plan;
        plan.votes.assign(view.filters.size(), Pushed::No);
        plan.est_rows = 1000 + static_cast<std::int64_t>(index.size());
        plan.token = 7;
        return plan;
    }

    dstd::Result<void> explain(const ScanView&, const ExplainOut& out) {
        out.add("duql", g_last_duql);
        return ok();
    }

    dstd::Result<ScanResult<FormatCursor>> scan(std::uint64_t,
                                                const ScanView& view) override {
        if (view.plan)
            counter("token", static_cast<std::int64_t>(view.plan->token));
        ScanResult<FormatCursor> result;
        result.cursor =
            dstd::UniquePtr<FormatCursor>(dstd::make_unique<SdkCursor>(ids_));
        return result;
    }

   private:
    dstd::Vector<std::int64_t> ids_;
};

class SdkFormat : public FileFormat {
   public:
    using File = SdkFile;

    std::int32_t probe(dstd::ByteView head, dstd::CStringView) override {
        const dstd::StringView text(reinterpret_cast<const char*>(head.data()),
                                    head.size());
        return text.starts_with("SDKP\n") ? 90 : 0;
    }

    dstd::Result<dstd::UniquePtr<FormatFile>> open(
        dstd::StringView path) override {
        auto text = dstd::fs::read_file(dstd::PathView(path));
        if (!text)
            return dstd::Unexpected(io_error("sdk_plan.fmt: cannot read file"));
        const dstd::StringView body(*text);
        dstd::Vector<std::int64_t> ids;
        std::size_t at = 5;
        while (at < body.size()) {
            const std::size_t newline = body.find('\n', at);
            const std::size_t end =
                newline == dstd::StringView::npos ? body.size() : newline;
            const dstd::Optional<std::int64_t> id =
                dstd::from_chars<std::int64_t>(body.substr(at, end - at));
            if (!id)
                return dstd::Unexpected(
                    invalid_argument("sdk_plan.fmt: a row is not an integer"));
            ids.push_back(*id);
            at = end + 1;
        }
        dstd::UniquePtr<FormatFile> file =
            dstd::make_unique<SdkFile>(std::move(ids));
        return file;
    }
};

}  // namespace

DFTU_PLUGIN("sdk_plan", "1.0",
            [](PluginBuilder& p, const Config&) -> dstd::Result<void> {
                p.source<SdkPlanSource>("sdk_plan.src",
                                        dstd::make_unique<SdkPlanSource>());
                p.source<SdkDuqlSource>("sdk_plan.dq",
                                        dstd::make_unique<SdkDuqlSource>());
                p.source<SdkPlainSource>("sdk_plan.plain",
                                         dstd::make_unique<SdkPlainSource>());
                p.file_format<SdkFormat>("sdk_plan.fmt",
                                         dstd::make_unique<SdkFormat>());
                return {};
            })
