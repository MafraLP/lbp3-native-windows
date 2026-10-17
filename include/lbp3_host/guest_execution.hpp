#pragma once
#include "lbp3_host/guest_thread.hpp"

namespace lbp3::abi {
struct ExecutionResult {
    void* value{};
    std::uint32_t code{}, thread_id{};
    std::uintptr_t rip{}, address{};
    std::uint64_t access{};
    bool fault{};
};
// Narrow SEH guard for the fixed synthetic leaf fault fixture, not arbitrary games.
void execute_fixture(GuestThreadEntry entry, void* argument, std::uintptr_t low,
                     std::uintptr_t high, ExecutionResult* result) noexcept;
}
