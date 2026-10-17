// The host-side GPU runtime is deliberately separate from command-buffer helpers.
// Until the Vulkan/Liverpool backend has been initialized with a guest address
// space and presenter, submissions must fail explicitly rather than pretending
// that a guest command buffer was consumed.
#include "lbp3_host/gnm_commands.hpp"
#include <Windows.h>
#include <array>
#include <cstdint>

namespace lbp3::gpu {
namespace {
constexpr std::int32_t kNotInitialized = static_cast<std::int32_t>(0x80d11000u);
}

std::int32_t LBP3_GUEST_ABI submit_and_flip_command_buffers(
    std::uint32_t count, const std::uint32_t* const* dcb_addresses,
    const std::uint32_t* dcb_sizes, const std::uint32_t* const* ccb_addresses,
    const std::uint32_t* ccb_sizes, std::uint32_t video_handle,
    std::uint32_t buffer_index, std::uint32_t flip_mode,
    std::int64_t flip_argument) noexcept {
    (void)video_handle;
    (void)buffer_index;
    (void)flip_mode;
    (void)flip_argument;

    // Match the guest ABI's argument validation before reporting that no real
    // command processor is available. A count of zero is valid (flip-only).
    if (count != 0 && (!dcb_addresses || !dcb_sizes || (ccb_sizes && !ccb_addresses)))
        return kNotInitialized;
    if (count > 0x10000u) return kNotInitialized;
    std::array<std::uint32_t, 2> values{};
    for (std::uint32_t i = 0; i < count; ++i) {
        SIZE_T read{};
        const auto index = static_cast<std::uintptr_t>(i) * sizeof(void*);
        const auto dcb_table = reinterpret_cast<std::uintptr_t>(dcb_addresses);
        const auto dcb_size_table = reinterpret_cast<std::uintptr_t>(dcb_sizes);
        if (dcb_table > UINTPTR_MAX - index || dcb_size_table > UINTPTR_MAX - static_cast<std::uintptr_t>(i) * 4 ||
            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(dcb_table + index), &values[0], sizeof(void*), &read) || read != sizeof(void*) ||
            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(dcb_size_table + static_cast<std::uintptr_t>(i) * 4), &values[1], 4, &read) || read != 4 ||
            !values[0] || values[1] == 0 || values[1] > 0x3ffffcu)
            return kNotInitialized;
        if (ccb_sizes) {
            const auto ccb_size_table = reinterpret_cast<std::uintptr_t>(ccb_sizes);
            if (ccb_size_table > UINTPTR_MAX - static_cast<std::uintptr_t>(i) * 4 ||
                !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ccb_size_table + static_cast<std::uintptr_t>(i) * 4), &values[1], 4, &read) || read != 4 || values[1] > 0x3ffffcu)
                return kNotInitialized;
        }
    }
    return kNotInitialized;
}
}
#include "lbp3_host\gnm_commands.hpp"
#include <Windows.h>
