#pragma once

#include <torch/csrc/jit/passes/memory_planning.h>

namespace torch {
namespace jit {

std::vector<MemAllocation> greedyByOperatorBreadth(
    std::unordered_map<const Value*, int64_t> managed_tensor_sizes,
    LiveRangesMap live_ranges,
    std::vector<const Node*> ops);

} // namespace jit
} // namespace torch