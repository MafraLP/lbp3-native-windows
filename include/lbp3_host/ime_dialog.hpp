#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
namespace lbp3::hle {
std::optional<std::uintptr_t> resolve_ime_dialog(const HostImport&);
void ime_dialog_self_test(std::ostream&);
}
