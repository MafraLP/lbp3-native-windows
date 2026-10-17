#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_live_streaming(const HostImport&);
void live_streaming_self_test(std::ostream&);
}
