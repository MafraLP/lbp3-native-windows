#pragma once
#include <Windows.h>
#include <cstdint>
namespace lbp3::abi {
// Pinned main executable's Variant-II TLS, with TP immediately after its block.
// Only the exact MOV RAX,FS:[0] instruction is emulated on a null-read fault.
// Windows FS/GS remains untouched. Other segment instructions are unsupported.
class ScopedGuestFs {
public:
    ScopedGuestFs(std::uintptr_t executable_begin,std::uintptr_t executable_end,std::uint64_t main_tls_id);
    ~ScopedGuestFs();
    ScopedGuestFs(const ScopedGuestFs&)=delete;
    ScopedGuestFs& operator=(const ScopedGuestFs&)=delete;
    std::uintptr_t begin{},end{};std::uint64_t tls_id{};
};
bool emulate_guest_fs(EXCEPTION_POINTERS*) noexcept;
}
