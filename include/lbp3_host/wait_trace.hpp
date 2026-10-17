#pragma once

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace lbp3::diagnostics {
inline bool guest_wait_trace_enabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[8]{};
        return GetEnvironmentVariableW(L"LBP3_GUEST_WAIT_TRACE", value, 8) != 0;
    }();
    return enabled;
}

inline void guest_wait_trace(const char* api, std::uint64_t handle,
                             std::uint64_t timeout_us, std::int32_t result,
                             std::chrono::steady_clock::duration elapsed,
                             std::uintptr_t caller = 0) noexcept {
    if (!guest_wait_trace_enabled()) return;
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    // Short lock contention and the guest's 1 ms polling loops obscure waits
    // that can actually hold startup. Keep this trace focused on those waits.
    if (micros < 10'000) return;
    static std::atomic_uint sequence{};
    const auto n = sequence.fetch_add(1, std::memory_order_relaxed);
    if (n >= 2048) return;
    std::fprintf(stderr,
        "[LBP3_GUEST_WAIT_TRACE] #%u tid=%lu api=%s handle=0x%llx caller=0x%llx timeout_us=%llu result=0x%08x elapsed_us=%lld\n",
        n, static_cast<unsigned long>(GetCurrentThreadId()), api,
        static_cast<unsigned long long>(handle), static_cast<unsigned long long>(caller),
        static_cast<unsigned long long>(timeout_us),
        static_cast<unsigned>(result), static_cast<long long>(micros));
    std::fflush(stderr);
}

inline void guest_sync_event(const char* api, std::uint64_t handle,
                             std::uint64_t detail, std::uintptr_t caller) noexcept {
    if (!guest_wait_trace_enabled()) return;
    static std::atomic_uint sequence{};
    const auto n = sequence.fetch_add(1, std::memory_order_relaxed);
    if (n >= 1024) return;
    std::fprintf(stderr,
        "[LBP3_GUEST_SYNC_TRACE] #%u tid=%lu api=%s handle=0x%llx detail=0x%llx caller=0x%llx\n",
        n, static_cast<unsigned long>(GetCurrentThreadId()), api,
        static_cast<unsigned long long>(handle), static_cast<unsigned long long>(detail),
        static_cast<unsigned long long>(caller));
    std::fflush(stderr);
}
}
