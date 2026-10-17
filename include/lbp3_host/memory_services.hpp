#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include "lbp3_host/memory_query.hpp"
namespace lbp3::hle {
std::int32_t LBP3_GUEST_ABI virtual_query(const void*,std::int32_t,memory::QueryInfo*,std::uint64_t) noexcept;
std::optional<std::uintptr_t> resolve_memory_query(const HostImport&);
}
