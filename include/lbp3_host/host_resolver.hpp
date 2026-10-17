#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace lbp3 {
struct HostImport {
    std::string_view nid, library, module;
    std::uint16_t library_version{}, module_version{};
    std::uint8_t type{};
};
using HostResolver = std::optional<std::uintptr_t> (*)(const HostImport&);
}
