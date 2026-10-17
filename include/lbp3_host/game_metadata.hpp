#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace lbp3::loader {
struct GameMetadata {
    std::string title;
    std::string title_id;
    std::uint32_t sdk_version{};
    std::uint32_t psf_attributes{};
};

// Reads TITLE/TITLE_ID/ATTRIBUTE from param.sfo and sdk_version from the
// executable's PT_SCE_PROCPARAM. Rejects malformed or incomplete metadata.
GameMetadata read_game_metadata(const std::filesystem::path& param_sfo,
                                const std::filesystem::path& executable_elf);
}
