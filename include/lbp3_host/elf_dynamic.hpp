#pragma once
#include "lbp3_host/elf_reader.hpp"
#include "lbp3_host/win32_file.hpp"
#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace lbp3::loader {
using SymbolIdentity = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t>;
struct DynamicSymbol {
    std::string name;
    std::optional<SymbolIdentity> identity;
    std::uint8_t info{}, visibility{};
    std::uint16_t section{};
    std::uint64_t value{}, size{};
};
struct Relocation {
    std::uint64_t target{};
    std::uint32_t type{}, symbol{};
    std::int64_t addend{};
};
struct DynamicImage {
    std::optional<std::uint64_t> init,fini;
    std::vector<DynamicSymbol> symbols;
    std::vector<Relocation> relocations;
};
DynamicImage read_dynamic(platform::Win32File& file, const ElfImageInfo& image);
// Checked S+A / B+A arithmetic, also used by relocation tests.
std::uint64_t add_signed(std::uint64_t base, std::int64_t addend);
}
