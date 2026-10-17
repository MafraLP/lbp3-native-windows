#include "lbp3_host/pthread.hpp"
#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/wait_trace.hpp"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <future>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <map>

namespace lbp3::abi {
namespace {
thread_local PthreadRuntime* active_runtime{};
thread_local GuestHandle own_handle{};
std::atomic<std::uint64_t> host_identity{1};
std::atomic<std::uint64_t> attribute_identity{1};
constexpr std::int32_t error(unsigned code) { return std::bit_cast<std::int32_t>(0x80020000u|code); }
template<class T> bool copy_in(const T* from, T& value) noexcept {
    SIZE_T bytes{};
    return from && ReadProcessMemory(GetCurrentProcess(),from,&value,sizeof(T),&bytes) && bytes==sizeof(T);
}
template<class T> bool copy_out(T* to, const T& value) noexcept {
    const auto start=reinterpret_cast<std::uintptr_t>(to);
    if (!start || start>UINTPTR_MAX-sizeof(T)) return false;
    auto cursor=start;
    while (cursor<start+sizeof(T)) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto p=info.Protect & 0xff;
        if (p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY) return false;
        const auto region=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize>UINTPTR_MAX-region || region+info.RegionSize<=cursor) return false;
        cursor=region+info.RegionSize;
    }
    SIZE_T bytes{};
    return to && WriteProcessMemory(GetCurrentProcess(),to,&value,sizeof(T),&bytes) && bytes==sizeof(T);
}
template<class T> bool writable(T* address) noexcept {
    T value{};
    return copy_in(address,value) && copy_out(address,value);
}
bool executable(GuestThreadEntry fn) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!fn || !VirtualQuery(reinterpret_cast<void*>(fn),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD|PAGE_NOACCESS))) return false;
    const auto p = info.Protect & 0xff;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}
std::int32_t apply_affinity(HANDLE native,std::uint64_t mask){
    DWORD_PTR process{},system{};if(!GetProcessAffinityMask(GetCurrentProcess(),&process,&system)||!process)return error(45);
    std::array<DWORD_PTR,64> cpus{};std::size_t count{};
    for(unsigned bit=0;bit<64;++bit)if(process&(DWORD_PTR{1}<<bit))cpus[count++]=DWORD_PTR{1}<<bit;
    DWORD_PTR mapped{};for(unsigned bit=0;bit<8;++bit)if(mask&(1ull<<bit))mapped|=cpus[bit%count];
    if(!SetThreadAffinityMask(native,mapped))return error(GetLastError()==ERROR_ACCESS_DENIED?13:22);
    return 0;
}
int native_priority(int value,int policy){return policy==2?THREAD_PRIORITY_NORMAL:value<400?THREAD_PRIORITY_HIGHEST:
    value<512?THREAD_PRIORITY_ABOVE_NORMAL:value<704?THREAD_PRIORITY_NORMAL:value<736?THREAD_PRIORITY_BELOW_NORMAL:THREAD_PRIORITY_LOWEST;}
}

ScopedPthreadRuntime::ScopedPthreadRuntime(PthreadRuntime& runtime) noexcept : previous_(active_runtime) { active_runtime=&runtime; }
ScopedPthreadRuntime::~ScopedPthreadRuntime() { active_runtime=previous_; }

