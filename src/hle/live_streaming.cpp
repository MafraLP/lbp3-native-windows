#include "lbp3_host/live_streaming.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace lbp3::hle {
namespace {
std::int32_t LBP3_GUEST_ABI initialize(std::uint32_t) noexcept { return 0; }
std::int32_t LBP3_GUEST_ABI enable(std::int32_t) noexcept { return 0; }
std::int32_t LBP3_GUEST_ABI current_status(std::uint32_t* output) noexcept {
    if (!output) return -1;
    const std::uint32_t offline[2]{};
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(), output, offline, sizeof(offline), &written) && written == sizeof(offline) ? 0 : -1;
}
std::int32_t LBP3_GUEST_ABI feedback_count(std::int32_t, std::int32_t* output) noexcept {
    if (!output) return -1;
    const std::int32_t zero{};SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(), output, &zero, sizeof(zero), &written) && written == sizeof(zero) ? 0 : -1;
}
}

std::optional<std::uintptr_t> resolve_live_streaming(const HostImport& r) {
    if (r.type != 2 || r.library != "libSceGameLiveStreaming" || r.module != "libSceGameLiveStreaming" ||
        r.library_version != 1 || r.module_version != 0x0101) return std::nullopt;
    if (r.nid == "kvYEw2lBndk") return reinterpret_cast<std::uintptr_t>(&initialize);
    if (r.nid == "dWM80AX39o4") return reinterpret_cast<std::uintptr_t>(&enable);
    if (r.nid == "CoPMx369EqM") return reinterpret_cast<std::uintptr_t>(&current_status);
    if (r.nid == "yeQKjHETi40") return reinterpret_cast<std::uintptr_t>(&feedback_count);
    return std::nullopt;
}

void live_streaming_self_test(std::ostream& out) {
    using Init = std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t) noexcept;
    using Enable = std::int32_t (LBP3_GUEST_ABI *)(std::int32_t) noexcept;
    using Status = std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t*) noexcept;
    using Count = std::int32_t (LBP3_GUEST_ABI *)(std::int32_t, std::int32_t*) noexcept;
    const auto init = resolve_live_streaming({"kvYEw2lBndk", "libSceGameLiveStreaming", "libSceGameLiveStreaming", 1, 0x0101, 2});
    const auto enable_fn = resolve_live_streaming({"dWM80AX39o4", "libSceGameLiveStreaming", "libSceGameLiveStreaming", 1, 0x0101, 2});
    const auto status = resolve_live_streaming({"CoPMx369EqM", "libSceGameLiveStreaming", "libSceGameLiveStreaming", 1, 0x0101, 2});
    std::uint32_t state[2]{1, 1};
    const auto count_fn = resolve_live_streaming({"yeQKjHETi40", "libSceGameLiveStreaming", "libSceGameLiveStreaming", 1, 0x0101, 2});
    std::int32_t count = 99;
    if (!init || reinterpret_cast<Init>(*init)(0x4000) != 0 || !enable_fn || reinterpret_cast<Enable>(*enable_fn)(1) != 0 ||
        !status || reinterpret_cast<Status>(*status)(state) != 0 || state[0] != 0 || state[1] != 0 || reinterpret_cast<Status>(*status)(nullptr) == 0 ||
        !count_fn || reinterpret_cast<Count>(*count_fn)(2,&count) != 0 || count != 0 || reinterpret_cast<Count>(*count_fn)(2,nullptr) == 0 ||
        resolve_live_streaming({"kvYEw2lBndk", "libSceNpManager", "libSceGameLiveStreaming", 1, 0x0101, 2}) ||
        resolve_live_streaming({"kvYEw2lBndk", "libSceGameLiveStreaming", "libSceGameLiveStreaming", 1, 0x0100, 2}))
        throw std::runtime_error("Optional live-streaming init/enable resolver fixture failed.");
    out << "Live-streaming tests passed: exact imports, init/enable path, offline status/count and pointer failures.\n";
}
}
