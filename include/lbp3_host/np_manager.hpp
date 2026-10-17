#pragma once
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_np_manager(const HostImport&);
std::optional<std::uintptr_t> resolve_np_webapi(const HostImport&);
std::optional<std::uintptr_t> resolve_np_signaling(const HostImport&);
void np_manager_self_test();
}
