#include "lbp3_host/elf_dynamic.hpp"
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>

namespace lbp3::loader {
namespace {
std::uint64_t read(std::span<const std::byte> data, std::uint64_t offset, std::size_t size) {
    if (offset > data.size() || size > data.size() - offset)
        throw std::runtime_error("Dynamic table range is outside its containing data.");
    std::uint64_t result{};
    for (std::size_t i = 0; i < size; ++i)
        result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(data[static_cast<std::size_t>(offset)+i])) << (8*i);
    return result;
}
std::span<const std::byte> range(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset)
        throw std::runtime_error("Dynamic table range is outside its containing data.");
    return bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
}
std::string string(std::span<const std::byte> bytes, std::uint64_t offset) {
    if (offset >= bytes.size()) throw std::runtime_error("Dynamic string offset outside table.");
    std::string result;
    for (auto b : bytes.subspan(static_cast<std::size_t>(offset))) {
        if (b == std::byte{}) return result;
        result += static_cast<char>(std::to_integer<unsigned char>(b));
    }
    throw std::runtime_error("Unterminated dynamic string.");
}
std::string encode(std::uint64_t id) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result(1, alphabet[id & 63]);
    while (id >= 64) { id >>= 6; result.insert(result.begin(), alphabet[id & 63]); }
    return result;
}
}
std::uint64_t add_signed(std::uint64_t base, std::int64_t addend) {
    if (addend >= 0) {
        const auto positive = static_cast<std::uint64_t>(addend);
        if (positive > std::numeric_limits<std::uint64_t>::max() - base)
            throw std::runtime_error("Relocation arithmetic overflow.");
        return base + positive;
    }
    const auto magnitude = static_cast<std::uint64_t>(-(addend + 1)) + 1;
    if (magnitude > base) throw std::runtime_error("Relocation arithmetic underflow.");
    return base - magnitude;
}

DynamicImage read_dynamic(platform::Win32File& file, const ElfImageInfo& image) {
    const auto segment = [&](std::uint32_t type) {
        const ProgramHeader* found{};
        for (const auto& ph : image.program_headers) if (ph.type == type) {
            if (found) throw std::runtime_error("Duplicate dynamic program header.");
            found = &ph;
        }
        if (!found) throw std::runtime_error("Required dynamic program header missing.");
        if (found->file_size > 64ull * 1024 * 1024) throw std::runtime_error("Dynamic metadata exceeds budget.");
        std::vector<std::byte> bytes(static_cast<std::size_t>(found->file_size));
        file.read_at(found->offset, bytes);
        return bytes;
    };
    const auto dynamic = segment(2);
    const auto blob = segment(0x61000000);
    if (dynamic.size() % 16) throw std::runtime_error("Truncated dynamic entry.");
    std::map<std::uint64_t, std::vector<std::uint64_t>> tags;
    bool terminated = false;
    for (std::size_t i = 0; i < dynamic.size(); i += 16) {
        const auto tag = read(dynamic, i, 8);
        if (!tag) { terminated = true; break; }
        tags[tag].push_back(read(dynamic, i+8, 8));
    }
    if (!terminated) throw std::runtime_error("Dynamic table is missing DT_NULL.");
    const auto one = [&](std::uint64_t tag) {
        const auto it = tags.find(tag);
        if (it == tags.end() || it->second.size() != 1)
            throw std::runtime_error("Missing or duplicate singleton dynamic tag.");
        return it->second.front();
    };
    const auto strings = range(blob, one(0x61000035), one(0x61000037));
    using Descriptor = std::pair<std::string, std::uint16_t>;
    std::map<std::string, Descriptor> libraries, modules;
    for (const auto tag : {0x6100000dull, 0x6100000full, 0x61000013ull, 0x61000015ull}) {
        for (const auto value : tags[tag]) {
            auto& table = (tag == 0x61000013 || tag == 0x61000015) ? libraries : modules;
            const auto id = encode(value >> 48);
            const Descriptor descriptor{string(strings, value & 0xffffffff), static_cast<std::uint16_t>(value >> 32)};
            const auto [it, inserted] = table.emplace(id, descriptor);
            if (!inserted && it->second != descriptor) throw std::runtime_error("Conflicting dynamic identity.");
        }
    }
    if (one(0x6100003b) != 24) throw std::runtime_error("Unsupported dynamic symbol size.");
    const auto symbols = range(blob, one(0x61000039), one(0x6100003f));
    if (symbols.size() % 24) throw std::runtime_error("Truncated dynamic symbol table.");
    DynamicImage result;
    if(tags.contains(12))result.init=one(12);
    if(tags.contains(13))result.fini=one(13);
    for (std::size_t i = 0; i < symbols.size(); i += 24) {
        DynamicSymbol symbol;
        symbol.name = string(strings, read(symbols, i, 4));
        symbol.info = static_cast<std::uint8_t>(read(symbols, i+4, 1));
        symbol.visibility = static_cast<std::uint8_t>(read(symbols, i+5, 1) & 3);
        symbol.section = static_cast<std::uint16_t>(read(symbols, i+6, 2));
        symbol.value = read(symbols, i+8, 8);
        symbol.size = read(symbols, i+16, 8);
        const auto a = symbol.name.find('#');
        const auto b = a == std::string::npos ? a : symbol.name.find('#', a+1);
        if (b != std::string::npos) {
            if (symbol.name.find('#', b+1) != std::string::npos) throw std::runtime_error("Malformed scoped symbol.");
            const auto lib = libraries.find(symbol.name.substr(a+1, b-a-1));
            const auto mod = modules.find(symbol.name.substr(b+1));
            if (lib == libraries.end() || mod == modules.end()) throw std::runtime_error("Undeclared symbol identity.");
            symbol.identity = SymbolIdentity{symbol.name.substr(0,a), lib->second.first, lib->second.second,
                                             mod->second.first, mod->second.second};
        }
        result.symbols.push_back(std::move(symbol));
    }
    for (const auto [offset_tag, size_tag] : {std::pair{0x61000029ull, 0x6100002dull},
                                             std::pair{0x6100002full, 0x61000031ull}}) {
        if (!tags.contains(offset_tag) && !tags.contains(size_tag)) continue;
        if (offset_tag == 0x61000029 && one(0x6100002b) != 7)
            throw std::runtime_error("PLT table must use RELA entries.");
        if (offset_tag == 0x6100002f && one(0x61000033) != 24)
            throw std::runtime_error("Unsupported RELA entry size.");
        const auto table = range(blob, one(offset_tag), one(size_tag));
        if (table.size() % 24) throw std::runtime_error("Truncated RELA table.");
        for (std::size_t i = 0; i < table.size(); i += 24) {
            const auto info = read(table, i+8, 8);
            const auto index = static_cast<std::uint32_t>(info >> 32);
            if (index >= result.symbols.size()) throw std::runtime_error("Invalid relocation symbol index.");
            result.relocations.push_back({read(table, i, 8), static_cast<std::uint32_t>(info), index,
                std::bit_cast<std::int64_t>(read(table, i+16, 8))});
        }
    }
    return result;
}
}
