#pragma once
#include "lbp3_host\host_resolver.hpp"
#include <iosfwd>
namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_pad_services(const HostImport&);
void pad_services_self_test(std::ostream&);
}
