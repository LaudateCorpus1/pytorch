#include <torch/csrc/jit/passes/memory_planning.h>
#include <torch/csrc/jit/passes/memory_planning/MemoryPlanningAllocator.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_breadth.h>
#include <torch/csrc/jit/passes/memory_planning/greedy_by_size.h>
#include <torch/csrc/jit/passes/memory_planning/linear_scan.h>

#include <regex>

#include <aten/src/ATen/core/interned_strings.h>
#include <c10/util/Backtrace.h>
#include <jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/ir/alias_analysis.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/runtime/operator.h>
#include <torch/csrc/jit/runtime/static/ops.h>

namespace torch {
namespace jit {

c10::optional<uint64_t> computeStorageSize(const Value& value) {
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
    uint64_t total_size) {
  auto* storage = graph->create(prim::AllocateStorage, 1);
  storage->i_(attr::total_size, total_size);

  auto deviceType = jit::tensorexpr::pickDeviceType(graph);
  if (deviceType.has_value()) {
    storage->i_(attr::device, static_cast<int8_t>(deviceType.value().type()));
  } else {
    storage->i_(attr::device, static_cast<int8_t>(at::kCPU));
  }
  storage->insertBefore(graph->nodes().front());
  return storage;
}

void insertAllocTensorNodes(
    std::shared_ptr<Graph>& graph,
    Node* storage,
    std::unordered_map<LiveRange, Region, live_range_hash> allocations,
    std::map<LiveRange, const Value*, live_range_start_cmp>
        manage_range_values) {
  uint64_t total_size = storage->i(attr::total_size);
  for (auto& item : manage_range_values) {
    auto lvr = item.first;
    auto region = allocations[lvr];
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

void insertPreAllocTensorNodes(
    std::shared_ptr<Graph>& graph,
    Node* storage,
    std::unordered_map<LiveRange, Region, live_range_hash> allocations,
    std::vector<std::pair<FrameNodeId, std::vector<LiveRange>>>
        collected_node_live_ranges) {
  std::sort(
      collected_node_live_ranges.begin(),
      collected_node_live_ranges.end(),
      frame_node_id_cmp());

  //  uint64_t total_size = storage->i(attr::total_size);
  auto node = graph->nodes().begin();

  for (auto& item : collected_node_live_ranges) {
    auto frame_id = item.first;
    auto lvrs = item.second;
    std::sort(lvrs.begin(), lvrs.end(), live_range_start_cmp());
    while (!getHeader(*node).compare(frame_id.node_header)) {
      node++;
    }
    TORCH_INTERNAL_ASSERT(
        canonicalSchemaString(node->schema()).compare(frame_id.node_header));

    for (const auto& lvr : lvrs) {
      auto region = allocations[lvr];
      auto* alloc = graph->create(prim::PreAllocateTensor, 0);
      GRAPH_DEBUG(
          "inserting allocation op for ",
          getHeader(*node),
          "with size ",
          region.size);
      alloc->insertBefore(*node);
      alloc->i_(attr::size, region.size);
      alloc->i_(attr::offset, region.offset);
    }
  }
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

std::pair<std::vector<const Node*>, std::unordered_map<const Value*, uint64_t>>
getManagedValues(
    const std::shared_ptr<Graph>& graph,
    std::unordered_set<const Value*> always_alive_values) {
  std::unordered_map<const Value*, uint64_t> managed_tensor_values;
  std::unordered_set<const Value*> leaked_values;
  std::vector<const Node*> out_nodes;

  for (auto node : graph->nodes()) {
    if (!hasOutVariant(node)) {
      continue;
    }
    out_nodes.emplace_back(node);
    for (const auto& out_v : node->outputs()) {
      if (always_alive_values.count(out_v)) {
        continue;
      }
      auto size = computeStorageSize(*out_v);
      if (size.has_value() && size.value() > 0) {
        managed_tensor_values.insert({out_v, size.value()});
      } else if (isOptimizableContainerType(node)) {
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
  return std::make_pair(out_nodes, managed_tensor_values);
}

std::tuple<
    std::vector<const Node*>,
    std::unordered_map<const Value*, uint64_t>,
    std::unordered_map<const Value*, LiveRange>>
getManagedStuff(std::shared_ptr<Graph>& graph) {
  AliasDb alias_db(graph);
  auto always_alive = jit::GetAlwaysAliveValues(graph, alias_db);
  auto live_ranges = jit::GetLiveness(graph, always_alive, alias_db).second;
  std::vector<const Node*> out_nodes;
  std::unordered_map<const Value*, uint64_t> managed_tensor_values;
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

uint64_t getTotalAllocationSize(
    std::unordered_map<LiveRange, Region, live_range_hash> allocations) {
  uint64_t total_size = 0;
  for (const auto& item : allocations) {
    total_size = std::max(total_size, item.second.offset + item.second.size);
  }
  return total_size;
}

void printAllocation(
    std::unordered_map<LiveRange, Region, live_range_hash> allocations,
    std::map<LiveRange, const Value*, live_range_start_cmp> managed_ranges) {
  for (const auto& item : managed_ranges) {
    auto lvr = item.first;
    auto val = item.second;
    auto alloced_reg = allocations[lvr];
    std::cout << val->debugName() << ": " << lvr << " " << alloced_reg << "\n";
  }
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
    collected_node_live_ranges.emplace_back(
        std::make_pair(item.first, lvrs));
  }
  std::sort(
      collected_node_live_ranges.begin(),
      collected_node_live_ranges.end(),
      frame_node_id_cmp());
  return collected_node_live_ranges;
}

std::pair<
    std::unordered_map<LiveRange, uint64_t, live_range_hash>,
    std::vector<std::pair<LiveRange, FrameNodeId>>>
getLiveRangesFromMemEvents(std::vector<MemEvent> mem_events) {
  std::unordered_map<LiveRange, uint64_t, live_range_hash> managed_live_ranges;
  std::vector<std::pair<LiveRange, FrameNodeId>> live_range_node_header;

  std::unordered_map<std::string, MemEvent> allocs;
  // validate
  for (auto& mem_event : mem_events) {
    if (mem_event.type == MemEvent::EventType::Allocate) {
      allocs.insert({mem_event.ptr_addr, mem_event});
    } else if (mem_event.type == MemEvent::EventType::Free) {
      TORCH_INTERNAL_ASSERT(allocs.count(mem_event.ptr_addr) > 0);
      auto alloc = allocs.at(mem_event.ptr_addr);
      TORCH_INTERNAL_ASSERT(
          alloc.type == MemEvent::EventType::Allocate &&
              alloc.size == mem_event.size && alloc.time < mem_event.time &&
              alloc.node_schema == mem_event.node_schema,
          alloc.node_header == mem_event.node_header);

      auto lvr = LiveRange{alloc.time, mem_event.time};
      managed_live_ranges.insert({lvr, alloc.size});
      live_range_node_header.emplace_back(std::make_tuple(
          lvr, FrameNodeId{alloc.time, alloc.node_schema, alloc.node_header}));
    }
  }
  TORCH_INTERNAL_ASSERT(allocs.empty());
  return std::make_pair(managed_live_ranges, live_range_node_header);
}

void planMemoryWithTracing(
    std::shared_ptr<Graph>& graph,
    Strategy strat,
    std::vector<MemEvent> mem_events) {
  TORCH_INTERNAL_ASSERT(!mem_events.empty());
  std::unordered_map<LiveRange, uint64_t, live_range_hash> managed_live_ranges;
  std::vector<std::pair<LiveRange, FrameNodeId>> live_range_node_header;
  std::tie(managed_live_ranges, live_range_node_header) =
      getLiveRangesFromMemEvents(mem_events);

  auto allocations = greedyBySize(managed_live_ranges);

  switch (strat) {
    case Strategy::NAIVE: {
      return;
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

  auto total_size = getTotalAllocationSize(allocations);

  GRAPH_DEBUG("\ngraph before inserting storage node\n", *graph);

  auto storage_node = insertAllocStorageNode(graph, total_size);
  GRAPH_DEBUG("\ngraph after inserting storage node\n", *graph);

  auto collected_node_live_ranges =
      collectLiveRangesPerNode(live_range_node_header);
  insertPreAllocTensorNodes(
      graph, storage_node, allocations, collected_node_live_ranges);
  GRAPH_DEBUG("\ngraph after inserting alloc nodes\n", *graph);
}

void planMemory(std::shared_ptr<Graph>& graph, Strategy strat) {
  std::unordered_map<const Value*, uint64_t> managed_value_sizes;
  std::unordered_map<const Value*, LiveRange> managed_value_ranges;
  std::vector<const Node*> out_nodes;
  std::tie(out_nodes, managed_value_sizes, managed_value_ranges) =
      getManagedStuff(graph);

  std::unordered_map<LiveRange, uint64_t, live_range_hash> managed_live_ranges;
  for (const auto& item : managed_value_sizes) {
    managed_live_ranges[managed_value_ranges[item.first]] = item.second;
  }
  std::unordered_map<LiveRange, Region, live_range_hash> allocations;

  switch (strat) {
    case Strategy::NAIVE: {
      return;
    }
    case Strategy::LINEAR_SCAN: {
      allocations = linearScanHeuristic(managed_live_ranges);
      break;
    };
    case Strategy::GREEDY_BY_SIZE: {
      allocations = greedyBySize(managed_live_ranges);
      break;
    }
    case Strategy::GREEDY_BY_BREADTH: {
      allocations = greedyByOperatorBreadth(
          managed_value_sizes, managed_value_ranges, out_nodes);
      break;
    };
    default:
      return;
  }

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

  printAllocation(allocations, managed_range_values);

  GRAPH_DEBUG("\ngraph before inserting storage node\n", *graph);

  auto storage_node = insertAllocStorageNode(graph, total_size);
  GRAPH_DEBUG("\ngraph after inserting storage node\n", *graph);

  insertAllocTensorNodes(
      graph, storage_node, allocations, managed_range_values);
  GRAPH_DEBUG("\ngraph after inserting alloc nodes\n", *graph);
}
} // namespace jit
} // namespace torch
