#pragma once
#include <filesystem>
#include <string_view>
namespace lbp3::diagnostics {
// Child-process diagnostic entry points, never a game-launch API.
void libc_probe(const std::filesystem::path&,bool dependencies=false,bool constructors=false,bool entry=false,const std::filesystem::path& app={},const std::filesystem::path& save={},bool initialize_gpu=false);
void probe_fixture(std::wstring_view);
}
