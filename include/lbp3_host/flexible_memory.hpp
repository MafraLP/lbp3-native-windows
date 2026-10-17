#pragma once
#include "lbp3_host/abi.hpp"
#include <cstdint>
#include <memory>

namespace lbp3::memory {
struct FlexibleState;
class ScopedFlexibleMemory {
public:
    // Control thread: workers must finish before destruction/unmapping.
    explicit ScopedFlexibleMemory(std::uint64_t quota=512ull*1024*1024);
    ~ScopedFlexibleMemory();
    ScopedFlexibleMemory(const ScopedFlexibleMemory&)=delete;
    ScopedFlexibleMemory& operator=(const ScopedFlexibleMemory&)=delete;
private:
    std::unique_ptr<FlexibleState> state_;
};
std::int32_t LBP3_GUEST_ABI map_named(void** address,std::uint64_t length,int protection,int flags,const char* name) noexcept;
std::int32_t LBP3_GUEST_ABI unmap(void* address,std::uint64_t length) noexcept;
std::int32_t LBP3_GUEST_ABI lock_pages(void* address,std::uint64_t length) noexcept;
bool owns_writable_range(std::uintptr_t,std::uint64_t) noexcept;
void self_test(std::ostream& output,std::uintptr_t mapper,std::uintptr_t unmapper,std::uintptr_t locker);
}
