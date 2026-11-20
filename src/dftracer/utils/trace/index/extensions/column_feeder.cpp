// SPDX-License-Identifier: MIT
#include <dftracer/utils/trace/index/extensions/column_feeder.h>
#include <dftracer/utils/trace/index/extensions/summaries.h>
#include <dftracer/utils/trace/index/tables.h>

#include <cstdint>

namespace dftracer::utils::trace::index::extensions {

ColumnFeeder::ColumnFeeder(
    dstd::Vector<dstd::SharedPtr<const Summary>> summaries)
    : summaries_(std::move(summaries)) {}

void ColumnFeeder::feed(std::uint64_t granule,
                        const dataframe::DataFrame& frame) {
    const auto rows = static_cast<std::uint64_t>(frame.num_rows());
    if (builders_) builders_->step_frame(granule, frame);
    for (std::size_t c = 0; c < frame.num_columns(); ++c) {
        const dstd::String path(frame.names[c].view());
        const auto& column = frame.columns[c];
        auto& file = files_[path];
        if (file.builders.empty()) file.builders.resize(summaries_.size());
        for (std::size_t s = 0; s < summaries_.size(); ++s) {
            const Summary& summary = *summaries_[s];
            if (filter_ && !filter_(path, summary.extension())) continue;
            auto builder = summary.builder(column.type());
            if (!builder) continue;
            builder->set_observed(rows);
            if (!builder->add(column)) continue;
            ExtensionOut out{{}, UINT64_MAX, false};
            if (builder->finish_granule(out))
                granules_.push_back(
                    {summary.extension(), path, granule, std::move(out.bytes)});
            if (!builder->has_file_payload()) continue;
            if (!file.builders[s])
                file.builders[s] = summary.builder(column.type());
            file.builders[s]->merge_from(*builder);
        }
    }
}

dstd::Vector<ColumnFeeder::Payload> ColumnFeeder::finish() {
    dstd::Vector<Payload> out = std::move(granules_);
    granules_.clear();
    for (auto& [path, file] : files_) {
        for (std::size_t s = 0; s < file.builders.size(); ++s) {
            if (!file.builders[s]) continue;
            ExtensionOut bytes{{}, UINT64_MAX, false};
            if (file.builders[s]->finish_file(bytes))
                out.push_back({summaries_[s]->extension(), path, dstd::nullopt,
                               std::move(bytes.bytes)});
        }
    }
    files_.clear();
    return out;
}

void ColumnFeeder::write(trace::index::FileBuild& build,
                         dstd::Span<const Payload> payloads) const {
    dstd::Map<dstd::String, dstd::Vector<store::IndexExtension>> marked;
    const auto mark = [&](const Payload& p) {
        auto& seen = marked[p.path];
        for (auto ext : seen)
            if (ext == p.extension) return;
        seen.push_back(p.extension);
        trace::index::put_path(build, p.extension, p.path);
    };
    for (const Payload& p : payloads) {
        mark(p);
        if (p.extension == store::IndexExtension::Postings) {
            for (std::uint64_t h : PostingsBuilder::decode(p.bytes)) {
                if (p.granule)
                    trace::index::put_posting_granule(build, p.path, h,
                                                      *p.granule);
                else
                    trace::index::put_posting(build, p.path, h);
            }
        } else if (p.granule) {
            trace::index::put_path_granule(build, p.extension, p.path,
                                           *p.granule, p.bytes);
        } else {
            trace::index::put_path_file(build, p.extension, p.path, p.bytes);
        }
    }
    for (const auto& summary : summaries_)
        build.finish(summary->extension(), summary->params_hash());
}

}  // namespace dftracer::utils::trace::index::extensions
