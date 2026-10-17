#include "lbp3_host/companion_httpd.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <atomic>
#include <cstdint>

namespace lbp3::hle {
namespace {
constexpr std::int32_t invalid_param = static_cast<std::int32_t>(0x80e40004u);
constexpr std::int32_t not_initialized = static_cast<std::int32_t>(0x80e40006u);
constexpr std::int32_t already_initialized = static_cast<std::int32_t>(0x80e40007u);
constexpr std::int32_t no_event = static_cast<std::int32_t>(0x80e40008u);
constexpr std::int32_t already_started = static_cast<std::int32_t>(0x80e4000au);
constexpr std::int32_t not_started = static_cast<std::int32_t>(0x80e4000bu);
constexpr std::size_t options_size = 0x60; // local fields occupy rbp-0x88..rbp-0x29; canary begins at rbp-0x28
std::atomic<bool> initialized{};
std::atomic<bool> started{};

bool accessible(const void* pointer, std::size_t length, bool write_access) noexcept {
    auto current = reinterpret_cast<std::uintptr_t>(pointer);
    if (!current || current > UINTPTR_MAX - length) return false;
    const auto end = current + length;
    while (current < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto protection = info.Protect & 0xff;
        const bool allowed = write_access
            ? protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY
            : protection == PAGE_READONLY || protection == PAGE_READWRITE ||
              protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
              protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (!allowed || info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= current) return false;
        current = base + info.RegionSize;
    }
    return true;
}

std::int32_t LBP3_GUEST_ABI initialize_options(void* options) noexcept {
    if (!accessible(options, options_size, true)) return invalid_param;
    std::uint8_t zeroes[options_size]{};
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(), options, zeroes, sizeof(zeroes), &written) &&
           written == sizeof(zeroes) ? 0 : invalid_param;
}

std::int32_t LBP3_GUEST_ABI initialize_server(const void* options) noexcept {
    if (!accessible(options, options_size, false)) return invalid_param;
    bool expected = false;
    return initialized.compare_exchange_strong(expected, true, std::memory_order_acq_rel)
        ? 0 : already_initialized;
}

std::int32_t LBP3_GUEST_ABI register_request_callback(void* callback, void* user_data) noexcept {
    if (!initialized.load(std::memory_order_acquire)) return not_initialized;
    if (!callback || !user_data) return invalid_param;
    // The offline host doesn't accept remote Companion requests; retaining and
    // dispatching a guest callback without a real request would be misleading.
    return 0;
}

std::int32_t LBP3_GUEST_ABI start_server() noexcept {
    if (!initialized.load(std::memory_order_acquire)) return not_initialized;
    bool expected = false;
    return started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)
        ? 0 : already_started;
}

std::int32_t LBP3_GUEST_ABI stop_server() noexcept {
    if (!initialized.load(std::memory_order_acquire)) return not_initialized;
    bool expected = true;
    if (!started.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return not_started;
    initialized.store(false, std::memory_order_release);
    return 0;
}

std::int32_t LBP3_GUEST_ABI add_header(const char* name, const char* value) noexcept {
    if (!started.load(std::memory_order_acquire)) return not_started;
    if (!name || !value) return invalid_param;
    // No Companion HTTP listener is exposed by the native offline port.
    return no_event;
}

std::int32_t LBP3_GUEST_ABI set_status(std::int32_t status, void* response) noexcept {
    if (!started.load(std::memory_order_acquire)) return not_started;
    if (status < 100 || status > 599 || !response) return invalid_param;
    return no_event;
}
}

std::optional<std::uintptr_t> resolve_companion_httpd(const HostImport& request) {
    if (request.type != 2 || request.library != "libSceCompanionHttpd" ||
        request.module != "libSceCompanionHttpd" || request.library_version != 1 ||
        request.module_version != 0x0101) return std::nullopt;
    if (request.nid == "r-2-a0c7Kfc") return reinterpret_cast<std::uintptr_t>(&initialize_options);
    if (request.nid == "ykNpWs3ktLY") return reinterpret_cast<std::uintptr_t>(&initialize_server);
    if (request.nid == "OaWw+IVEdbI") return reinterpret_cast<std::uintptr_t>(&register_request_callback);
    if (request.nid == "k7F0FcDM-Xc") return reinterpret_cast<std::uintptr_t>(&start_server);
    if (request.nid == "0SCgzfVQHpo") return reinterpret_cast<std::uintptr_t>(&stop_server);
    if (request.nid == "8pWltDG7h6A") return reinterpret_cast<std::uintptr_t>(&add_header);
    if (request.nid == "w7oz0AWHpT4") return reinterpret_cast<std::uintptr_t>(&set_status);
    return std::nullopt;
}
}
