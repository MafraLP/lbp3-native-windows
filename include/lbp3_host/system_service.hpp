#pragma once
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_system_service(const HostImport&);
void system_service_self_test();
}