struct PthreadRuntime::Impl {
    struct Record {
        PthreadRuntime* runtime{};
        GuestHandle handle{};
        GuestThreadEntry entry{};
        void* argument{};
        std::promise<bool> gate;
        std::future<bool> release{gate.get_future()};
        std::unique_ptr<GuestThread> worker;
        HANDLE affinity_handle{};
        ~Record(){if(affinity_handle)CloseHandle(affinity_handle);}
        bool joining{};
        std::uint64_t affinity{0x7f};
        std::int32_t priority{700};
        int policy{1};bool detached{};
    };
    struct Slot { std::uint32_t generation{1}; std::shared_ptr<Record> record; };
    std::mutex mutex;
    std::array<Slot,64> slots;
    std::vector<TlsTemplate> templates;
    bool closing{};
    GuestHandle controller{};HANDLE controller_thread{};GROUP_AFFINITY original_affinity{};
    std::uint64_t controller_affinity{0x7f};
    std::int32_t controller_priority{700};int original_priority{THREAD_PRIORITY_NORMAL};
    std::map<GuestAttribute,PthreadAttributes> attributes;
    static void* LBP3_GUEST_ABI enter(void* value) noexcept {
        auto& record = *static_cast<Record*>(value);
        try {
            if (!record.release.get()) return nullptr;
            ScopedPthreadRuntime scope(*record.runtime);
            const auto previous = own_handle;
            own_handle=record.handle;
            auto* result=record.entry(record.argument);
            cleanup_thread_specific(); // Keep runtime and identity valid inside key destructors.
            own_handle=previous;
            return result;
        } catch (...) { return nullptr; }
    }
};

PthreadRuntime::PthreadRuntime(std::span<const TlsTemplate> templates) : impl_(std::make_unique<Impl>()) {
    impl_->templates.assign(templates.begin(),templates.end());
    ThreadTls validate(impl_->templates);
    impl_->controller=sce_pthread_self();
    if(!GetThreadGroupAffinity(GetCurrentThread(),&impl_->original_affinity)||
       !DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&impl_->controller_thread,0,FALSE,DUPLICATE_SAME_ACCESS))
        throw std::runtime_error("Cannot retain pthread controller handle.");
    if(apply_affinity(impl_->controller_thread,impl_->controller_affinity)){
        CloseHandle(impl_->controller_thread);impl_->controller_thread=nullptr;
        throw std::runtime_error("Cannot apply initial guest controller affinity.");
    }
    impl_->original_priority=GetThreadPriority(impl_->controller_thread);
    if(impl_->original_priority==THREAD_PRIORITY_ERROR_RETURN||!SetThreadPriority(impl_->controller_thread,THREAD_PRIORITY_NORMAL)){
        SetThreadGroupAffinity(impl_->controller_thread,&impl_->original_affinity,nullptr);
        CloseHandle(impl_->controller_thread);impl_->controller_thread=nullptr;
        throw std::runtime_error("Cannot initialize guest controller priority.");
    }
}
PthreadRuntime::~PthreadRuntime() {
    std::array<std::shared_ptr<Impl::Record>,64> records;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closing=true;
        for (std::size_t i=0;i<records.size();++i) records[i]=std::move(impl_->slots[i].record);
    }
    // Destruction waits outside the table lock; no detached or leaked workers.
    for (auto& record : records) if (record && record->worker && !record->joining) {
        try { (void)record->worker->join(); } catch (...) {}
    }
    if(impl_->controller_thread){SetThreadGroupAffinity(impl_->controller_thread,&impl_->original_affinity,nullptr);SetThreadPriority(impl_->controller_thread,impl_->original_priority);CloseHandle(impl_->controller_thread);}
}
std::int32_t PthreadRuntime::priority(GuestHandle handle,std::int32_t* value,bool set) noexcept {
    try{std::lock_guard lock(impl_->mutex);HANDLE native{};std::int32_t* stored{};
        if(handle==impl_->controller){native=impl_->controller_thread;stored=&impl_->controller_priority;}
        else for(auto& slot:impl_->slots)if(slot.record&&slot.record->handle==handle){native=slot.record->affinity_handle;stored=&slot.record->priority;break;}
        if(!native||!stored)return error(3);
        if(!set)return copy_out(value,*stored)?0:error(14);
        int policy=1;for(const auto& slot:impl_->slots)if(slot.record&&slot.record->handle==handle)policy=slot.record->policy;
        if(!value||*value<(policy==2?768:256)||*value>(policy==2?959:767))return error(22);
        // Windows offers five ordinary priority levels, not the PS4's full
        // real-time scheduler. Preserve the guest value and apply its bucket.
        const int mapped=native_priority(*value,policy);
        if(!SetThreadPriority(native,mapped))return error(GetLastError()==ERROR_ACCESS_DENIED?13:3);
        *stored=*value;return 0;
    }catch(...){return error(5);}
}
std::int32_t PthreadRuntime::affinity(GuestHandle handle,std::uint64_t* mask,bool set) noexcept {
    try{std::lock_guard lock(impl_->mutex);HANDLE native{};std::uint64_t* stored{};
        if(handle==impl_->controller){native=impl_->controller_thread;stored=&impl_->controller_affinity;}
        else for(auto& slot:impl_->slots)if(slot.record&&slot.record->handle==handle){
            native=slot.record->affinity_handle;stored=&slot.record->affinity;break;
        }
        if(!native||!stored)return error(3);
        if(!set)return copy_out(mask,*stored)?0:error(14);
        if(!mask||!*mask||(*mask&~0xffull))return error(22);
        const auto status=apply_affinity(native,*mask);if(status)return status;
        *stored=*mask;return 0;
    }catch(...){return error(5);}
}

