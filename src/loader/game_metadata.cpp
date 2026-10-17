#include "lbp3_host/game_metadata.hpp"
#include "lbp3_host/elf_reader.hpp"
#include "lbp3_host/win32_file.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace lbp3::loader {
namespace {
std::uint16_t u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 2) throw std::runtime_error("Truncated SFO field.");
    return std::to_integer<std::uint8_t>(bytes[at]) |
           (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[at + 1])) << 8);
}
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Truncated metadata field.");
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[at + 3])) << 24);
}
std::size_t checked_range(std::uint32_t base, std::uint32_t offset, std::uint32_t size,
                          std::size_t total, const char* label) {
    const std::uint64_t begin = static_cast<std::uint64_t>(base) + offset;
    if (begin > total || size > total - static_cast<std::size_t>(begin))
        throw std::runtime_error(std::string("Invalid SFO ") + label + " range.");
    return static_cast<std::size_t>(begin);
}
std::map<std::string, std::vector<std::byte>> parse_sfo(const std::filesystem::path& path) {
    platform::Win32File file(path);
    if (file.size() < 20 || file.size() > 1024 * 1024) throw std::runtime_error("SFO size is outside supported bounds.");
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.size()));
    file.read_at(0, bytes);
    const auto data = std::span<const std::byte>(bytes);
    if (u32(data, 0) != 0x46535000 || u32(data, 4) != 0x00000101)
        throw std::runtime_error("Unsupported param.sfo magic or version.");
    const auto key_base = u32(data, 8), data_base = u32(data, 12), count = u32(data, 16);
    if (count > 4096 || 20ull + 16ull * count > bytes.size() || key_base > bytes.size() || data_base > bytes.size())
        throw std::runtime_error("Invalid SFO table bounds.");
    std::map<std::string, std::vector<std::byte>> values;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = 20 + static_cast<std::size_t>(i) * 16;
        const auto key_offset = u16(data, entry);
        const auto format = u16(data, entry + 2);
        const auto length = u32(data, entry + 4);
        const auto maximum = u32(data, entry + 8);
        const auto value_offset = u32(data, entry + 12);
        if (length > maximum || maximum > 1024 * 1024) throw std::runtime_error("Invalid SFO value length.");
        const auto key_at = checked_range(key_base, key_offset, 1, bytes.size(), "key");
        auto key_end = key_at;
        while (key_end < data_base && bytes[key_end] != std::byte{}) ++key_end;
        if (key_end == data_base) throw std::runtime_error("Unterminated or out-of-range SFO key.");
        std::string key;
        key.reserve(key_end - key_at);
        for (auto at = key_at; at < key_end; ++at) key.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[at])));
        const auto value_at = checked_range(data_base, value_offset, length, bytes.size(), "value");
        if (format != 0x0204 && format != 0x0404) continue;
        if (!values.emplace(std::move(key), std::vector<std::byte>(bytes.begin() + value_at,
                                                                   bytes.begin() + value_at + length)).second)
            throw std::runtime_error("Duplicate SFO key.");
    }
    return values;
}
std::string string_value(const std::map<std::string, std::vector<std::byte>>& values, const char* key) {
    const auto it = values.find(key);
    if (it == values.end() || it->second.empty()) throw std::runtime_error(std::string("param.sfo is missing ") + key + ".");
    const auto& bytes = it->second;
    const auto end = std::find(bytes.begin(), bytes.end(), std::byte{});
    std::string value;
    value.reserve(static_cast<std::size_t>(end - bytes.begin()));
    for (auto at = bytes.begin(); at != end; ++at) value.push_back(static_cast<char>(std::to_integer<unsigned char>(*at)));
    if (value.empty()) throw std::runtime_error(std::string("param.sfo has empty ") + key + ".");
    return value;
}
std::uint32_t integer_value(const std::map<std::string, std::vector<std::byte>>& values, const char* key) {
    const auto it = values.find(key);
    if (it == values.end() || it->second.size() != 4) throw std::runtime_error(std::string("param.sfo has invalid ") + key + ".");
    return u32(it->second, 0);
}
std::uint32_t executable_sdk(const std::filesystem::path& path) {
    const auto image = inspect_elf(path, false);
    const auto it = std::find_if(image.program_headers.begin(), image.program_headers.end(),
                                 [](const ProgramHeader& header) { return header.type == 0x61000001; });
    if (it == image.program_headers.end() || it->file_size < 20 || it->file_size > 4096)
        throw std::runtime_error("Executable has no valid PT_SCE_PROCPARAM.");
    std::array<std::byte, 20> bytes{};
    platform::Win32File file(path);
    file.read_at(it->offset, bytes);
    const auto data = std::span<const std::byte>(bytes);
    if (u32(data, 8) != 0x4942524f) throw std::runtime_error("PT_SCE_PROCPARAM has invalid ORBI magic.");
    const auto size = static_cast<std::uint64_t>(u32(data, 0)) | (static_cast<std::uint64_t>(u32(data, 4)) << 32);
    if (size < 20 || size > it->file_size) throw std::runtime_error("PT_SCE_PROCPARAM has invalid size.");
    const auto sdk = u32(data, 16);
    if (sdk == 0 || sdk == UINT32_MAX) throw std::runtime_error("PT_SCE_PROCPARAM has invalid SDK version.");
    return sdk;
}
}

GameMetadata read_game_metadata(const std::filesystem::path& param_sfo,
                                const std::filesystem::path& executable_elf) {
    const auto values = parse_sfo(param_sfo);
    GameMetadata result;
    result.title = string_value(values, "TITLE");
    result.title_id = string_value(values, "TITLE_ID");
    result.psf_attributes = integer_value(values, "ATTRIBUTE");
    result.sdk_version = executable_sdk(executable_elf);
    return result;
}
}
