#pragma once
#include <Windows.h>
#include <cstdint>
namespace lbp3::memory {
constexpr std::uintptr_t guest_address_min=0x1000000000ull;
constexpr std::uintptr_t guest_address_limit=1ull<<40;
// New reservations only. Keeps guest heap/module pointers representable by
// the game's 40-bit shader addresses. Never replaces an existing allocation.
void* allocate_low(std::uint64_t length,DWORD allocation,DWORD protection) noexcept;
}
