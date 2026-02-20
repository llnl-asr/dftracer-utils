#ifndef DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_PREDICATE_PARSER_UTILITY_H
#define DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_PREDICATE_PARSER_UTILITY_H

#include <string>
#include <unordered_map>
#include <vector>

namespace dftracer::utils::utilities::composites::dft::indexing {

// Output type: dimension -> values (OR within dimension, AND across dimensions)
using PredicateMap = std::unordered_map<std::string, std::vector<std::string>>;

struct PredicateParserInput {
    // Predicate strings like "cat=POSIX,name=read|write"
    std::vector<std::string> predicate_strings;
    // Pre-existing predicates (e.g. from --filter-name, --filter-cat)
    PredicateMap existing_predicates;

    PredicateParserInput& with_predicate_string(const std::string& s) {
        predicate_strings.push_back(s);
        return *this;
    }

    PredicateParserInput& with_predicate_strings(
        const std::vector<std::string>& strs) {
        predicate_strings.insert(predicate_strings.end(), strs.begin(),
                                 strs.end());
        return *this;
    }

    PredicateParserInput& with_existing(
        const std::string& dimension, const std::vector<std::string>& values) {
        auto& v = existing_predicates[dimension];
        v.insert(v.end(), values.begin(), values.end());
        return *this;
    }
};

struct PredicateParserOutput {
    PredicateMap predicates;
    bool success = false;
};

class PredicateParserUtility {
   public:
    PredicateParserUtility() = default;

    PredicateParserOutput process(const PredicateParserInput& input) {
        PredicateParserOutput output;
        output.predicates = input.existing_predicates;

        for (const auto& pred_str : input.predicate_strings) {
            parse_into(pred_str, output.predicates);
        }

        output.success = true;
        return output;
    }

   private:
    static void parse_into(const std::string& pred_str, PredicateMap& target) {
        // Split by comma for different dimensions
        std::string::size_type pos = 0;
        while (pos < pred_str.size()) {
            auto comma = pred_str.find(',', pos);
            std::string token = pred_str.substr(pos, comma == std::string::npos
                                                         ? std::string::npos
                                                         : comma - pos);
            pos = (comma == std::string::npos) ? pred_str.size() : comma + 1;

            // Split by '=' for dim=values
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;

            std::string dim = token.substr(0, eq);
            std::string values_str = token.substr(eq + 1);

            // Split values by '|' for OR within dimension
            std::vector<std::string> values;
            std::string::size_type vpos = 0;
            while (vpos < values_str.size()) {
                auto pipe = values_str.find('|', vpos);
                values.push_back(values_str.substr(
                    vpos, pipe == std::string::npos ? std::string::npos
                                                    : pipe - vpos));
                vpos =
                    (pipe == std::string::npos) ? values_str.size() : pipe + 1;
            }

            // Merge into target (append to existing dimension values)
            auto& existing = target[dim];
            existing.insert(existing.end(), values.begin(), values.end());
        }
    }
};

}  // namespace dftracer::utils::utilities::composites::dft::indexing

#endif  // DFTRACER_UTILS_UTILITIES_COMPOSITES_DFT_INDEXING_PREDICATE_PARSER_UTILITY_H
