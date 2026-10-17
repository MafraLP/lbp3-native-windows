#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <memory>
#include <array>
namespace lbp3::memory {
struct DirectState;
struct DirectViewInformation {std::uintptr_t address{};std::uint64_t physical{},length{};int protection{},memory_type{};std::array<char,32> name{};};
std::optional<DirectViewInformation> direct_view(std::uintptr_t,bool next=false);
class ScopedDirectMemory {
public:
    explicit ScopedDirectMemory(std::uint64_t capacity=5056ull*1024*1024);
    ~ScopedDirectMemory();
    ScopedDirectMemory(const ScopedDirectMemory&)=delete;
    ScopedDirectMemory& operator=(const ScopedDirectMemory&)=delete;
private:std::unique_ptr<DirectState> state_;
};
std::uint64_t LBP3_GUEST_ABI direct_size() noexcept;
std::int32_t LBP3_GUEST_ABI direct_allocate(std::int64_t,std::int64_t,std::uint64_t,std::uint64_t,int,std::int64_t*) noexcept;
std::int32_t LBP3_GUEST_ABI direct_release(std::int64_t,std::uint64_t) noexcept;
std::int32_t LBP3_GUEST_ABI direct_map(void**,std::uint64_t,int,int,std::int64_t,std::uint64_t) noexcept;
std::int32_t LBP3_GUEST_ABI direct_map_named(void**,std::uint64_t,int,int,std::int64_t,std::uint64_t,const char*) noexcept;
std::int32_t direct_unmap(void*,std::uint64_t) noexcept;
std::optional<std::uintptr_t> resolve_direct_memory(const HostImport&);
}
