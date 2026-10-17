#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
namespace lbp3::gpu {
std::int32_t LBP3_GUEST_ABI submit_and_flip_command_buffers(
    std::uint32_t, const std::uint32_t* const*, const std::uint32_t*,
    const std::uint32_t* const*, const std::uint32_t*, std::uint32_t,
    std::uint32_t, std::uint32_t, std::int64_t) noexcept;
std::uint32_t LBP3_GUEST_ABI dispatch_initial_state(std::uint32_t*,std::uint32_t) noexcept;
std::uint32_t LBP3_GUEST_ABI draw_initial_state(std::uint32_t*,std::uint32_t) noexcept;
std::int32_t LBP3_GUEST_ABI insert_wait_flip(std::uint32_t*,std::uint32_t,std::int32_t,std::uint32_t) noexcept;
std::int32_t LBP3_GUEST_ABI set_compute_shader(std::uint32_t*,std::uint32_t,const std::uint32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI pop_marker(std::uint32_t*,std::uint32_t) noexcept;
std::int32_t LBP3_GUEST_ABI push_marker(std::uint32_t*,std::uint32_t,const char*) noexcept;
std::optional<std::uintptr_t> resolve_gnm_commands(const HostImport&);
}
