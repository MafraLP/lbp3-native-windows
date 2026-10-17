#pragma once
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_np_trophy(const HostImport&);
void np_trophy_self_test();
}
