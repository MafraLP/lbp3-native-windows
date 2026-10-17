#include "lbp3_host/elf_reader.hpp"
#include "lbp3_host/win32_file.hpp"

#include <array>
#include <iomanip>
#include <limits>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lbp3::loader {
namespace {
constexpr std::size_t elf_header_size = 64;
constexpr std::size_t program_header_size = 56;

// Decode explicitly: no packing, aliasing, alignment or host-endian assumptions.
template <typename T>
T little_endian(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset + i]))
                 << (8 * i);
    }
    return static_cast<T>(value);
}

bool executable_type(std::uint16_t type) {
    // ET_EXEC, ET_DYN (possibly PIE), ET_SCE_EXEC, ET_SCE_DYNEXEC.
    // ET_SCE_DYNAMIC (0xfe18) is a library, not a main executable.
    return type == 2 || type == 3 || type == 0xfe00 || type == 0xfe10;
}

std::string_view program_type(std::uint32_t type) {
    switch (type) {
    case 0: return "PT_NULL";
    case 1: return "PT_LOAD";
    case 2: return "PT_DYNAMIC";
    case 3: return "PT_INTERP";
    case 4: return "PT_NOTE";
    case 5: return "PT_SHLIB";
    case 6: return "PT_PHDR";
    case 7: return "PT_TLS";
    case 0x6474e550: return "PT_GNU_EH_FRAME";
    case 0x6474e551: return "PT_GNU_STACK";
    case 0x6474e552: return "PT_GNU_RELRO";
    default: return "OS/PROC/UNKNOWN"; // Preserve the numeric value in the report.
    }
}

void validate_segment(const ProgramHeader& ph, std::uint64_t file_size,
                      std::size_t index) {
    if (ph.type == 0) { // PT_NULL fields have no defined meaning.
        return;
    }
    const auto invalid = [index](const char* message) {
        throw std::runtime_error("Program header " + std::to_string(index) + ": " + message);
    };
    if (ph.file_size != 0 &&
        (ph.offset > file_size || ph.file_size > file_size - ph.offset)) {
        invalid("file range exceeds EOF.");
    }
    if (ph.type != 1 && ph.type != 7) {
        return; // Other segment types can have platform-specific semantics.
    }
    if (ph.file_size > ph.memory_size) {
        invalid("p_filesz exceeds p_memsz.");
    }
    if (ph.memory_size > std::numeric_limits<std::uint64_t>::max() - ph.virtual_address) {
        invalid("virtual address range overflows.");
    }
    if (ph.alignment > 1) {
        if ((ph.alignment & (ph.alignment - 1)) != 0) {
            invalid("p_align must be zero, one or a power of two.");
        }
        if (ph.type == 1 && ph.virtual_address % ph.alignment != ph.offset % ph.alignment) {
            invalid("p_vaddr and p_offset are not congruent modulo p_align.");
        }
    }
}
} // namespace

