#include <torch/csrc/jit/passes/memory_planning.h>
#include <torch/csrc/jit/passes/memory_planning/linear_scan.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_size.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_breadth.h>

#include <jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/runtime/static/ops.h>
#include <limits>

namespace torch {
namespace jit {

bool valid_add(int64_t a, int64_t b) {
  static constexpr int64_t max = std::numeric_limits<int64_t>::max();
  static constexpr int64_t min = std::numeric_limits<int64_t>::min();

  if (((b > 0) && (a > max - b)) || ((b < 0) && (a < min - b)))
    return false;
  return true;
}

bool valid_sub(int64_t a, int64_t b) {
  static constexpr int64_t max = std::numeric_limits<int64_t>::max();
  static constexpr int64_t min = std::numeric_limits<int64_t>::min();

  if (((b < 0) && (a > max + b)) || ((b > 0) && (a < min + b)))
    return false;
  return true;
}

int intersectArea(int64_t a, int64_t b, int64_t c, int64_t d) {
  TORCH_INTERNAL_ASSERT(a <= b);
  TORCH_INTERNAL_ASSERT(c <= d);
  int64_t outer = std::max(b, d) - std::min(a, c);
  int64_t l1 = (b - a), l2 = (d - c);

  if (!valid_add(l1, l2)) {
    // sum areas larger than possible outer area (thus overlap)
    return -1;
  } else if (!valid_sub(outer, l1 + l2)) {
    // multipoint overlap (sum areas larger than outer area)
    return -1;
  } else if (outer - (l1 + l2) > 0) {
    // outer area larger than sum (no overlap)
    return 1;
  } else {
    // outer area equals sum area (single point overlap)
    return 0;
  }
}

bool intersectLiveRange(LiveRange lvr1, LiveRange lvr2) {
  return intersectArea(lvr1.begin, lvr1.end, lvr2.begin, lvr2.end) <= 0;
}

bool intersectMemRegion(MemRegion reg1, MemRegion reg2) {
  // greater than 1 point overlap
  return intersectArea(
             reg1.offset,
             reg1.offset + reg1.size,
             reg2.offset,
             reg2.offset + reg2.size) < 0;
}

std::vector<MemAllocation> naive(
    std::unordered_map<LiveRange, int64_t, live_range_hash>
        managed_live_ranges) {
  std::map<LiveRange, int64_t, live_range_start_cmp> sorted_managed_live_ranges(
      managed_live_ranges.begin(), managed_live_ranges.end());
  std::vector<MemAllocation> allocations;
  allocations.reserve(managed_live_ranges.size());
  int64_t offset = 0;
  for (const auto& item : sorted_managed_live_ranges) {
    auto aligned_size = MemoryPlanner::computeAlignedTensorSize(item.second);
    allocations.push_back({item.first, {offset, aligned_size}});
    offset += aligned_size;
  }
  return allocations;
}

c10::optional<size_t> computeStorageSize(const Value& value) {
  auto ttp = value.type()->cast<TensorType>();
  if (!ttp) {
    TORCH_WARN("out isn't a tensortype ", *value.type());
    return c10::nullopt;
  }
  if (!ttp->scalarType().has_value()) {
    TORCH_WARN(
        "This output was profiled but didn't have a scalar type: ",
        *ttp,
        ", ",
        value.debugName());
    return c10::nullopt;
  }
  if (!ttp->sizes().concrete_sizes().has_value()) {
    TORCH_WARN(
        "This output was profiled but doesn't have sizes: ",
        *ttp,
        ", ",
        value.debugName());
    return c10::nullopt;
  }

  auto scalar_type = ttp->scalarType();
  if (!scalar_type.has_value()) {
    TORCH_WARN(
        "This value doesn't have a scalar type", *ttp, ", ", value.debugName());
    return c10::nullopt;
  }

  auto element_size = c10::elementSize(scalar_type.value());
  // TODO: when does this fail? answer: in place mutation
  auto numel = ttp->numel();
  if (!numel.has_value()) {
    TORCH_WARN("doesn't have numel", *ttp, ", ", value.debugName());
    return c10::nullopt;
  }

  return numel.value() * element_size;
}

std::pair<std::vector<int64_t>, std::vector<int64_t>> getSizesStrides(
    const c10::TensorTypePtr& ttp) {
  std::vector<int64_t> sizes;
  auto _sizes = ttp->sizes().concrete_sizes();
  // TODO: why does this break? answer: in place mutation
  // also %9 : Long(requires_grad=0, device=cpu) = prim::Constant[value={0}]()
  if (_sizes.has_value() && _sizes.value().size() > 0 &&
      _sizes.value()[0] != 0) {
    sizes = _sizes.value();
  } else {
    sizes = std::vector<int64_t>{0};
  }
  std::vector<int64_t> strides;
  auto _strides = ttp->strides().concrete_sizes();
  if (_strides.has_value() && _strides.value().size() > 0 &&
      _strides.value()[0] != 0) {
    strides = _strides.value();
  } else {
    strides = at::detail::defaultStrides(sizes);
  }
  return std::make_pair(sizes, strides);
}

Node* insertAllocStorageNode(
    std::shared_ptr<Graph>& graph,
    int64_t total_size) {
  auto* storage = graph->create(prim::AllocateStorage, 1);
  storage->i_(attr::total_size, total_size);

  auto device_type = jit::tensorexpr::pickDeviceType(graph);
  if (device_type.has_value()) {
    storage->i_(attr::device, static_cast<int8_t>(device_type.value().type()));
  } else {
    storage->i_(attr::device, static_cast<int8_t>(at::kCPU));
  }
  storage->insertBefore(graph->nodes().front());
  return storage;
}

void insertAllocTensorNodes(
    std::shared_ptr<Graph>& graph,
    Node* storage,
    std::vector<MemAllocation> allocations,
    std::map<LiveRange, const Value*, live_range_start_cmp>
        managed_range_values) {
  std::unordered_map<LiveRange, MemRegion, live_range_hash> allocations_map;
  allocations_map.reserve(allocations.size());
  for (const auto& item : allocations) {
    allocations_map[item.lvr] = item.reg;
  }

  int64_t total_size = storage->i(attr::total_size);
  for (auto& item : managed_range_values) {
    auto lvr = item.first;
    auto region = allocations_map[lvr];
    auto allocation = item.second;

    // const_cast fishy?
    auto node = const_cast<Node*>(allocation->node());

    // the way that this node magically *becomes* the out varaint is simply
    // by add an extra input. this is because op resolution happens
    // at runtime via the op registry (by matching on the schema).
    auto* alloc = graph->create(prim::AllocateTensor, 1);
    node->addInput(alloc->output());
    GRAPH_DEBUG("inserting allocation op for ", node->getOperator().schema());
    alloc->insertBefore(node);
    alloc->addInput(storage->output());

    auto ttp = allocation->type()->expect<c10::TensorType>();
    std::vector<int64_t> sizes, strides;
    std::tie(sizes, strides) = getSizesStrides(ttp);
    TORCH_CHECK(
        region.offset + region.size <= total_size,
        "trying to create an allocation that exceeds previously planned memory");
    alloc->i_(attr::size, region.size);
    alloc->i_(attr::offset, region.offset);
    alloc->is_(attr::sizes, sizes);
    alloc->is_(attr::stride, strides);
    alloc->i_(attr::device, static_cast<int8_t>(storage->i(attr::device)));
    alloc->i_(attr::dtype, static_cast<int8_t>(ttp->scalarType().value()));
  }
}

std::vector<Node*> insertPreAllocTensorNodes(
    std::shared_ptr<Graph>& graph,
    Node* storage,
    std::vector<MemAllocation> allocations,
    std::vector<std::pair<FrameNodeId, std::vector<LiveRange>>>
        collected_node_live_ranges) {
  std::unordered_map<LiveRange, MemRegion, live_range_hash> allocations_map;
  allocations_map.reserve(allocations.size());
  for (const auto& item : allocations) {
    allocations_map[item.lvr] = item.reg;
  }

  std::sort(
      collected_node_live_ranges.begin(),
      collected_node_live_ranges.end(),
      frame_node_id_cmp());

  std::vector<Node*> inserted_alloc_nodes;
  for (auto& item : collected_node_live_ranges) {
    auto frame_id = item.first;
    auto lvrs = item.second;
    std::sort(lvrs.begin(), lvrs.end(), live_range_start_cmp());
    auto node = frame_id.node;

    for (const auto& lvr : lvrs) {
      auto region = allocations_map[lvr];
      auto* alloc = graph->create(prim::PreAllocateTensor, 1);
      inserted_alloc_nodes.emplace_back(alloc);
      GRAPH_DEBUG(
          "inserting preallocation op for ",
          getHeader(node),
          " ",
          std::addressof(*node),
          " with size ",
          region.size);
      alloc->insertBefore(node);
      alloc->addInput(storage->output());

      alloc->i_(attr::size, region.size);
      alloc->i_(attr::offset, region.offset);
      alloc->i_(attr::device, storage->i(attr::device));
    }
  }
  return inserted_alloc_nodes;
}

bool hasOutVariant(Node* node) {
  for (const auto& variant : getAllOperatorsFor(node->kind())) {
    auto variant_args = variant->schema().arguments();
    /* TODO
      aten::cat.names_out(Tensor[] tensors, str dim, *, Tensor(a!) out) ->
      (Tensor(a!)) aten::cat.out(Tensor[] tensors, int dim=0, *,
      Tensor(a!) out) -> (Tensor(a!))
    */
    auto maybe_out_arg =
        std::find_if(variant_args.begin(), variant_args.end(), [](auto arg) {
          return arg.name() == "out";
        });
    if (maybe_out_arg != variant_args.end()) {
      return true;
    }
  }
  return false;
}

std::pair<std::vector<const Node*>, std::unordered_map<const Value*, int64_t>>
getManagedValues(
    const std::shared_ptr<Graph>& graph,
    std::unordered_set<const Value*> always_alive_values) {
  std::unordered_map<const Value*, int64_t> managed_tensor_values;
  std::unordered_set<const Value*> leaked_values;
  std::vector<const Node*> out_nodes;

  FastMap<Node*, bool> node_has_out_variant;
  for (auto* node : graph->nodes()) {
    node_has_out_variant.insert({node, hasOutVariant(node)});
  }

  for (auto node : graph->nodes()) {
    if (!node_has_out_variant[node]) {
      continue;
    }
    out_nodes.emplace_back(node);
    for (const auto* out_v : node->outputs()) {
      if (always_alive_values.count(out_v)) {
        continue;
      }
      auto size = computeStorageSize(*out_v);
      if (size.has_value() && size.value() > 0) {
        managed_tensor_values.insert({out_v, size.value()});
      } else if (isOptimizableContainerType(node, node_has_out_variant)) {
        leaked_values.insert(out_v);
      } else {
        TORCH_WARN(
            "not handling unsupported value: ",
            out_v->debugName(),
            " ",
            *out_v->type());
        leaked_values.insert(out_v);
      }
    }
  }
  GRAPH_DEBUG("memory planning leaked values: ", c10::Join(",", leaked_values));
  return std::make_pair(out_nodes, managed_tensor_values);
}

std::tuple<
    std::vector<const Node*>,
    std::unordered_map<const Value*, int64_t>,
    std::unordered_map<const Value*, LiveRange>>
getManagedStuff(std::shared_ptr<Graph>& graph) {
  AliasDb alias_db(graph);
  auto always_alive = jit::GetAlwaysAliveValues(graph, alias_db);
  auto live_ranges = jit::GetLiveness(graph, always_alive, alias_db).second;
  std::vector<const Node*> out_nodes;
  std::unordered_map<const Value*, int64_t> managed_tensor_values;
  std::tie(out_nodes, managed_tensor_values) =
      getManagedValues(graph, always_alive);

  std::unordered_map<const Value*, LiveRange> managed_ranges;
  for (const auto& lvr : live_ranges) {
    if (managed_tensor_values.count(lvr.first) > 0) {
      managed_ranges.insert(lvr);
    }
  }
  return std::make_tuple(out_nodes, managed_tensor_values, managed_ranges);
}

int64_t getTotalAllocationSize(std::vector<MemAllocation> allocations) {
  int64_t total_size = 0;
  for (const auto& alloc : allocations) {
    total_size = std::max(total_size, alloc.reg.offset + alloc.reg.size);
  }
  return total_size;
}

bool intersectAllocs(MemAllocation m1, MemAllocation m2) {
  return intersectLiveRange(m1.lvr, m2.lvr) &&
         intersectMemRegion(m1.reg, m2.reg);
}

bool validateAllocations(std::vector<MemAllocation> allocations) {
  for (const auto& alloc1 : allocations) {
    for (const auto& alloc2 : allocations) {
      if (alloc1 == alloc2) {
        continue;
      }
      if (intersectAllocs(alloc1, alloc2)) {
        std::cerr << alloc1 << "," << alloc2 << "\n";
        return false;
      }
    }
  }
  return true;
}

std::ostream& printAllocation(
    std::ostream& out,
    std::vector<MemAllocation> allocations,
    std::map<LiveRange, const Value*, live_range_start_cmp> managed_ranges) {
  std::map<LiveRange, MemRegion, live_range_start_cmp> allocations_map;
  for (const auto& item : allocations) {
    allocations_map[item.lvr] = item.reg;
  }

  for (const auto& item : managed_ranges) {
    auto lvr = item.first;
    auto val = item.second;
    auto alloced_reg = allocations_map[lvr];
    out << val->debugName() << ": " << lvr << " " << alloced_reg << "\n";
  }
  return out;
}

std::vector<std::pair<FrameNodeId, std::vector<LiveRange>>>
collectLiveRangesPerNode(
    std::vector<std::pair<LiveRange, FrameNodeId>> live_range_node_header) {
  std::unordered_map<FrameNodeId, std::vector<LiveRange>, frame_node_id_hash>
      node_live_ranges;

  for (const auto& item : live_range_node_header) {
    auto lvr = item.first;
    auto frame_node_id = item.second;
    node_live_ranges[frame_node_id].emplace_back(lvr);
  }

  std::vector<std::pair<FrameNodeId, std::vector<LiveRange>>>
      collected_node_live_ranges;
  for (const auto& item : node_live_ranges) {
    std::vector<LiveRange> lvrs(item.second.begin(), item.second.end());
    std::sort(lvrs.begin(), lvrs.end(), live_range_start_cmp());
    collected_node_live_ranges.emplace_back(std::make_pair(item.first, lvrs));
  }
  std::sort(
      collected_node_live_ranges.begin(),
      collected_node_live_ranges.end(),
      frame_node_id_cmp());
  return collected_node_live_ranges;
}

std::pair<
    std::unordered_map<LiveRange, int64_t, live_range_hash>,
    std::vector<std::pair<LiveRange, FrameNodeId>>>
getManagedLiveRangesFromMemEvents(
    std::vector<MemEvent> mem_events,
    const std::shared_ptr<Graph> graph) {
  std::unordered_map<LiveRange, int64_t, live_range_hash> managed_live_ranges;
  std::vector<std::pair<LiveRange, FrameNodeId>> live_range_node_header;
  live_range_node_header.reserve(mem_events.size());

  std::unordered_map<std::string, MemEvent> allocs;
  // validate
  for (auto& mem_event : mem_events) {
    if (mem_event.type == MemEvent::EventType::Allocate) {
      if (mem_event.frame_node_id.has_value()) {
        allocs.insert({mem_event.ptr_addr, mem_event});
      } else {
        // created before interpreter started e.g. inputs and weights...
        TORCH_INTERNAL_ASSERT(mem_event.time == 0);
      }
    } else if (mem_event.type == MemEvent::EventType::Free) {
      TORCH_INTERNAL_ASSERT(allocs.count(mem_event.ptr_addr) > 0);
      TORCH_INTERNAL_ASSERT(allocs.find(mem_event.ptr_addr) != allocs.end());
      auto alloc = allocs.at(mem_event.ptr_addr);
      TORCH_INTERNAL_ASSERT(
          alloc.type == MemEvent::EventType::Allocate,
          " ",
          alloc.type,
          " ",
          MemEvent::EventType::Allocate);
      TORCH_INTERNAL_ASSERT(
          alloc.size == mem_event.size, " ", alloc.size, " ", mem_event.size);
      TORCH_INTERNAL_ASSERT(
          alloc.time < mem_event.time, " ", alloc.time, " ", mem_event.time);

      auto lvr = LiveRange{alloc.time, mem_event.time};
      managed_live_ranges.insert({lvr, alloc.size});

      live_range_node_header.emplace_back(
          std::make_pair(lvr, alloc.frame_node_id.value()));
      allocs.erase(mem_event.ptr_addr);
    }
  }

  if (!allocs.empty()) {
    auto g_outputs = std::unordered_set<const jit::Value*>(
        graph->outputs().begin(), graph->outputs().end());
    for (auto& alloc : allocs) {
      TORCH_INTERNAL_ASSERT(
          alloc.second.type == MemEvent::EventType::Allocate &&
          alloc.second.frame_node_id.has_value());
      GRAPH_DEBUG("leaked alloc: ", alloc.second, "\n");
      if (alloc.second.frame_node_id.value().node->outputs().size() > 0) {
        for (const auto& out :
             alloc.second.frame_node_id.value().node->outputs()) {
          // TODO: this is a very bad heuristic (that this temp tensor is output
          // because no outputs). should find some way to connect value/ivalue
          // to alloc
          TORCH_INTERNAL_ASSERT(g_outputs.count(out) > 0);
        }
      }
    }
  }
  return std::make_pair(managed_live_ranges, live_range_node_header);
}

void insertCollectAllocatedTensorsNode(
    std::shared_ptr<Graph>& graph,
    std::vector<Node*> alloc_nodes) {
  auto* collect_node = graph->create(prim::Constant, 1);
  collect_node->insertBefore(graph->return_node());
  for (auto& node : alloc_nodes) {
    collect_node->addInput(node->output());
  }
}

void planMemoryWithTracing(
    std::shared_ptr<Graph>& graph,
    Strategy strat,
    std::vector<MemEvent> mem_events,
    c10::optional<at::Device> device_type) {
  TORCH_INTERNAL_ASSERT(!mem_events.empty());
  std::unordered_map<LiveRange, int64_t, live_range_hash> managed_live_ranges;
  std::vector<std::pair<LiveRange, FrameNodeId>> live_range_node_header;
  std::tie(managed_live_ranges, live_range_node_header) =
      getManagedLiveRangesFromMemEvents(mem_events, graph);
  std::vector<MemAllocation> allocations;

  switch (strat) {
    case Strategy::NAIVE: {
      allocations = naive(managed_live_ranges);
      break;
    }
    case Strategy::LINEAR_SCAN: {
      allocations = linearScanHeuristic(managed_live_ranges);
      break;
    };
    case Strategy::GREEDY_BY_SIZE: {
      allocations = greedyBySize(managed_live_ranges);
      break;
    }
    default:
      return;
  }
  GRAPH_DEBUG("\nnumber of allocations\n", allocations.size());
  auto total_size = getTotalAllocationSize(allocations);
  GRAPH_DEBUG("\ngraph before inserting storage node\n", *graph);
  auto storage_node = insertAllocStorageNode(graph, total_size);
  GRAPH_DEBUG("\ngraph after inserting storage node\n", *graph);

  auto collected_node_live_ranges =
      collectLiveRangesPerNode(live_range_node_header);

  auto inserted_alloc_nodes = insertPreAllocTensorNodes(
      graph, storage_node, allocations, collected_node_live_ranges);
  GRAPH_DEBUG("\ngraph after inserting prealloc nodes\n", *graph);
  // otherwise
  insertCollectAllocatedTensorsNode(graph, inserted_alloc_nodes);
  GRAPH_DEBUG("\ngraph after inserting collect node\n", *graph);
}

void planMemory(std::shared_ptr<Graph>& graph, Strategy strat) {
  std::unordered_map<const Value*, int64_t> managed_value_sizes;
  std::unordered_map<const Value*, LiveRange> managed_value_ranges;
  std::vector<const Node*> out_nodes;
  std::tie(out_nodes, managed_value_sizes, managed_value_ranges) =
      getManagedStuff(graph);

  std::unordered_map<LiveRange, int64_t, live_range_hash> managed_live_ranges;
  managed_live_ranges.reserve(managed_value_sizes.size());
  for (const auto& item : managed_value_sizes) {
    managed_live_ranges[managed_value_ranges[item.first]] = item.second;
  }
  std::vector<MemAllocation> allocations;

  switch (strat) {
    case Strategy::NAIVE: {
      allocations = naive(managed_live_ranges);
      break;
    }
    case Strategy::LINEAR_SCAN: {
      allocations = linearScanHeuristic(managed_live_ranges);
      break;
    };
    case Strategy::GREEDY_BY_SIZE: {
      allocations = greedyBySize(managed_live_ranges);
      break;
    }
    case Strategy::GREEDY_BY_SIZE_WITH_FIRST_GAP: {
      allocations = greedyBySizeWithFirstGap(managed_live_ranges);
      break;
    }
    case Strategy::GREEDY_BY_LONGEST_AND_SIZE: {
      allocations = greedyBySizeAndLongestWithFirstGap(managed_live_ranges);
      break;
    }
    case Strategy::GREEDY_BY_BREADTH: {
      allocations = greedyByOperatorBreadth(
          managed_value_sizes, managed_value_ranges, out_nodes);
      break;
    }
    default:
      return;
  }

  TORCH_INTERNAL_ASSERT(
      validateAllocations(allocations), "invalid allocation", strat);

  auto total_size = getTotalAllocationSize(allocations);

  std::map<LiveRange, const Value*, live_range_start_cmp> managed_range_values;
  for (const auto& item : managed_value_ranges) {
    if (managed_range_values.count(item.second)) {
      TORCH_WARN(
          "overlapping live ranges ",
          item.first->debugName(),
          " with ",
          managed_range_values.at(item.second)->debugName());
    }
    managed_range_values.insert({item.second, item.first});
  }

  std::stringstream allocs_str;
  printAllocation(allocs_str, allocations, managed_range_values);
  GRAPH_DEBUG("\nallocs\n", allocs_str.str());

  GRAPH_DEBUG("\ngraph before inserting storage node\n", *graph);

  auto storage_node = insertAllocStorageNode(graph, total_size);
  GRAPH_DEBUG("\ngraph after inserting storage node\n", *graph);

  insertAllocTensorNodes(
      graph, storage_node, allocations, managed_range_values);
  GRAPH_DEBUG("\ngraph after inserting alloc nodes\n", *graph);
}
} // namespace jit
} // namespace torch
