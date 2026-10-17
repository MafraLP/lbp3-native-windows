#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_event_flags(const HostImport&);
void event_flags_self_test(std::ostream&);
}
