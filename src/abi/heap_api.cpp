#include "lbp3_host/heap_api.hpp"
#include <Windows.h>
#include <atomic>
#include <bit>
#include <exception>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace lbp3::abi {
namespace {
std::mutex registry_mutex;
ScopedHeapApi* active{};
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
bool executable(std::uintptr_t address){
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
       (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
    const auto p=info.Protect&255;
    return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
}
std::atomic<unsigned> allocations{},frees{};
alignas(16) std::array<std::byte,128> fixture_buffer{};
__declspec(noinline) void* LBP3_GUEST_ABI fixture_malloc(std::uint64_t size) noexcept {
    ++allocations;
    // Calling back into the registry proves it is not locked across dispatch.
    if(!heap_api_snapshot() || size>fixture_buffer.size())return nullptr;
    return fixture_buffer.data();
}
__declspec(noinline) void LBP3_GUEST_ABI fixture_free(void* pointer) noexcept {
    if(heap_api_snapshot() && pointer==fixture_buffer.data())++frees;
}
}
ScopedHeapApi::ScopedHeapApi(){
    std::lock_guard lock(registry_mutex);
    if(active)throw std::runtime_error("An application heap context is already active.");
    active=this;
}
ScopedHeapApi::~ScopedHeapApi(){std::lock_guard lock(registry_mutex);active=nullptr;}
std::optional<HeapApiTable> heap_api_snapshot(){
    std::lock_guard lock(registry_mutex);return active ? active->installed_ : std::nullopt;
}
std::int32_t LBP3_GUEST_ABI set_application_heap_api(const std::uintptr_t* source) noexcept {
    try{
        std::lock_guard lock(registry_mutex);
        if(!active || !source)return error(22);
        HeapApiTable candidate{};SIZE_T copied{};
        if(!ReadProcessMemory(GetCurrentProcess(),source,candidate.data(),sizeof(candidate),&copied) ||
           copied!=sizeof(candidate))return error(14);
        if(!candidate[0] || !candidate[1])return error(22);
        for(const auto address:candidate)if(address && !executable(address))return error(14);
        active->installed_=candidate;return 0; // Atomic publication of the validated copy.
    }catch(...){return error(5);}
}

void heap_api_self_test(std::ostream& output,std::uintptr_t setter){
    using Setter=std::int32_t (LBP3_GUEST_ABI *)(const std::uintptr_t*) noexcept;
    using Malloc=void* (LBP3_GUEST_ABI *)(std::uint64_t) noexcept;
    using Free=void (LBP3_GUEST_ABI *)(void*) noexcept;
    Setter volatile install=reinterpret_cast<Setter>(setter);
    HeapApiTable table{};table[0]=reinterpret_cast<std::uintptr_t>(&fixture_malloc);
    table[1]=reinterpret_cast<std::uintptr_t>(&fixture_free);
    if(install(table.data())!=error(22)||heap_api_snapshot())throw std::runtime_error("Heap API escaped its context.");
    allocations=0;frees=0;
    {
        ScopedHeapApi scope;
        bool rejected=false;try{ScopedHeapApi nested;}catch(const std::runtime_error&){rejected=true;}
        if(!rejected || heap_api_snapshot())throw std::runtime_error("Heap API context isolation failed.");
        if(install(table.data()) || heap_api_snapshot()!=table || allocations || frees)
            throw std::runtime_error("Heap API registration/copy failed or invoked callbacks.");
        const auto expected=table;
        table.fill(0); // Source storage can disappear; only callback code must remain alive.
        if(heap_api_snapshot()!=expected)throw std::runtime_error("Heap API retained borrowed table storage.");
        auto bad=expected;bad[1]=0;
        if(install(nullptr)!=error(22)||install(reinterpret_cast<const std::uintptr_t*>(1))!=error(14)||
           install(bad.data())!=error(22))throw std::runtime_error("Invalid heap table accepted.");
        bad=expected;bad[2]=reinterpret_cast<std::uintptr_t>(fixture_buffer.data());
        if(install(bad.data())!=error(14)||heap_api_snapshot()!=expected)
            throw std::runtime_error("Nonexecutable heap callback accepted or valid state lost.");
        auto* boundary=static_cast<std::byte*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        if(!boundary)throw std::runtime_error("Heap boundary fixture allocation failed.");
        DWORD old{};
        const bool protected_page=VirtualProtect(boundary+4096,4096,PAGE_NOACCESS,&old)!=FALSE;
        const auto result=protected_page ? install(reinterpret_cast<std::uintptr_t*>(boundary+4096-8)) : 0;
        VirtualFree(boundary,0,MEM_RELEASE);
        if(!protected_page || result!=error(14)||heap_api_snapshot()!=expected)
            throw std::runtime_error("Truncated heap table accepted.");
        std::exception_ptr worker_error;
        std::thread worker([&]{try{
            const auto snapshot=heap_api_snapshot();
            if(snapshot!=expected)throw std::runtime_error("Worker cannot see installed heap.");
            Malloc volatile allocate=reinterpret_cast<Malloc>((*snapshot)[0]);
            Free volatile release=reinterpret_cast<Free>((*snapshot)[1]);
            auto* pointer=static_cast<std::byte*>(allocate(32));
            if(pointer!=fixture_buffer.data())throw std::runtime_error("SysV heap callback returned wrong pointer.");
            pointer[31]=std::byte{0x5a};release(pointer);
        }catch(...){worker_error=std::current_exception();}});worker.join();
        if(worker_error)std::rethrow_exception(worker_error);
        if(allocations!=1||frees!=1||fixture_buffer[31]!=std::byte{0x5a})throw std::runtime_error("Heap callback roundtrip failed.");
        // Replacement is supported; null optional slots stay null.
        bad=expected;bad[9]=expected[0];
        if(install(bad.data())||heap_api_snapshot()!=bad)throw std::runtime_error("Heap API replacement failed.");
    }
    if(heap_api_snapshot())throw std::runtime_error("Heap API survived scope teardown.");
    {ScopedHeapApi clean;if(heap_api_snapshot())throw std::runtime_error("Heap API leaked into next context.");}
    output << "Heap API self-test passed: scoped registration, copied table, optional slots, invalid pointers and atomic replacement.\n"
              "Synthetic SysV malloc/free on worker, reentrant registry access and teardown verified. No libc code executed.\n";
}
}
