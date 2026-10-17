#pragma once
#include "lbp3_host/guest_thread.hpp"
#include "lbp3_host/host_resolver.hpp"

namespace lbp3::abi {
using GuestHandle = std::uint64_t;
using GuestAttribute=std::uint64_t;
enum class AttributeField {StackSize,Affinity,Inherit,Detach,Policy,Priority};
struct PthreadAttributes {std::uint64_t stack_size{1024*1024},affinity{0x7f};int inherit{4},detached{},policy{1},priority{700};};
class PthreadRuntime {
public:
    explicit PthreadRuntime(std::span<const TlsTemplate> templates);
    ~PthreadRuntime();
    PthreadRuntime(const PthreadRuntime&) = delete;
    PthreadRuntime& operator=(const PthreadRuntime&) = delete;
    std::int32_t create(GuestHandle* output, const void* attributes, GuestThreadEntry entry,
                        void* argument, const char* name) noexcept;
    std::int32_t join(GuestHandle handle, void** result, std::uintptr_t caller = 0) noexcept;
    std::int32_t affinity(GuestHandle,std::uint64_t*,bool set) noexcept;
    std::int32_t priority(GuestHandle,std::int32_t*,bool set) noexcept;
    std::int32_t detach(GuestHandle) noexcept;
    std::int32_t attribute_init(GuestAttribute*) noexcept;
    std::int32_t attribute_destroy(GuestAttribute*) noexcept;
    std::int32_t attribute_change(const GuestAttribute*,AttributeField,std::uint64_t) noexcept;
    std::int32_t attribute_read(const GuestAttribute*,AttributeField,void*) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class ScopedPthreadRuntime {
public:
    explicit ScopedPthreadRuntime(PthreadRuntime& runtime) noexcept;
    ~ScopedPthreadRuntime();
    ScopedPthreadRuntime(const ScopedPthreadRuntime&) = delete;
    ScopedPthreadRuntime& operator=(const ScopedPthreadRuntime&) = delete;
private:
    PthreadRuntime* previous_{};
};
std::int32_t LBP3_GUEST_ABI sce_pthread_create(GuestHandle*, const void*, GuestThreadEntry, void*, const char*) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_join(GuestHandle, void**) noexcept;
GuestHandle LBP3_GUEST_ABI sce_pthread_self() noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_equal(GuestHandle,GuestHandle) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_set_affinity(GuestHandle,std::uint64_t) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_get_affinity(GuestHandle,std::uint64_t*) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_set_priority(GuestHandle,std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_get_priority(GuestHandle,std::int32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI sce_pthread_detach(GuestHandle) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_init(GuestAttribute*) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_destroy(GuestAttribute*) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_stack(const GuestAttribute*,std::uint64_t) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_affinity(const GuestAttribute*,std::uint64_t) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_inherit(const GuestAttribute*,int) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_detach(const GuestAttribute*,int) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_policy(const GuestAttribute*,int) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_priority(const GuestAttribute*,const std::int32_t*) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_get_stack(const GuestAttribute*,std::uint64_t*) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_get_affinity(const GuestAttribute*,std::uint64_t*) noexcept;
std::int32_t LBP3_GUEST_ABI pthread_attr_get_priority(const GuestAttribute*,std::int32_t*) noexcept;
std::optional<std::uintptr_t> resolve_pthread_attributes(const HostImport&);
using PthreadCreate = decltype(&sce_pthread_create);
using PthreadJoin = decltype(&sce_pthread_join);
void pthread_self_test(std::ostream& output, PthreadCreate create = &sce_pthread_create,
                       PthreadJoin join = &sce_pthread_join);
}
