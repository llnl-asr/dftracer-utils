// SPDX-License-Identifier: MIT
#include <dftracer/utils/domains/workload/dlio/yaml_emit.h>
#include <dftracer/utils/dstd/string.h>
#include <dftracer/utils/yaml/writer.h>

#include <cstddef>
#include <variant>

namespace dftracer::utils::domains::workload::dlio {

namespace {

template <class W>
void write_single(W& w, const FittedDistribution& f) {
    switch (f.kind) {
        case DistributionKind::Normal:
            w.member("type", "normal");
            w.member("mean", f.params[0]);
            w.member("stdev", f.params[1]);
            break;
        case DistributionKind::Lognormal:
            w.member("type", "lognormal");
            w.member("mean", f.params[0]);
            w.member("sigma", f.params[1]);
            break;
        case DistributionKind::Gamma:
            w.member("type", "gamma");
            w.member("shape", f.params[0]);
            w.member("scale", f.params[1]);
            break;
        case DistributionKind::Exponential:
            // params[0] is rate; DLIO config expects "scale" = 1/rate.
            w.member("type", "exponential");
            w.member("scale", f.params[0] > 0.0 ? 1.0 / f.params[0] : 0.0);
            break;
        case DistributionKind::Weibull:
            w.member("type", "weibull");
            w.member("shape", f.params[0]);
            w.member("scale", f.params[1]);
            break;
    }
}

template <class W>
void write_mixture(W& w, const FittedMixture& m) {
    w.member("type", "mixture");
    w.member("n_components", m.weights.size());
    w.key("components");
    w.begin_array();
    for (std::size_t k = 0; k < m.weights.size(); ++k) {
        w.begin_object();
        w.member("weight", m.weights[k]);
        w.key("params");
        w.begin_object();
        w.member("type", "normal");
        w.member("mean", m.components[k].mean);
        w.member("stdev", m.components[k].stddev);
        w.end_object();
        w.end_object();
    }
    w.end_array();
}

template <class W>
void write_block(W& w, const char* name, const DlioTimingBlock& block) {
    w.key(name);
    w.begin_object();
    if (const auto* single = std::get_if<FittedDistribution>(&block.model))
        write_single(w, *single);
    else
        write_mixture(w, std::get<FittedMixture>(block.model));
    w.member("max_bound", block.max_bound);
    w.end_object();
}

}  // namespace

dstd::String render_dlio_yaml(const DlioTimingBlock* computation,
                              const DlioTimingBlock* preprocess) {
    dstd::String out;
    if (!computation && !preprocess) return out;
    yaml::Writer w(out);
    w.begin_object();
    if (computation) {
        w.key("train");
        w.begin_object();
        write_block(w, "computation_time", *computation);
        w.end_object();
    }
    if (preprocess) {
        w.key("reader");
        w.begin_object();
        write_block(w, "preprocess_time", *preprocess);
        w.end_object();
    }
    w.end_object();
    return out;
}

}  // namespace dftracer::utils::domains::workload::dlio
