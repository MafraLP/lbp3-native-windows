#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
namespace lbp3::abi {
std::optional<std::uintptr_t> resolve_condition(const HostImport&);
void condition_self_test(std::ostream&);
}
