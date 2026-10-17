#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/memory_query.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>
namespace lbp3::abi {
struct ModuleSegment {std::uint64_t address{};std::uint32_t size{};std::int32_t protection{};};
struct ModuleInfoEx {
    std::uint64_t size{424};std::array<char,256> name{};
    std::int32_t id{};std::uint32_t tls_index{};std::uint64_t tls_initial{};
    std::uint32_t tls_initial_size{},tls_size{},tls_offset{},tls_alignment{};
    std::uint64_t init{},fini{},reserved1{},reserved2{},eh_header{},eh_frame{};
    std::uint32_t eh_header_size{},eh_frame_size{};
    std::array<ModuleSegment,4> segments{};std::uint32_t segment_count{};
};
static_assert(sizeof(ModuleInfoEx)==424);
static_assert(offsetof(ModuleInfoEx,segments)==352);
struct SignalReturnRange {std::uintptr_t address{};std::size_t size{};};
class ScopedModuleRegistry {
public:
    ScopedModuleRegistry(std::span<const ModuleInfoEx>,std::span<const SignalReturnRange> returns={});
    ~ScopedModuleRegistry();
    ScopedModuleRegistry(const ScopedModuleRegistry&)=delete;
    ScopedModuleRegistry& operator=(const ScopedModuleRegistry&)=delete;
private:
    std::vector<ModuleInfoEx> modules_;
    std::vector<SignalReturnRange> returns_;
};
std::int32_t LBP3_GUEST_ABI module_from_address(std::uintptr_t,int,ModuleInfoEx*) noexcept;
std::int32_t LBP3_GUEST_ABI module_matches_address(const ModuleInfoEx*,std::uintptr_t) noexcept;
std::int32_t LBP3_GUEST_ABI is_signal_return(std::uintptr_t) noexcept;
std::optional<memory::QueryInfo> query_module_memory(std::uintptr_t,bool next);
void module_registry_self_test(std::ostream&);
}
