#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <memory>
#include <cstdint>

namespace lbp3::abi {
struct ModuleInfoEx;
using ForkCallback=void (LBP3_GUEST_ABI *)() noexcept;
// Registry support only; this does not implement process fork.
std::int32_t register_atfork(ForkCallback,ForkCallback,ForkCallback) noexcept;
std::size_t atfork_handler_count();
void LBP3_GUEST_ABI pthread_cxa_finalize(const ModuleInfoEx*) noexcept;
using OnceRoutine=void (LBP3_GUEST_ABI *)() noexcept;
using KeyDestructor=void (LBP3_GUEST_ABI *)(const void*) noexcept;
struct GuestOnce {std::uint32_t state{},padding{};std::uint64_t mutex{};};
static_assert(sizeof(GuestOnce)==16);
struct ThreadServiceState;
class ScopedThreadServices {
public:
    ScopedThreadServices();
    ~ScopedThreadServices();
    ScopedThreadServices(const ScopedThreadServices&)=delete;
    ScopedThreadServices& operator=(const ScopedThreadServices&)=delete;
private:
    std::unique_ptr<ThreadServiceState> state_;
};
// Normal return path only; called before detaching ELF TLS and before join completes.
void cleanup_thread_specific() noexcept;
std::int32_t LBP3_GUEST_ABI pthread_once(GuestOnce*,OnceRoutine) noexcept;
std::int32_t LBP3_GUEST_ABI key_create(std::int32_t*,KeyDestructor) noexcept;
std::int32_t LBP3_GUEST_ABI key_delete(std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI set_specific(std::int32_t,const void*) noexcept;
const void* LBP3_GUEST_ABI get_specific(std::int32_t) noexcept;
std::optional<std::uintptr_t> resolve_thread_services(const HostImport&);
void thread_services_self_test(std::ostream&);
}