std::int32_t PthreadRuntime::create(GuestHandle* output, const void* attributes, GuestThreadEntry entry,
                                  void* argument, const char* name) noexcept {
    if (!output || !entry) return error(22);
    if (!executable(entry)) return error(22);
    if (!writable(output)) return error(14);
    // Name is optional metadata; reject inaccessible/unterminated input instead of dereferencing it.
    if (name) {
        bool terminated=false;
        for (std::size_t i=0;i<32;++i) {
            char c{};
            if (!copy_in(name+i,c)) return error(14);
            if (!c) { terminated=true; break; }
        }
        if (!terminated) return error(22);
    }
    try {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closing) return error(35);
        PthreadAttributes requested;
        if(attributes){GuestAttribute id{};if(!copy_in(static_cast<const GuestAttribute*>(attributes),id))return error(14);
            const auto found=impl_->attributes.find(id);if(found==impl_->attributes.end())return error(22);requested=found->second;
        }
        if(requested.inherit==4){requested.priority=impl_->controller_priority;requested.policy=1;
            for(const auto& slot:impl_->slots)if(slot.record&&slot.record->handle==own_handle){requested.priority=slot.record->priority;requested.policy=slot.record->policy;break;}
        }
        // Detached guest threads retain an owned native record until completion;
        // reap only after the native handle is signaled, never while CRT/TLS exits.
        for(auto& slot:impl_->slots)if(slot.record&&slot.record->detached&&WaitForSingleObject(slot.record->affinity_handle,0)==WAIT_OBJECT_0){
            (void)slot.record->worker->join();slot.record.reset();++slot.generation;
        }
        for (std::size_t i=0;i<impl_->slots.size();++i) {
            auto& slot=impl_->slots[i];
            if (slot.record || !slot.generation) continue;
            auto record=std::make_shared<Impl::Record>();
            record->runtime=this;
            record->handle=(static_cast<GuestHandle>(slot.generation)<<32)|(i+1);
            record->entry=entry; record->argument=argument;
            record->affinity=requested.affinity;record->priority=requested.priority;record->policy=requested.policy;record->detached=requested.detached!=0;
            slot.record=record;
            try {
                record->worker=std::make_unique<GuestThread>(impl_->templates,&Impl::enter,record.get(),requested.stack_size);
                const bool retained=DuplicateHandle(GetCurrentProcess(),record->worker->native_handle(),GetCurrentProcess(),&record->affinity_handle,0,FALSE,DUPLICATE_SAME_ACCESS)!=0;
                const auto pinned=retained?apply_affinity(record->affinity_handle,record->affinity):error(35);
                if(pinned){record->gate.set_value(false);(void)record->worker->join();slot.record.reset();++slot.generation;return pinned;}
                if(!SetThreadPriority(record->affinity_handle,native_priority(record->priority,record->policy))){record->gate.set_value(false);(void)record->worker->join();slot.record.reset();++slot.generation;return error(13);}
                if (!copy_out(output,record->handle)) {
                    record->gate.set_value(false);
                    (void)record->worker->join();
                    slot.record.reset(); ++slot.generation;
                    return error(14);
                }
                record->gate.set_value(true);
                return 0;
            } catch (...) {
                slot.record.reset(); ++slot.generation;
                throw;
            }
        }
        return error(35);
    } catch (const std::bad_alloc&) { return error(12); }
    catch (...) { return error(35); }
}

