#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
namespace lbp3::abi {
std::int32_t LBP3_GUEST_ABI sem_create(std::uint32_t*,const char*,std::uint32_t,std::int32_t,std::int32_t,const void*) noexcept;
std::int32_t LBP3_GUEST_ABI sem_delete(std::uint32_t) noexcept;
std::int32_t LBP3_GUEST_ABI sem_signal(std::uint32_t,std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI sem_poll(std::uint32_t,std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI sem_wait(std::uint32_t,std::int32_t,std::uint32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI sem_cancel(std::uint32_t,std::int32_t,std::int32_t*) noexcept;
std::optional<std::uintptr_t> resolve_semaphore(const HostImport&);
// Host diagnostics only; not a guest export.
std::optional<std::size_t> semaphore_waiter_count(std::uint32_t);
}
