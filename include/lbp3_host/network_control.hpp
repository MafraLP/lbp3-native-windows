#pragma once
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_network_control(const HostImport&);
void network_control_self_test();
}