std::int32_t PthreadRuntime::join(GuestHandle handle, void** result,std::uintptr_t caller) noexcept {
    if (handle && handle==own_handle && active_runtime==this) return error(11);
    const auto index=static_cast<std::uint32_t>(handle);
    const auto generation=static_cast<std::uint32_t>(handle>>32);
    if (!index || index>impl_->slots.size() || !generation) return error(3);
    if (result && !writable(result)) return error(14);
    try {
        std::shared_ptr<Impl::Record> record;
        {
            std::lock_guard lock(impl_->mutex);
            auto& slot=impl_->slots[index-1];
            if (slot.generation!=generation || !slot.record) return error(3);
            record=slot.record;
            if(record->detached)return error(22);
            if (record->joining) return error(22);
            record->joining=true;
        }
        void* value{};
        std::int32_t status{};
        const auto blocked_at=std::chrono::steady_clock::now();
        try { value=record->worker->join(); }
        catch (...) { status=error(5); }
        diagnostics::guest_wait_trace("pthread_join",handle,UINT64_MAX,status,
            std::chrono::steady_clock::now()-blocked_at,caller);
        if (!status && result && !copy_out(result,value)) status=error(14);
        {
            std::lock_guard lock(impl_->mutex);
            auto& slot=impl_->slots[index-1];
            slot.record.reset(); ++slot.generation; // Zero generation retires a wrapped slot.
        }
        return status;
    } catch (...) { return error(5); }
}

