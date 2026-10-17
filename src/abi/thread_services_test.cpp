#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/guest_thread.hpp"
#include "lbp3_host/pthread.hpp"
#include "lbp3_host/heap_api.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace lbp3::abi {
namespace {
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
struct TestApi {
    decltype(&pthread_once) once;
    decltype(&key_create) create;
    decltype(&key_delete) remove;
    decltype(&set_specific) set;
    decltype(&get_specific) get;
};
TestApi api{};
std::atomic<unsigned> calls{};
GuestOnce* recursive_control{};
std::int32_t recursive_result{};
int published{};
void LBP3_GUEST_ABI initialize_once() noexcept {
    ++calls;for(unsigned i=0;i<100;++i)std::this_thread::yield();
    std::int32_t key{};
    if(api.create(&key,nullptr)||api.remove(key)){published=-1;return;}
    published=42;
}
void LBP3_GUEST_ABI recursive_once() noexcept {recursive_result=api.once(recursive_control,&recursive_once);}
struct Payload {std::int32_t key{};unsigned destructors{};bool valid{true};GuestHandle self{};};
void* LBP3_GUEST_ABI destructor_child(void* arg) noexcept {return arg;}
void LBP3_GUEST_ABI destroy_value(const void* argument) noexcept {
    auto& payload=*const_cast<Payload*>(static_cast<const Payload*>(argument));
    ++payload.destructors;
    const TlsIndex index{1,0};const auto* tls=tls_get_addr(&index);
    payload.valid=payload.valid && !api.get(payload.key) && tls && *tls==std::byte{0x42};
    payload.valid=payload.valid && sce_pthread_equal(sce_pthread_self(),payload.self);
    if(payload.destructors==1){GuestHandle child{};void* result{};
        if(sce_pthread_create(&child,nullptr,&destructor_child,&payload,"key-cleanup-child")||
           sce_pthread_join(child,&result)||result!=&payload)payload.valid=false;
    }
    // Intentionally reinstall forever: cleanup must stop after four passes.
    if(api.set(payload.key,argument))payload.valid=false;
}
void* LBP3_GUEST_ABI specific_worker(void* argument) noexcept {
    auto& payload=*static_cast<Payload*>(argument);
    payload.self=sce_pthread_self();
    payload.valid=payload.valid && !api.get(payload.key) && !api.set(payload.key,&payload) && api.get(payload.key)==&payload;
    return argument;
}
void LBP3_GUEST_ABI controller_destructor(const void* argument) noexcept {
    const TlsIndex index{1,0};const auto* bytes=tls_get_addr(&index);
    *const_cast<bool*>(static_cast<const bool*>(argument))=bytes && *bytes==std::byte{0x42} && heap_api_snapshot().has_value();
}
void* LBP3_GUEST_ABI unused_malloc(std::uint64_t) noexcept {return nullptr;}
void LBP3_GUEST_ABI unused_free(void*) noexcept {}
}
void thread_services_self_test(std::ostream& output){
    const auto get=[](const char* nid){auto result=resolve_thread_services({nid,"libkernel","libkernel",1,0x0101,2});
        if(!result)throw std::runtime_error("Missing thread services test export.");return *result;};
    api={reinterpret_cast<decltype(api.once)>(get("14bOACANTBo")),reinterpret_cast<decltype(api.create)>(get("geDaqgH9lTg")),
         reinterpret_cast<decltype(api.remove)>(get("PrdHuuDekhY")),reinterpret_cast<decltype(api.set)>(get("+BzXYkqYeLE")),
         reinterpret_cast<decltype(api.get)>(get("eoht7mQOCmo"))};
    GuestOnce control{};std::int32_t key=-1;
    if(api.once(&control,&initialize_once)!=error(22)||api.create(&key,nullptr)!=error(22))
        throw std::runtime_error("Thread services escaped context.");
    {
        ScopedThreadServices scope;
        bool rejected=false;try{ScopedThreadServices nested;}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Nested thread services accepted.");
        control.padding=0x12345678;control.mutex=0xabcdef;calls=0;published=0;
        std::array<std::thread,8> threads;std::array<bool,8> success{};std::barrier start(9);
        for(unsigned i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{start.arrive_and_wait();success[i]=!api.once(&control,&initialize_once)&&published==42;});
        start.arrive_and_wait();for(auto& thread:threads)thread.join();
        for(const auto value:success)if(!value)throw std::runtime_error("Once wait did not observe initialized data.");
        if(calls!=1||control.state!=1||control.padding!=0x12345678||control.mutex!=0xabcdef||api.once(&control,&initialize_once)||calls!=1)
            throw std::runtime_error("Once publication/layout failed.");
        GuestOnce recursive{};recursive_control=&recursive;
        if(api.once(&recursive,&recursive_once)||recursive_result!=error(11))throw std::runtime_error("Recursive once diagnostics failed.");
        GuestOnce invalid{};invalid.state=99;
        if(api.once(&invalid,&initialize_once)!=error(22)||api.once(nullptr,&initialize_once)!=error(22)||
           api.once(&invalid,nullptr)!=error(22)||api.once(reinterpret_cast<GuestOnce*>(8),&initialize_once)!=error(14))
            throw std::runtime_error("Invalid once control accepted.");
        auto* readonly=static_cast<GuestOnce*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
        if(!readonly)throw std::runtime_error("Once read-only fixture allocation failed.");
        const auto denied=api.once(readonly,&initialize_once);VirtualFree(readonly,0,MEM_RELEASE);
        if(denied!=error(14))throw std::runtime_error("Read-only once control accepted.");
        if(api.create(&key,&destroy_value)||api.set(key,&control)||api.get(key)!=&control)
            throw std::runtime_error("Key parent setup failed.");
        Payload one{key},two{key};const std::vector<TlsTemplate> templates{{1,16,16,{std::byte{0x42}}}};
        PthreadRuntime runtime(templates);ScopedPthreadRuntime runtime_scope(runtime);
        GuestHandle a{},b{};void* result_a{};void* result_b{};
        if(sce_pthread_create(&a,nullptr,&specific_worker,&one,"specific-a")||
           sce_pthread_create(&b,nullptr,&specific_worker,&two,"specific-b")||
           sce_pthread_join(a,&result_a)||sce_pthread_join(b,&result_b)||
           result_a!=&one||result_b!=&two||one.self!=a||two.self!=b||!one.valid||!two.valid||one.destructors!=4||two.destructors!=4||api.get(key)!=&control)
            throw std::runtime_error("Key destructors/TLS/isolation/lifecycle failed.");
        if(api.remove(key)||api.remove(key)!=error(22)||api.get(key)||api.set(key,&control)!=error(22))
            throw std::runtime_error("Key deletion validation failed.");
        std::int32_t reused=-1;
        if(api.create(&reused,nullptr)||reused!=key||api.get(reused)||api.remove(reused))
            throw std::runtime_error("Reused key exposed stale data.");
        if(api.create(nullptr,nullptr)!=error(22)||api.create(reinterpret_cast<std::int32_t*>(8),nullptr)!=error(14)||
           api.create(&key,reinterpret_cast<KeyDestructor>(8))!=error(14)||api.set(-1,&control)!=error(22)||api.get(256))
            throw std::runtime_error("Invalid key arguments accepted.");
        std::array<std::int32_t,256> keys{};
        for(auto& value:keys)if(api.create(&value,nullptr))throw std::runtime_error("Key capacity fixture failed.");
        std::int32_t extra=-77;
        if(api.create(&extra,nullptr)!=error(35)||extra!=-77)throw std::runtime_error("Key capacity ignored.");
        for(auto value:keys)if(api.remove(value))throw std::runtime_error("Key capacity cleanup failed.");
    }
    if(api.get(0)||api.set(0,&control)!=error(22))throw std::runtime_error("Key state survived context cleanup.");
    {ScopedThreadServices clean;if(api.create(&key,nullptr)||api.get(key)||api.remove(key))throw std::runtime_error("New key context inherited stale data.");}
    bool controller_cleaned=false;
    {const std::vector<TlsTemplate> templates{{1,16,16,{std::byte{0x42}}}};
        ThreadTls tls(templates);ScopedThreadTls attached(tls);
        ScopedHeapApi heap;
        HeapApiTable table{};table[0]=reinterpret_cast<std::uintptr_t>(&unused_malloc);table[1]=reinterpret_cast<std::uintptr_t>(&unused_free);
        if(set_application_heap_api(table.data()))throw std::runtime_error("Controller cleanup heap setup failed.");
        {ScopedThreadServices services;
            if(api.create(&key,&controller_destructor)||api.set(key,&controller_cleaned))throw std::runtime_error("Controller cleanup key setup failed.");
        }
        if(!controller_cleaned)throw std::runtime_error("Controller destructors ran after heap/TLS teardown.");
    }
    output << "Thread services self-test passed: eight-way SysV once, publication, recursion and pointer errors.\n"
              "256 keys, generation isolation, two guest workers and four destructor passes before ELF TLS detach verified.\n";
}
}
