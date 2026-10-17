#pragma once
#include <cstddef>
#include <vector>

namespace lbp3::loader {
// Fixed fixture code, assembled here from explicit x86-64 bytes; no game input.
std::vector<std::byte> synthetic_elf(bool fault);
}