std::int32_t LBP3_GUEST_ABI sce_pthread_create(GuestHandle* output, const void* attr, GuestThreadEntry entry,
                                             void* arg, const char* name) noexcept {
    return active_runtime ? active_runtime->create(output,attr,entry,arg,name) : error(22);
}
std::int32_t LBP3_GUEST_ABI sce_pthread_join(GuestHandle handle, void** result) noexcept {
    return active_runtime ? active_runtime->join(handle,result,reinterpret_cast<std::uintptr_t>(_ReturnAddress())) : error(22);
}
GuestHandle LBP3_GUEST_ABI sce_pthread_self() noexcept {
    if(!own_handle)own_handle=0xf000000000000000ull|host_identity.fetch_add(1);
    return own_handle;
}
std::int32_t LBP3_GUEST_ABI sce_pthread_equal(GuestHandle a,GuestHandle b) noexcept {return a==b?1:0;}
std::int32_t LBP3_GUEST_ABI sce_pthread_set_affinity(GuestHandle handle,std::uint64_t mask) noexcept {
    return active_runtime?active_runtime->affinity(handle,&mask,true):error(22);
}
std::int32_t LBP3_GUEST_ABI sce_pthread_get_affinity(GuestHandle handle,std::uint64_t* mask) noexcept {
    return active_runtime?active_runtime->affinity(handle,mask,false):error(22);
}
std::int32_t LBP3_GUEST_ABI sce_pthread_set_priority(GuestHandle handle,std::int32_t value) noexcept {
    return active_runtime?active_runtime->priority(handle,&value,true):error(22);
}
std::int32_t LBP3_GUEST_ABI sce_pthread_get_priority(GuestHandle handle,std::int32_t* value) noexcept {
    return active_runtime?active_runtime->priority(handle,value,false):error(22);
}
std::int32_t PthreadRuntime::detach(GuestHandle handle) noexcept {
    try{std::lock_guard lock(impl_->mutex);for(auto& slot:impl_->slots)if(slot.record&&slot.record->handle==handle){
        if(slot.record->detached||slot.record->joining)return error(22);slot.record->detached=true;return 0;
    }return error(3);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI sce_pthread_detach(GuestHandle handle) noexcept {return active_runtime?active_runtime->detach(handle):error(22);}
std::int32_t PthreadRuntime::attribute_init(GuestAttribute* output) noexcept {
    try{std::lock_guard lock(impl_->mutex);if(!output)return error(22);if(!writable(output))return error(14);
        GuestAttribute old{};if(!copy_in(output,old))return error(14);if(impl_->attributes.contains(old))return error(16);
        if(impl_->attributes.size()>=64)return error(12);const auto sequence=attribute_identity.fetch_add(1);
        if(sequence>0xffffffffull)return error(12);const auto id=0x4500000000000000ull|sequence;
        impl_->attributes.emplace(id,PthreadAttributes{});
        if(!copy_out(output,id)){impl_->attributes.erase(id);return error(14);}return 0;
    }catch(...){return error(12);}
}
std::int32_t PthreadRuntime::attribute_destroy(GuestAttribute* pointer) noexcept {
    try{std::lock_guard lock(impl_->mutex);GuestAttribute id{};if(!pointer)return error(22);if(!copy_in(pointer,id))return error(14);
        const auto found=impl_->attributes.find(id);if(found==impl_->attributes.end())return error(22);
        if(!copy_out(pointer,GuestAttribute{}))return error(14);impl_->attributes.erase(found);return 0;
    }catch(...){return error(5);}
}
std::int32_t PthreadRuntime::attribute_change(const GuestAttribute* pointer,AttributeField field,std::uint64_t value) noexcept {
    try{std::lock_guard lock(impl_->mutex);GuestAttribute id{};if(!pointer)return error(22);if(!copy_in(pointer,id))return error(14);
        const auto found=impl_->attributes.find(id);if(found==impl_->attributes.end())return error(22);auto& attr=found->second;
        switch(field){
        case AttributeField::StackSize:if(value<0x4000||value>64ull*1024*1024)return error(22);attr.stack_size=value;break;
        case AttributeField::Affinity:if(!value||(value&~0xffull))return error(22);attr.affinity=value;break;
        case AttributeField::Inherit:if(value!=0&&value!=4)return error(45);attr.inherit=static_cast<int>(value);break;
        case AttributeField::Detach:if(value>1)return error(22);attr.detached=static_cast<int>(value);break;
        case AttributeField::Policy:if(value<1||value>3)return error(45);attr.policy=static_cast<int>(value);attr.priority=value==2?900:700;break;
        case AttributeField::Priority:if(value<static_cast<std::uint64_t>(attr.policy==2?768:256)||value>static_cast<std::uint64_t>(attr.policy==2?959:767))return error(45);attr.priority=static_cast<int>(value);break;
        }return 0;
    }catch(...){return error(5);}
}
std::int32_t PthreadRuntime::attribute_read(const GuestAttribute* pointer,AttributeField field,void* output) noexcept {
    try{std::lock_guard lock(impl_->mutex);GuestAttribute id{};if(!pointer)return error(22);if(!copy_in(pointer,id))return error(14);
        const auto found=impl_->attributes.find(id);if(found==impl_->attributes.end())return error(22);const auto& attr=found->second;
        if(field==AttributeField::StackSize||field==AttributeField::Affinity)return copy_out(static_cast<std::uint64_t*>(output),field==AttributeField::StackSize?attr.stack_size:attr.affinity)?0:error(14);
        const std::int32_t value=field==AttributeField::Priority?attr.priority:field==AttributeField::Policy?attr.policy:field==AttributeField::Detach?attr.detached:attr.inherit;
        return copy_out(static_cast<std::int32_t*>(output),value)?0:error(14);
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI pthread_attr_init(GuestAttribute* output) noexcept {return active_runtime?active_runtime->attribute_init(output):error(22);}
std::int32_t LBP3_GUEST_ABI pthread_attr_destroy(GuestAttribute* pointer) noexcept {return active_runtime?active_runtime->attribute_destroy(pointer):error(22);}
#define ATTR_SET(name,field,type) std::int32_t LBP3_GUEST_ABI name(const GuestAttribute* pointer,type value) noexcept {return active_runtime?active_runtime->attribute_change(pointer,AttributeField::field,static_cast<std::uint64_t>(value)):error(22);}
ATTR_SET(pthread_attr_stack,StackSize,std::uint64_t)
ATTR_SET(pthread_attr_affinity,Affinity,std::uint64_t)
ATTR_SET(pthread_attr_inherit,Inherit,int)
ATTR_SET(pthread_attr_detach,Detach,int)
ATTR_SET(pthread_attr_policy,Policy,int)
#undef ATTR_SET
std::int32_t LBP3_GUEST_ABI pthread_attr_priority(const GuestAttribute* pointer,const std::int32_t* input) noexcept {
    if(!active_runtime)return error(22);std::int32_t value{};if(!copy_in(input,value))return error(14);
    return active_runtime->attribute_change(pointer,AttributeField::Priority,static_cast<std::uint64_t>(value));
}
#define ATTR_GET(name,field,type) std::int32_t LBP3_GUEST_ABI name(const GuestAttribute* pointer,type* output) noexcept {return active_runtime?active_runtime->attribute_read(pointer,AttributeField::field,output):error(22);}
ATTR_GET(pthread_attr_get_stack,StackSize,std::uint64_t)
ATTR_GET(pthread_attr_get_affinity,Affinity,std::uint64_t)
ATTR_GET(pthread_attr_get_priority,Priority,std::int32_t)
#undef ATTR_GET
std::optional<std::uintptr_t> resolve_pthread_attributes(const HostImport& r){
    if(r.type!=2||r.library!="libkernel"||r.module!="libkernel"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="nsYoNRywwNg")return reinterpret_cast<std::uintptr_t>(&pthread_attr_init);
    if(r.nid=="62KCwEMmzcM")return reinterpret_cast<std::uintptr_t>(&pthread_attr_destroy);
    if(r.nid=="UTXzJbWhhTE"||r.nid=="Bvn74vj6oLo")return reinterpret_cast<std::uintptr_t>(&pthread_attr_stack);
    if(r.nid=="3qxgM4ezETA")return reinterpret_cast<std::uintptr_t>(&pthread_attr_affinity);
    if(r.nid=="eXbUSpEaTsA")return reinterpret_cast<std::uintptr_t>(&pthread_attr_inherit);
    if(r.nid=="-Wreprtu0Qs")return reinterpret_cast<std::uintptr_t>(&pthread_attr_detach);
    if(r.nid=="4+h9EzwKF4I")return reinterpret_cast<std::uintptr_t>(&pthread_attr_policy);
    if(r.nid=="DzES9hQF4f4")return reinterpret_cast<std::uintptr_t>(&pthread_attr_priority);
    if(r.nid=="-fA+7ZlGDQs")return reinterpret_cast<std::uintptr_t>(&pthread_attr_get_stack);
    if(r.nid=="8+s5BzZjxSg")return reinterpret_cast<std::uintptr_t>(&pthread_attr_get_affinity);
    if(r.nid=="FXPWHNk8Of0")return reinterpret_cast<std::uintptr_t>(&pthread_attr_get_priority);
    if(r.nid=="4qGrR6eoP9Y")return reinterpret_cast<std::uintptr_t>(&sce_pthread_detach);
    return std::nullopt;
}

namespace {
struct Test {
    GuestHandle handle{};
    std::barrier<>* barrier{};
    unsigned value{};
    std::uintptr_t address{};
    std::int32_t self_join{};
    bool valid{};
};
void* LBP3_GUEST_ABI test_entry(void* argument) noexcept {
    auto& t=*static_cast<Test*>(argument);
    const TlsIndex index{1,0};
    auto* p=tls_get_addr(&index);
    bool valid=p && p[0]==std::byte{0x42} && p[1]==std::byte{};
    t.address=reinterpret_cast<std::uintptr_t>(p);
    if (p) *p=static_cast<std::byte>(t.value);
    t.self_join=sce_pthread_join(t.handle,nullptr);
    t.barrier->arrive_and_wait();
    t.valid=valid && *tls_get_addr(&index)==static_cast<std::byte>(t.value);
    return argument;
}
void* LBP3_GUEST_ABI identity(void* argument) noexcept { return argument; }
void* LBP3_GUEST_ABI mark_completed(void* argument) noexcept {
    static_cast<std::atomic<bool>*>(argument)->store(true);
    return argument;
}
struct Blocking { std::shared_future<void> release; };
void* LBP3_GUEST_ABI blocked(void* argument) noexcept {
    static_cast<Blocking*>(argument)->release.wait();
    return argument;
}
struct Nested { bool valid{}; };
void* LBP3_GUEST_ABI nested_child(void* argument) noexcept {
    const TlsIndex index{1,0};
    auto* p=tls_get_addr(&index);
    static_cast<Nested*>(argument)->valid=p && *p==std::byte{0x42};
    if (p) *p=std::byte{0x25};
    return argument;
}
void* LBP3_GUEST_ABI nested_parent(void* argument) noexcept {
    const TlsIndex index{1,0};
    auto* p=tls_get_addr(&index);
    if (!p) return nullptr;
    *p=std::byte{0x55};
    GuestHandle child{}; void* result{};
    if (sce_pthread_create(&child,nullptr,&nested_child,argument,nullptr) ||
        sce_pthread_join(child,&result) || result!=argument || *p!=std::byte{0x55}) return nullptr;
    return argument;
}
}

void pthread_self_test(std::ostream& output, PthreadCreate create_entry, PthreadJoin join_entry) {
    using Create=decltype(&sce_pthread_create);
    using Join=decltype(&sce_pthread_join);
    Create volatile create=create_entry;
    Join volatile join=join_entry;
    const std::vector<TlsTemplate> templates{{1,32,32,{std::byte{0x42}}}};
    GuestHandle dummy{};
    if (create(&dummy,nullptr,&identity,nullptr,nullptr)!=error(22)) throw std::runtime_error("Missing runtime accepted.");
    PthreadRuntime runtime(templates);
    ScopedPthreadRuntime attached(runtime);
    const auto self=sce_pthread_self();
    if(!self||sce_pthread_self()!=self||!sce_pthread_equal(self,self)||sce_pthread_equal(self,0)||join(self,nullptr)!=error(11))
        throw std::runtime_error("Pthread controller identity/self-join failed.");
    ThreadTls parent(templates);
    ScopedThreadTls parent_scope(parent);
    const TlsIndex index{1,0};
    auto* parent_data=tls_get_addr(&index);
    *parent_data=std::byte{0x77};
    std::barrier barrier(2);
    Test a{0,&barrier,1}, b{0,&barrier,2};
    if (create(&a.handle,nullptr,&test_entry,&a,"first") || create(&b.handle,nullptr,&test_entry,&b,"second"))
        throw std::runtime_error("scePthreadCreate failed.");
    void* ra{}; void* rb{};
    if (join(a.handle,&ra) || join(b.handle,&rb) || ra!=&a || rb!=&b || !a.valid || !b.valid ||
        a.address==b.address || a.address==reinterpret_cast<std::uintptr_t>(parent_data) ||
        a.self_join!=error(11) || b.self_join!=error(11) || *parent_data!=std::byte{0x77})
        throw std::runtime_error("scePthreadJoin/TLS/self-join test failed.");
    if (join(a.handle,nullptr)!=error(3) || join(0,nullptr)!=error(3) ||
        create(nullptr,nullptr,&identity,nullptr,nullptr)!=error(22) ||
        create(&dummy,nullptr,nullptr,nullptr,nullptr)!=error(22) ||
        create(&dummy,reinterpret_cast<void*>(1),&identity,nullptr,nullptr)!=error(14) ||
        create(reinterpret_cast<GuestHandle*>(1),nullptr,&identity,nullptr,nullptr)!=error(14))
        throw std::runtime_error("scePthread invalid-argument/handle error test failed.");
    auto* read_only=static_cast<GuestHandle*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
    if (!read_only) throw std::runtime_error("Read-only output fixture allocation failed.");
    const auto read_only_status=create(read_only,nullptr,&identity,nullptr,nullptr);
    VirtualFree(read_only,0,MEM_RELEASE);
    if (read_only_status!=error(14) ||
        create(&dummy,nullptr,&identity,nullptr,reinterpret_cast<const char*>(1))!=error(14) ||
        create(&dummy,nullptr,reinterpret_cast<GuestThreadEntry>(1),nullptr,nullptr)!=error(22))
        throw std::runtime_error("Pthread pointer validation failed.");
    GuestHandle reused{};
    if (create(&reused,nullptr,&identity,&dummy,nullptr) || reused==a.handle || join(a.handle,nullptr)!=error(3) ||
        join(reused,reinterpret_cast<void**>(1))!=error(14) || join(reused,nullptr))
        throw std::runtime_error("Generation reuse or failed-output join test failed.");
    std::array<GuestHandle,64> handles{};
    for (auto& handle:handles) if (create(&handle,nullptr,&identity,nullptr,nullptr)) throw std::runtime_error("Thread table allocation failed.");
    if (create(&dummy,nullptr,&identity,nullptr,nullptr)!=error(35)) throw std::runtime_error("Thread table limit was ignored.");
    for (auto handle:handles) if (join(handle,nullptr)) throw std::runtime_error("Thread table cleanup failed.");
    Nested nested;
    GuestHandle parent_handle{}; void* nested_result{};
    if (create(&parent_handle,nullptr,&nested_parent,&nested,nullptr) || join(parent_handle,&nested_result) ||
        nested_result!=&nested || !nested.valid) throw std::runtime_error("Nested pthread create/join failed.");
    // Both join callers start together. The rejected caller signals the controller
    // while the winning caller is still waiting for the blocked target.
    std::promise<void> finish, rejected_join;
    Blocking blocking{finish.get_future().share()};
    auto rejected_ready=rejected_join.get_future();
    GuestHandle target{};
    if (create(&target,nullptr,&blocked,&blocking,nullptr)) throw std::runtime_error("Concurrent join setup failed.");
    std::barrier join_start(3);
    std::int32_t statuses[2]{};
    const auto waiter=[&](int i) {
        ScopedPthreadRuntime context(runtime);
        join_start.arrive_and_wait();
        statuses[i]=join(target,nullptr);
        if (statuses[i]==error(22)) rejected_join.set_value();
    };
    std::thread first_waiter(waiter,0), second_waiter(waiter,1);
    join_start.arrive_and_wait();
    const bool concurrent_rejected=rejected_ready.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    finish.set_value();
    first_waiter.join(); second_waiter.join();
    if (!concurrent_rejected || !((statuses[0]==0 && statuses[1]==error(22)) ||
                                 (statuses[1]==0 && statuses[0]==error(22))))
        throw std::runtime_error("Concurrent join was not rejected correctly.");
    std::atomic<bool> cleaned{};
    {
        PthreadRuntime scoped(templates);
        ScopedPthreadRuntime context(scoped);
        if (create(&dummy,nullptr,&mark_completed,&cleaned,nullptr))
            throw std::runtime_error("Runtime cleanup setup failed.");
    }
    if (!cleaned.load()) throw std::runtime_error("Unjoined pthread was abandoned at runtime destruction.");
    output << "Pthread API self-test passed: scePthreadCreate/Join via SysV, handles, results and per-thread TLS.\n"
              "Stale handles, self/concurrent join, nested creation, bad pointers, attributes and resource limits verified.\n"
              "Synthetic entries only. No game code was executed.\n";
}
}
