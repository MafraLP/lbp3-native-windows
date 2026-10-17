#pragma once

#include "lbp3_host/host_resolver.hpp"
#include <Windows.h>
#include <cstdint>
#include <optional>

namespace lbp3::gpu {
int initialize_iss_backend(const char* title, const char* serial, const char* user_directory,
                           std::uint32_t sdk_version, std::uint32_t psf_attributes,
                           std::int32_t width, std::int32_t height);
std::optional<std::uintptr_t> resolve_iss_backend_import(const HostImport& request);
bool iss_backend_handle_fault(EXCEPTION_POINTERS* exception, void* address) noexcept;
}