ElfImageInfo inspect_elf(const std::filesystem::path& path, bool allow_shared) {
    platform::Win32File file(path);
    // Read magic first, allowing a useful error even for a short non-ELF file.
    std::array<std::byte, 4> magic{};
    file.read_at(0, magic);
    if (magic != std::array{std::byte{0x7f}, std::byte{'E'}, std::byte{'L'}, std::byte{'F'}}) {
        if (little_endian<std::uint32_t>(magic, 0) == 0x1d3d154f) {
            throw std::runtime_error("SELF container detected; provide an extracted ELF image.");
        }
        throw std::runtime_error("Invalid ELF magic; expected 7F 45 4C 46 at offset zero.");
    }
    std::array<std::byte, elf_header_size> bytes{};
    file.read_at(0, bytes);
    if (bytes[4] != std::byte{2}) {
        throw std::runtime_error("Expected ELFCLASS64 (64-bit ELF).");
    }
    if (bytes[5] != std::byte{1}) {
        throw std::runtime_error("Expected ELFDATA2LSB (little-endian ELF).");
    }
    if (bytes[6] != std::byte{1} || little_endian<std::uint32_t>(bytes, 20) != 1) {
        throw std::runtime_error("Unsupported ELF version.");
    }
    if (little_endian<std::uint16_t>(bytes, 18) != 62) {
        throw std::runtime_error("Expected EM_X86_64 (machine 62).");
    }
    ElfImageInfo image{};
    auto& header = image.header;
    header.type = little_endian<std::uint16_t>(bytes, 16);
    if (!executable_type(header.type) && !(allow_shared && header.type == 0xfe18)) {
        throw std::runtime_error("Unsupported ELF type; expected EXEC, DYN, SCE_EXEC or SCE_DYNEXEC.");
    }
    if (little_endian<std::uint16_t>(bytes, 52) != elf_header_size) {
        throw std::runtime_error("Invalid ELF64 e_ehsize; expected 64.");
    }
    header.os_abi = std::to_integer<std::uint8_t>(bytes[7]);
    header.entry = little_endian<std::uint64_t>(bytes, 24);
    header.program_header_offset = little_endian<std::uint64_t>(bytes, 32);
    header.program_header_count = little_endian<std::uint16_t>(bytes, 56);
    if (header.program_header_count == 0xffff) {
        throw std::runtime_error("PN_XNUM extended numbering is not implemented.");
    }
    if (header.program_header_count == 0) {
        throw std::runtime_error("Executable has no program headers.");
    }
    if (little_endian<std::uint16_t>(bytes, 54) != program_header_size) {
        throw std::runtime_error("Invalid ELF64 e_phentsize; expected 56.");
    }
    image.file_size = file.size();
    const std::uint64_t table_size =
        static_cast<std::uint64_t>(header.program_header_count) * program_header_size;
    if (header.program_header_offset < elf_header_size ||
        header.program_header_offset > file.size() ||
        table_size > file.size() - header.program_header_offset) {
        throw std::runtime_error("Program header table is outside the file or overlaps the ELF header.");
    }
    // Read only metadata: a multi-gigabyte game image is never copied wholesale.
    std::vector<std::byte> table(static_cast<std::size_t>(table_size));
    file.read_at(header.program_header_offset, table);
    image.program_headers.reserve(header.program_header_count);
    for (std::size_t i = 0; i < header.program_header_count; ++i) {
        const auto ph_bytes = std::span<const std::byte>(table).subspan(
            i * program_header_size, program_header_size);
        ProgramHeader ph{
            little_endian<std::uint32_t>(ph_bytes, 0),
            little_endian<std::uint32_t>(ph_bytes, 4),
            little_endian<std::uint64_t>(ph_bytes, 8),
            little_endian<std::uint64_t>(ph_bytes, 16),
            little_endian<std::uint64_t>(ph_bytes, 24),
            little_endian<std::uint64_t>(ph_bytes, 32),
            little_endian<std::uint64_t>(ph_bytes, 40),
            little_endian<std::uint64_t>(ph_bytes, 48)};
        validate_segment(ph, file.size(), i);
        image.program_headers.push_back(ph);
    }
    return image;
}

void print_elf(const ElfImageInfo& image, std::ostream& output) {
    const auto flags = output.flags();
    const auto fill = output.fill();
    output << "ELF64 / x86-64 / little-endian\n"
           << "Type: 0x" << std::hex << image.header.type
           << "  OS ABI: 0x" << static_cast<unsigned>(image.header.os_abi)
           << "  Entry: 0x" << image.header.entry << '\n'
           << "File size: " << std::dec << image.file_size << " bytes\n"
           << "Program headers: " << image.program_headers.size()
           << " at 0x" << std::hex << image.header.program_header_offset << '\n';
    for (std::size_t i = 0; i < image.program_headers.size(); ++i) {
        const auto& ph = image.program_headers[i];
        output << "\n[" << std::dec << i << "] " << program_type(ph.type)
               << " (0x" << std::hex << ph.type << ")  "
               << ((ph.flags & 4) ? 'R' : '-')
               << ((ph.flags & 2) ? 'W' : '-')
               << ((ph.flags & 1) ? 'X' : '-')
               << " (flags=0x" << ph.flags << ")\n";
        const auto field = [&output](std::string_view name, std::uint64_t value) {
            output << "  " << name << " = 0x" << std::setfill('0')
                   << std::setw(16) << std::hex << value << '\n';
        };
        field("p_offset", ph.offset);
        field("p_vaddr ", ph.virtual_address);
        field("p_paddr ", ph.physical_address);
        field("p_filesz", ph.file_size);
        field("p_memsz ", ph.memory_size);
        field("p_align ", ph.alignment);
    }
    output.flags(flags);
    output.fill(fill);
}

} // namespace lbp3::loader
