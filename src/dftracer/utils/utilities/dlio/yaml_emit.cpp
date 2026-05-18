#include <dftracer/utils/utilities/dlio/yaml_emit.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <ostream>
#include <sstream>
#include <string>
#include <variant>

namespace dftracer::utils::utilities::dlio {

namespace stats = ::dftracer::utils::utilities::common::statistics;

namespace {

YAML::Node emit_single(const stats::FittedDistribution& f) {
    YAML::Node n;
    switch (f.kind) {
        case stats::DistributionKind::Normal:
            n["type"] = "normal";
            n["mean"] = f.params[0];
            n["stdev"] = f.params[1];
            break;
        case stats::DistributionKind::Lognormal:
            n["type"] = "lognormal";
            n["mean"] = f.params[0];
            n["sigma"] = f.params[1];
            break;
        case stats::DistributionKind::Gamma:
            n["type"] = "gamma";
            n["shape"] = f.params[0];
            n["scale"] = f.params[1];
            break;
        case stats::DistributionKind::Exponential:
            // params[0] is rate; DLIO config expects "scale" = 1/rate.
            n["type"] = "exponential";
            n["scale"] = f.params[0] > 0.0 ? 1.0 / f.params[0] : 0.0;
            break;
        case stats::DistributionKind::Weibull:
            n["type"] = "weibull";
            n["shape"] = f.params[0];
            n["scale"] = f.params[1];
            break;
    }
    return n;
}

YAML::Node emit_mixture(const stats::FittedMixture& m) {
    YAML::Node n;
    n["type"] = "mixture";
    n["n_components"] = static_cast<int>(m.weights.size());
    YAML::Node components(YAML::NodeType::Sequence);
    for (std::size_t k = 0; k < m.weights.size(); ++k) {
        YAML::Node comp;
        comp["weight"] = m.weights[k];
        YAML::Node params;
        params["type"] = "normal";
        params["mean"] = m.components[k].mean;
        params["stdev"] = m.components[k].stddev;
        comp["params"] = params;
        components.push_back(comp);
    }
    n["components"] = components;
    return n;
}

YAML::Node emit_block(const DlioTimingBlock& block) {
    YAML::Node n = std::visit(
        [](const auto& v) -> YAML::Node {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, stats::FittedDistribution>) {
                return emit_single(v);
            } else {
                return emit_mixture(v);
            }
        },
        block.model);
    n["max_bound"] = block.max_bound;
    return n;
}

}  // namespace

std::string render_dlio_yaml(const DlioTimingBlock* computation,
                             const DlioTimingBlock* preprocess) {
    YAML::Node root;
    if (computation) {
        YAML::Node train;
        train["computation_time"] = emit_block(*computation);
        root["train"] = train;
    }
    if (preprocess) {
        YAML::Node reader;
        reader["preprocess_time"] = emit_block(*preprocess);
        root["reader"] = reader;
    }
    YAML::Emitter emit;
    emit.SetIndent(2);
    emit.SetMapFormat(YAML::Block);
    emit.SetSeqFormat(YAML::Block);
    emit << root;
    return std::string(emit.c_str());
}

bool write_dlio_yaml(std::ostream& out, const DlioTimingBlock* computation,
                     const DlioTimingBlock* preprocess) {
    out << render_dlio_yaml(computation, preprocess);
    out << "\n";
    return static_cast<bool>(out);
}

}  // namespace dftracer::utils::utilities::dlio
