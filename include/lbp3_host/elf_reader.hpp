#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <vector>

namespace lbp3::loader {

// Decoded values, not structures mapped onto untrusted file bytes.
struct ElfHeader {
    std::uint16_t type{};
    std::uint8_t os_abi{};
    std::uint64_t entry{};
    std::uint64_t program_header_offset{};
    std::uint16_t program_header_count{};
};

struct ProgramHeader {
    std::uint32_t type{};
    std::uint32_t flags{};
    std::uint64_t offset{};
    std::uint64_t virtual_address{};
    std::uint64_t physical_address{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
};

struct ElfImageInfo {
    ElfHeader header;
    std::uint64_t file_size{};
    std::vector<ProgramHeader> program_headers;
};

// Throws std::runtime_error or std::system_error for invalid input / Win32 errors.
// This inspector does not map executable memory or transfer control to the image.
// Shared PRX images are accepted only when explicitly requested by a module loader.
[[nodiscard]] ElfImageInfo inspect_elf(const std::filesystem::path& path, bool allow_shared = false);
void print_elf(const ElfImageInfo& image, std::ostream& output);

} // namespace lbp3::loader
