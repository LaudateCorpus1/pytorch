#include <torch/csrc/jit/passes/memory_planning/MemoryPlanningAllocator.h>

#include <sstream> //for std::stringstream

#include <c10/util/Backtrace.h>
#include <torch/csrc/jit/mobile/interpreter.h>

static void DoNothing(void* ptr) {
  return;
}

inline int64_t timeSinceEpoch(
    const std::chrono::time_point<std::chrono::system_clock>& t) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             t.time_since_epoch())
      .count();
}

MemoryPlanningAllocator::MemoryPlanningAllocator(at::DeviceType& device_type)
    : device_type_(device_type),
      orig_allocator_(*c10::GetAllocator(device_type)) {
  c10::SetAllocator(device_type, this);
};

at::DataPtr MemoryPlanningAllocator::allocate(size_t nbytes) const {
  auto alloc = allocs_.top();
  allocs_.pop();
  auto size = alloc.first;
  TORCH_CHECK(size == nbytes);
  auto data = alloc.second;
  return {data, data, &DoNothing, at::Device(device_type_)};
}

at::DeleterFnPtr MemoryPlanningAllocator::raw_deleter() const {
  return Allocator::raw_deleter();
}

void MemoryPlanningAllocator::push_allocation(
    c10::Storage buffer,
    size_t size,
    size_t offset,
    at::DeviceType device_type) {
  TORCH_CHECK(device_type == device_type_);
  uint8_t* start = static_cast<uint8_t*>(buffer.data());
  void* src = static_cast<void*>(start + offset);
  allocs_.push(std::make_pair(size, src));
}

at::DeleterFnPtr raw_deleter() {
  return &DoNothing;
}

std::string dataPtrAddrToStr(void* ptr) {
  std::stringstream ss;
  ss << ptr;
  return ss.str();
}

struct MemoryTracingAllocator final : at::Allocator {
  MemoryTracingAllocator(at::DeviceType& device_type)
      : orig_allocator_(*c10::GetAllocator(device_type)) {
    c10::SetAllocator(device_type, this);
  }

  at::DataPtr allocate(size_t nbytes) const override {
    auto orig_ptr = orig_allocator_.allocate(nbytes);

    auto bt = c10::get_backtrace(0, 200, true);
    TORCH_INTERNAL_ASSERT(torch::jit::currentFrameId().has_value());
    auto frame_node_id = torch::jit::currentFrameId().value();
    allocation_traces_.emplace_back(torch::jit::MemEvent{
        frame_node_id.pc,
        bt,
        dataPtrAddrToStr(orig_ptr.get()),
        frame_node_id.node_schema,
        frame_node_id.node_header,
        nbytes,
        torch::jit::MemEvent::EventType::Allocate});
    allocations_.insert({orig_ptr.get(), nbytes});

    auto deleter = [this, &bt, &nbytes](void* ptr) {
      auto frame_node_id = torch::jit::currentFrameId().value();
      allocation_traces_.emplace_back(torch::jit::MemEvent{
          frame_node_id.pc,
          bt,
          dataPtrAddrToStr(ptr),
          frame_node_id.node_schema,
          frame_node_id.node_header,
          nbytes,
          torch::jit::MemEvent::EventType::Free});
      return DoNothing(ptr);
    };
    return c10::InefficientStdFunctionContext::makeDataPtr(
        orig_ptr.get(), deleter, orig_ptr.device());
  }

 private:
  c10::Allocator& orig_allocator_;
  mutable std::vector<torch::jit::MemEvent> allocation_traces_;
  mutable std::map<void*, size_t> allocations_;
  friend WithProfileAllocationsGuard;
};

WithProfileAllocationsGuard::WithProfileAllocationsGuard(
    at::DeviceType& device_type)
    : tracer_(std::make_shared<MemoryTracingAllocator>(
          MemoryTracingAllocator{device_type})),
      device_type_(device_type) {}

std::vector<torch::jit::MemEvent> WithProfileAllocationsGuard::getAllocationTraces() {
  return tracer_->allocation_traces_;
}

WithProfileAllocationsGuard::~WithProfileAllocationsGuard() {
  c10::SetAllocator(device_type_, &tracer_->orig_allocator_);
}
