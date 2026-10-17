#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <memory>
namespace lbp3::gpu {
struct ComputeState;
struct ComputeQueueInfo {std::uint32_t pipe{},queue{};std::uintptr_t ring{};std::uint32_t dwords{};std::uint32_t* read_pointer{};};
class ScopedComputeQueues {
public:
    ScopedComputeQueues();~ScopedComputeQueues();
    ScopedComputeQueues(const ScopedComputeQueues&)=delete;
    ScopedComputeQueues& operator=(const ScopedComputeQueues&)=delete;
private:std::unique_ptr<ComputeState> state_;
};
std::int32_t LBP3_GUEST_ABI map_compute_queue(std::uint32_t,std::uint32_t,std::uintptr_t,std::uint32_t,std::uint32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI unmap_compute_queue(std::uint32_t) noexcept;
std::int32_t LBP3_GUEST_ABI gnm_add_event(std::int64_t,std::uint64_t,void*) noexcept;
std::int32_t LBP3_GUEST_ABI gnm_delete_event(std::int64_t,std::uint64_t) noexcept;
std::optional<ComputeQueueInfo> compute_queue(std::uint32_t);
std::optional<std::uintptr_t> resolve_compute_queue(const HostImport&);
}
