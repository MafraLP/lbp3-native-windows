#pragma once
#include "lbp3_host/host_resolver.hpp"
namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_filesystem(const HostImport&);
std::int32_t* guest_errno_address() noexcept;
}
