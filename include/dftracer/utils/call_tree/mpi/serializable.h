#ifndef DFTRACER_UTILS_CALL_TREE_MPI_SERIALIZABLE_H
#define DFTRACER_UTILS_CALL_TREE_MPI_SERIALIZABLE_H

// Two save/load formats for in-memory call trees:
//
//   save_binary / load_binary  -- compact custom format with a string
//     dictionary (name/category/arg keys/string values share storage) and
//     typed args (preserves int/uint/double/bool vs flattening to strings).
//     Header is fixed-size; body lays out a global string table followed
//     by ProcessCallTree records.
//
//   save_arrow / load_arrow    -- Arrow IPC (.arrow) with zstd buffer-level
//     compression. Columnar layout with dictionary-encoded name/category;
//     readable by pyarrow / polars / nanoarrow. Best for analysis tooling
//     that already speaks Arrow.

#include <dftracer/utils/call_tree/internal/call_tree.h>
#include <dftracer/utils/core/coro/task.h>
#include <dftracer/utils/core/tasks/coro_scope.h>

#include <cstdint>
#include <memory>
#include <string>

namespace dftracer::utils::call_tree {

inline constexpr char CALLTREE_BINARY_MAGIC[8] = {'D', 'F', 'T', 'C',
                                                  'G', 'R', 'P', '2'};
inline constexpr std::uint32_t CALLTREE_BINARY_VERSION = 2;

coro::CoroTask<bool> save_binary(CoroScope* scope,
                                 const internal::CallTree& tree,
                                 std::string output_path);
coro::CoroTask<std::unique_ptr<internal::CallTree>> load_binary(
    CoroScope* scope, std::string input_path);

coro::CoroTask<bool> save_arrow(CoroScope* scope,
                                const internal::CallTree& tree,
                                std::string output_path);
coro::CoroTask<std::unique_ptr<internal::CallTree>> load_arrow(
    CoroScope* scope, std::string input_path);

}  // namespace dftracer::utils::call_tree

#endif  // DFTRACER_UTILS_CALL_TREE_MPI_SERIALIZABLE_H
