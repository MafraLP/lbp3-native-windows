#include "lbp3_host/hle.hpp"
#include "lbp3_host/system_service.hpp"
#include "lbp3_host/np_manager.hpp"
#include "lbp3_host/audio_out.hpp"
#include "lbp3_host/np_trophy.hpp"
#include "lbp3_host/network_control.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/tls.hpp"
#include "lbp3_host/pthread.hpp"
#include "lbp3_host/process_param.hpp"
#include "lbp3_host/mutex.hpp"
#include "lbp3_host/heap_api.hpp"
#include "lbp3_host/flexible_memory.hpp"
#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/condition.hpp"
#include "lbp3_host/semaphore.hpp"
#include "lbp3_host/clock_services.hpp"
#include "lbp3_host/companion_httpd.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/video_out.hpp"
#include "lbp3_host/memory_services.hpp"
#include "lbp3_host/compute_queue.hpp"
#include "lbp3_host/gnm_commands.hpp"
#include "lbp3_host/iss_backend.hpp"
#include "lbp3_host/event_queue.hpp"
#include "lbp3_host/event_flags.hpp"
#include "lbp3_host/pad_services.hpp"
#include "lbp3_host/live_streaming.hpp"
#include "lbp3_host/ime_dialog.hpp"
#include "lbp3_host/signal_mask.hpp"
#include "lbp3_host/module_registry.hpp"
#include "lbp3_host/filesystem_exports.hpp"
#include "lbp3_host/guest_filesystem.hpp"
#include <array>
#include <barrier>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <thread>
#include <Windows.h>
#include <bcrypt.h>
#include <bit>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <system_error>
#include <atomic>

namespace lbp3::hle {
namespace {
LARGE_INTEGER epoch{}, frequency{};
bool ready{};
std::uintptr_t stack_guard{};
char progname_storage[]="eboot.bin";
char* progname=progname_storage;
thread_local std::int32_t guest_errno{};
[[noreturn]] void terminate_diagnostic(const char* service,std::uint32_t guest_code,std::uint32_t exit_code) noexcept {
    char buffer[192]{};
    const int length=std::snprintf(buffer,sizeof(buffer),"Guest diagnostic stop: %s code=0x%08x; no ELF unwinding.\n",service,guest_code);
    DWORD written{};
    if(length>0)(void)WriteFile(GetStdHandle(STD_ERROR_HANDLE),buffer,
        static_cast<DWORD>(length<static_cast<int>(sizeof(buffer))?length:sizeof(buffer)-1),&written,nullptr);
    for(;;)(void)TerminateProcess(GetCurrentProcess(),exit_code);
}
[[noreturn]] __declspec(noinline) void LBP3_GUEST_ABI debug_exception(std::uint32_t code,const void*) noexcept {
    terminate_diagnostic("sceKernelDebugRaiseException",code,0xe04c4202u);
}
[[noreturn]] __declspec(noinline) void LBP3_GUEST_ABI debug_exception_release(std::uint32_t code,const void*) noexcept {
    terminate_diagnostic("sceKernelDebugRaiseExceptionOnReleaseMode",code,0xe04c4203u);
}
[[noreturn]] __declspec(noinline) void LBP3_GUEST_ABI guest_exit(std::int32_t code) noexcept {
    terminate_diagnostic("_exit",static_cast<std::uint32_t>(code),static_cast<std::uint32_t>(code));
}
__declspec(noinline) std::int32_t* LBP3_GUEST_ABI error_location() noexcept {return &guest_errno;}
__declspec(noinline) std::int32_t LBP3_GUEST_ABI guest_page_size() noexcept {return 0x4000;}
__declspec(noinline) int LBP3_GUEST_ABI signal_fill(abi::SignalMask* output) noexcept {
    const auto error=abi::fill_signal_mask(output,true);if(error){guest_errno=error;return -1;}return 0;
}
__declspec(noinline) int LBP3_GUEST_ABI signal_change(int how,const abi::SignalMask* input,abi::SignalMask* previous) noexcept {
    const auto error=abi::change_signal_mask(how,input,previous);if(error){guest_errno=error;return -1;}return 0;
}
struct SignalTest {
    abi::SignalMask expected{};
    decltype(&signal_change) change{};
    bool valid{};
};
void* LBP3_GUEST_ABI signal_worker(void* argument) noexcept {
    auto& test=*static_cast<SignalTest*>(argument);abi::SignalMask observed{};
    const abi::SignalMask empty{};
    test.valid=!test.change(0,nullptr,&observed)&&observed==test.expected &&
        !test.change(3,&empty,nullptr)&&abi::current_signal_mask()==empty;
    return argument;
}
void* const map_failed=reinterpret_cast<void*>(UINTPTR_MAX);
bool round_guest_pages(std::uint64_t length,std::uint64_t& rounded) noexcept {
    if(!length || length>UINT64_MAX-0x3fff)return false;
    rounded=(length+0x3fff)&~std::uint64_t{0x3fff};return true;
}
std::atomic<std::int32_t> next_prelinked_module_handle{0x100};
__declspec(noinline) std::int32_t LBP3_GUEST_ABI kernel_load_start_module(
    const char* guest_path, std::uint64_t, const void*, std::uint32_t, const void*, std::int32_t* guest_result) noexcept {
    char path[1024]{};
    const auto path_address=reinterpret_cast<std::uintptr_t>(guest_path);
    if (!path_address || path_address>UINTPTR_MAX-(sizeof(path)-1)) return std::bit_cast<std::int32_t>(0x8002000eu);
    bool terminated=false;
    for(std::size_t i=0;i<sizeof(path)-1;++i){SIZE_T copied{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(path_address+i),&path[i],1,&copied)||copied!=1)
            return std::bit_cast<std::int32_t>(0x8002000eu);
        if(!path[i]){terminated=true;break;}
    }
    if(!terminated)return std::bit_cast<std::int32_t>(0x80020016u);
    if (guest_result) {
        const auto at=reinterpret_cast<std::uintptr_t>(guest_result);
        MEMORY_BASIC_INFORMATION info{};
        if (!at || !VirtualQuery(guest_result,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) return std::bit_cast<std::int32_t>(0x8002000eu);
        const auto protection=info.Protect&0xff;
        if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
           protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)
            return std::bit_cast<std::int32_t>(0x8002000eu);
    }
    if (std::strncmp(path,"/app0/",6)!=0) return std::bit_cast<std::int32_t>(0x80020002u);
    if (guest_result) {
        const std::int32_t success=0;SIZE_T written{};
        if(!WriteProcessMemory(GetCurrentProcess(),guest_result,&success,sizeof(success),&written)||written!=sizeof(success))
            return std::bit_cast<std::int32_t>(0x8002000eu);
    }
    // The eboot's app0 dependencies are already mapped and linked in the
    // diagnostic module set. Return positive pseudo handles for its load loop.
    auto handle=next_prelinked_module_handle.fetch_add(1,std::memory_order_relaxed);
    if(handle<=0||handle==INT32_MAX){next_prelinked_module_handle.store(0x101,std::memory_order_relaxed);handle=0x100;}
    return handle;
}
__declspec(noinline) void* LBP3_GUEST_ABI posix_map(void* hint,std::uint64_t length,int protection,
                                                int flags,int descriptor,std::int64_t offset) noexcept {
    std::uint64_t rounded{};
    if(!round_guest_pages(length,rounded)){guest_errno=22;return map_failed;}
    // Anonymous/private calls observed at libc 0x581af; file/fixed/GPU paths stay unsupported.
    if(hint || flags!=0x1002 || descriptor!=-1 || offset){guest_errno=45;return map_failed;}
    void* address{};const auto result=memory::map_named(&address,rounded,protection,0,"posix-anon");
    if(result){guest_errno=static_cast<std::uint32_t>(result)&0xffff;return map_failed;}
    return address;
}
__declspec(noinline) std::int32_t LBP3_GUEST_ABI posix_unmap(void* address,std::uint64_t length) noexcept {
    std::uint64_t rounded{};
    if(!round_guest_pages(length,rounded)){guest_errno=22;return -1;}
    const auto result=memory::unmap(address,rounded);
    if(result){guest_errno=static_cast<std::uint32_t>(result)&0xffff;return -1;}
    return 0;
}
[[noreturn]] __declspec(noinline) void LBP3_GUEST_ABI stack_failure() noexcept {
    // A corrupted guest stack must never unwind through unregistered ELF frames.
    constexpr char message[]="Guest stack canary failure (__stack_chk_fail); terminating diagnostic process.\n";
    DWORD written{};
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,sizeof(message)-1,&written,nullptr);
    (void)TerminateProcess(GetCurrentProcess(),0xe04c4201u);
    // TerminateProcess on our own process normally cannot return successfully.
    for (;;) (void)TerminateProcess(GetCurrentProcess(),0xe04c4201u);
}
constexpr std::int32_t guest_eio = std::bit_cast<std::int32_t>(0x80020005u);
__declspec(noinline) std::uint64_t LBP3_GUEST_ABI process_time() noexcept {
    LARGE_INTEGER current{};
    if (!QueryPerformanceCounter(&current) || current.QuadPart < epoch.QuadPart) return 0;
    const auto ticks = static_cast<std::uint64_t>(current.QuadPart - epoch.QuadPart);
    const auto freq = static_cast<std::uint64_t>(frequency.QuadPart);
    return (ticks/freq)*1000000 + (ticks%freq)*1000000/freq;
}
__declspec(noinline) std::int32_t LBP3_GUEST_ABI kernel_usleep(std::uint32_t microseconds) noexcept {
    if (!microseconds) { Sleep(0); return 0; }
    HANDLE timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!timer) return guest_eio;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(microseconds)*10;
    const bool armed = SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    const DWORD waited = armed ? WaitForSingleObject(timer, INFINITE) : WAIT_FAILED;
    CloseHandle(timer);
    return waited == WAIT_OBJECT_0 ? 0 : guest_eio;
}
__declspec(noinline) std::int32_t LBP3_GUEST_ABI yield() noexcept {
    (void)SwitchToThread();
    return 0; // No runnable peer is a valid yield, not a guest error.
}
}

void initialize() {
    if (ready) return;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        static_cast<std::uint64_t>(frequency.QuadPart) > std::numeric_limits<std::uint64_t>::max()/1000000 ||
        !QueryPerformanceCounter(&epoch))
        throw std::runtime_error("Cannot initialize the monotonic guest clock.");
    if (BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&stack_guard),sizeof(stack_guard),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 || !stack_guard)
        throw std::runtime_error("Cannot initialize the guest stack guard.");
    ready = true;
}

std::optional<std::uintptr_t> resolve(const HostImport& request) {
    if(const auto service=resolve_system_service(request))return service;
    if(const auto service=resolve_np_manager(request))return service;
    if(const auto service=resolve_np_webapi(request))return service;
    if(const auto service=resolve_np_signaling(request))return service;
    if(ready)if(const auto service=resolve_audio_out(request))return service;
    if(const auto service=resolve_np_trophy(request))return service;
    if(const auto service=resolve_companion_httpd(request))return service;
    if(const auto service=resolve_event_flags(request))return service;
    if(const auto service=resolve_pad_services(request))return service;
    if(const auto service=resolve_live_streaming(request))return service;
    if(const auto service=resolve_ime_dialog(request))return service;
    if(const auto service=resolve_network_control(request))return service;
    if(ready)if(const auto clock=resolve_clock(request))return clock;
    if(ready)if(const auto direct=memory::resolve_direct_memory(request))return direct;
    if(ready)if(const auto video=gpu::resolve_video_out(request))return video;
    if(ready)if(const auto query=resolve_memory_query(request))return query;
    if(ready)if(const auto queue=gpu::resolve_compute_queue(request))return queue;
    if(ready)if(const auto native_gpu=gpu::resolve_iss_backend_import(request))return native_gpu;
    if(ready)if(const auto commands=gpu::resolve_gnm_commands(request))return commands;
    if (!ready || request.library != "libkernel" || request.module != "libkernel" ||
        request.library_version != 1 || request.module_version != 0x0101) return std::nullopt;
    if (request.type==1 && request.nid=="f7uOxY9mM1U") return reinterpret_cast<std::uintptr_t>(&stack_guard);
    if (request.type==1 && request.nid=="djxxOmW6-aw") return reinterpret_cast<std::uintptr_t>(&progname);
    if (request.type!=2) return std::nullopt;
    if(request.nid=="wzvqT4UqKX8")return reinterpret_cast<std::uintptr_t>(&kernel_load_start_module);
    if (request.nid == "aI+OeCz8xrQ") return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_self);
    if (request.nid == "3PtV6p3QNX4") return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_equal);
    if (request.nid == "T72hz6ffq08") return reinterpret_cast<std::uintptr_t>(&yield);
    if(request.nid=="bt3CTBKmGyI")return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_set_affinity);
    if(request.nid=="rcrVFJsQWRY")return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_get_affinity);
    if(request.nid=="W0Hpm2X0uPE")return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_set_priority);
    if(request.nid=="1tKyG7RlMJo")return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_get_priority);
    if (request.nid == "f7KBOafysXo") return reinterpret_cast<std::uintptr_t>(&abi::module_from_address);
    if (request.nid == "Fjc4-n1+y2g") return reinterpret_cast<std::uintptr_t>(&abi::module_matches_address);
    if (request.nid == "crb5j7mkk1c") return reinterpret_cast<std::uintptr_t>(&abi::is_signal_return);
    if (request.nid == "VkTAsrZDcJ0") return reinterpret_cast<std::uintptr_t>(&signal_fill);
    if (request.nid == "aPcyptbOiZs") return reinterpret_cast<std::uintptr_t>(&signal_change);
    if (request.nid == "OMDRKKAZ8I4") return reinterpret_cast<std::uintptr_t>(&debug_exception);
    if (request.nid == "zE-wXIZjLoM") return reinterpret_cast<std::uintptr_t>(&debug_exception_release);
    if (request.nid == "6Z83sYWFlA8") return reinterpret_cast<std::uintptr_t>(&guest_exit);
    if (request.nid == "9BcDykPmo1I") return reinterpret_cast<std::uintptr_t>(&error_location);
    if (request.nid == "k+AXqu2-eBc") return reinterpret_cast<std::uintptr_t>(&guest_page_size);
    if (request.nid == "BPE9s9vQQXo") return reinterpret_cast<std::uintptr_t>(&posix_map);
    if (request.nid == "UqDGjXA5yUM") return reinterpret_cast<std::uintptr_t>(&posix_unmap);
    if (request.nid == "Ou3iL1abvng") return reinterpret_cast<std::uintptr_t>(&stack_failure);
    if (request.nid == "1jfXLRVzisc") return reinterpret_cast<std::uintptr_t>(&kernel_usleep);
    if (request.nid == "4J2sUJmuHZQ") return reinterpret_cast<std::uintptr_t>(&process_time);
    if (request.nid == "6XG4B33N09g") return reinterpret_cast<std::uintptr_t>(&yield);
    if (request.nid == "vNe1w4diLCs") return reinterpret_cast<std::uintptr_t>(&abi::tls_get_addr);
    if (request.nid == "6UgtwV+0zb4") return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_create);
    if (request.nid == "onNY9Byn-W8") return reinterpret_cast<std::uintptr_t>(&abi::sce_pthread_join);
    if (request.nid == "959qrazPIrg") return reinterpret_cast<std::uintptr_t>(&abi::get_proc_param);
    if (request.nid == "p5EcQeEeJAE") return reinterpret_cast<std::uintptr_t>(&abi::set_application_heap_api);
    if (request.nid == "mL8NDH86iQI") return reinterpret_cast<std::uintptr_t>(&memory::map_named);
    if (request.nid == "cQke9UuBQOk") return reinterpret_cast<std::uintptr_t>(&memory::unmap);
    if (request.nid == "3k6kx-zOOSQ") return reinterpret_cast<std::uintptr_t>(&memory::lock_pages);
    if(const auto service=resolve_filesystem(request))return service;
    if(const auto service=abi::resolve_thread_services(request))return service;
    if(const auto service=abi::resolve_condition(request))return service;
    if(const auto service=abi::resolve_semaphore(request))return service;
    if(const auto service=abi::resolve_event_queue(request))return service;
    if(const auto service=abi::resolve_pthread_attributes(request))return service;
    return abi::resolve_mutex(request);
}

std::int32_t* guest_errno_address() noexcept {return &guest_errno;}

void filesystem_self_test(const std::filesystem::path& root,std::ostream& output){
    initialize();
    for(const auto nid:{"1G3lF1Gg1k8","UK2Tl2DWUns","Cg4srZ6TKbU","4wSze92BhLI","+r3rMFwItV4","nKWi-N2HBV4",
                       "oib76F-12fk","kBwCPsYX-m4","eV9wAD2riIA","fTx66l5iWIA","VW3TVZiM4-E","WlyEA-sLDf0",
                       "1-LFLmRFxxM","naInUjYt3so","AUXVxWeJU-A","52NcYU9+lEo","mqQMh1zPPT8"}){
        HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        if(!resolve(request))throw std::runtime_error("Filesystem export missing.");
        auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("Filesystem function resolved as data.");
    }
    io::self_test(root,output);
}

void thread_services_self_test(std::ostream& output) {
    initialize();
    for(const auto nid:{"14bOACANTBo","geDaqgH9lTg","PrdHuuDekhY","+BzXYkqYeLE","eoht7mQOCmo"}){
        HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        if(!resolve(request))throw std::runtime_error("Thread services NID unresolved.");
        auto wrong=request;wrong.type=1;
        if(resolve(wrong))throw std::runtime_error("Thread services function resolved as data.");
        wrong=request;wrong.module_version=0;
        if(resolve(wrong))throw std::runtime_error("Thread services wrong version accepted.");
    }
    abi::thread_services_self_test(output);
}

void condition_self_test(std::ostream& output) {
    initialize();
    for(const auto nid:{"2Tb92quprl0","g+PZd2hiacg","WKAXJ4XBPQ4","kDh-NfxgMtE","JGgj7Uvrl+A"}){
        HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        if(!resolve(request))throw std::runtime_error("Condition export unresolved.");
        auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("Condition resolved as data.");
    }
    if(!resolve({"BmMjYxmew1w","libkernel","libkernel",1,0x0101,2}))throw std::runtime_error("Relative timed condition missing.");
    abi::condition_self_test(output);
}

void fatal_service_fixture(std::wstring_view mode) {
    initialize();
    const char* nid=mode==L"debug"?"OMDRKKAZ8I4":mode==L"release"?"zE-wXIZjLoM":mode==L"exit"?"6Z83sYWFlA8":nullptr;
    if(!nid)throw std::runtime_error("Unknown fatal fixture.");
    HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
    const auto address=resolve(request);if(!address)throw std::runtime_error("Fatal service missing.");
    auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("Fatal service resolved as data.");
    wrong=request;wrong.module_version=0;if(resolve(wrong))throw std::runtime_error("Fatal service accepted wrong version.");
    if(mode==L"exit"){
        using Exit=void (LBP3_GUEST_ABI *)(std::int32_t) noexcept;
        Exit volatile call=reinterpret_cast<Exit>(*address);call(73);
    }else{
        using Raise=void (LBP3_GUEST_ABI *)(std::uint32_t,const void*) noexcept;
        Raise volatile call=reinterpret_cast<Raise>(*address);call(0xa002000bu,nullptr);
    }
    throw std::runtime_error("Fatal service unexpectedly returned.");
}

void signal_mask_self_test(std::ostream& output) {
    initialize();
    const auto get=[](const char* nid){HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        const auto found=resolve(request);if(!found)throw std::runtime_error("Signal mask export missing.");
        auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("Signal mask function resolved as data.");
        wrong=request;wrong.module_version=0;if(resolve(wrong))throw std::runtime_error("Signal mask wrong version accepted.");
        return *found;};
    auto volatile fill=reinterpret_cast<decltype(&signal_fill)>(get("VkTAsrZDcJ0"));
    auto volatile change=reinterpret_cast<decltype(&signal_change)>(get("aPcyptbOiZs"));
    struct Restore {abi::SignalMask saved=abi::current_signal_mask();~Restore(){abi::inherit_signal_mask(saved);}} restore;
    abi::SignalMask empty{},full{},old{},expected{};
    guest_errno=81;
    if(fill(&full)||guest_errno!=81)throw std::runtime_error("Signal fill errno contract failed.");
    for(auto word:full)if(word!=UINT32_MAX)throw std::runtime_error("Signal set layout mismatch.");
    if(change(3,&empty,nullptr)||change(1,&full,&old)||old!=empty)throw std::runtime_error("Signal block failed.");
    expected=full;expected[0]&=~((1u<<8)|(1u<<16));
    if(abi::current_signal_mask()!=expected)throw std::runtime_error("Unmaskable signals were blocked.");
    abi::SignalMask subset{};subset[0]=1u<<1;
    if(change(2,&subset,&old)||old!=expected)throw std::runtime_error("Signal unblock failed.");
    expected[0]&=~(1u<<1);
    abi::SignalMask alias{};alias[3]=0x1234;
    const auto replacement=alias;
    if(change(3,&alias,&alias)||alias!=expected||abi::current_signal_mask()!=replacement)
        throw std::runtime_error("Aliased signal mask input/output failed.");
    old=full;
    if(change(99,&empty,&old)!=-1||guest_errno!=22||old!=full||
       change(3,reinterpret_cast<const abi::SignalMask*>(8),nullptr)!=-1||guest_errno!=14||
       fill(nullptr)!=-1||guest_errno!=14||abi::current_signal_mask()!=replacement)
        throw std::runtime_error("Signal mask invalid pointers/options altered state.");
    auto* readonly=static_cast<abi::SignalMask*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
    if(!readonly)throw std::runtime_error("Signal mask readonly fixture failed.");
    const auto denied=change(3,&empty,readonly);VirtualFree(readonly,0,MEM_RELEASE);
    if(denied!=-1||guest_errno!=14||abi::current_signal_mask()!=replacement)
        throw std::runtime_error("Signal mask output failure committed state.");
    SignalTest test{replacement,&signal_change};
    abi::GuestThread worker({},&signal_worker,&test);
    if(worker.join()!=&test||!test.valid||abi::current_signal_mask()!=replacement)
        throw std::runtime_error("Guest thread signal mask inheritance/isolation failed.");
    guest_errno=0;
    output << "Signal mask self-test passed: 128-bit layout, block/unblock/set/query, unmaskable signals and aliased buffers.\n"
              "Invalid pointers preserve mask; guest thread inherits an independent mask. Signal delivery is not implemented.\n";
}

void module_registry_self_test(std::ostream& output){
    initialize();
    for(const auto nid:{"f7KBOafysXo","Fjc4-n1+y2g","crb5j7mkk1c"}){
        HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        if(!resolve(request))throw std::runtime_error("Module query export missing.");
        auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("Module query resolved as data.");
    }
    abi::module_registry_self_test(output);
}

void posix_memory_self_test(std::ostream& output) {
    initialize();
    using Map=decltype(&posix_map);using Unmap=decltype(&posix_unmap);using Error=decltype(&error_location);
    const auto get=[](const char* nid){HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        const auto found=resolve(request);if(!found)throw std::runtime_error("Missing POSIX memory export.");
        auto wrong=request;wrong.type=1;if(resolve(wrong))throw std::runtime_error("POSIX memory resolved as data.");
        return *found;};
    Map volatile map=reinterpret_cast<Map>(get("BPE9s9vQQXo"));
    Unmap volatile unmap=reinterpret_cast<Unmap>(get("UqDGjXA5yUM"));
    Error volatile location=reinterpret_cast<Error>(get("9BcDykPmo1I"));
    memory::ScopedFlexibleMemory context(0x8000);
    *location()=71;
    auto* region=static_cast<std::byte*>(map(nullptr,17,3,0x1002,-1,0));
    if(region==map_failed || reinterpret_cast<std::uintptr_t>(region)%0x4000 || *location()!=71)
        throw std::runtime_error("POSIX anonymous mmap or success errno contract failed.");
    for(unsigned i=0;i<0x4000;++i)if(region[i]!=std::byte{})throw std::runtime_error("Rounded POSIX pages not zero.");
    region[16]=std::byte{0x5a};
    if(unmap(region,0)!=-1 || *location()!=22 || unmap(region,0x8000)!=-1 || *location()!=45 || region[16]!=std::byte{0x5a})
        throw std::runtime_error("POSIX unmap error/errno/preservation failed.");
    *location()=72;
    if(unmap(region,17)||*location()!=72||unmap(region,17)!=-1||*location()!=22)
        throw std::runtime_error("POSIX rounded release contract failed.");
    if(map(nullptr,0,3,0x1002,-1,0)!=map_failed||*location()!=22||
       map(nullptr,UINT64_MAX,3,0x1002,-1,0)!=map_failed||*location()!=22||
       map(nullptr,16,3,0x1002,0,0)!=map_failed||*location()!=45||
       map(nullptr,16,3,0x1002,-1,1)!=map_failed||*location()!=45||
       map(nullptr,16,3,0x1012,-1,0)!=map_failed||*location()!=45||
       map(nullptr,16,7,0x1002,-1,0)!=map_failed||*location()!=45||
       map(nullptr,0xc000,3,0x1002,-1,0)!=map_failed||*location()!=12)
        throw std::runtime_error("POSIX unsupported/error translation failed.");
    *location()=73;bool isolated=false;
    std::thread worker([&]{isolated=*location()==0 && map(nullptr,0,3,0x1002,-1,0)==map_failed && *location()==22;});worker.join();
    if(!isolated || *location()!=73)throw std::runtime_error("POSIX memory errno leaked between threads.");
    *location()=0;
    output << "POSIX memory self-test passed: six-argument SysV anonymous mmap, rounded guest pages, full munmap and MAP_FAILED/errno.\n"
              "Unsupported file/fixed/execute paths, quotas and thread errno isolation verified. No libc code executed.\n";
}

void libc_environment_self_test(std::ostream& output) {
    initialize();
    const auto get=[](const char* nid){HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        const auto address=resolve(request);if(!address)throw std::runtime_error("Libc environment export missing.");
        for(unsigned i=0;i<5;++i){auto wrong=request;
            switch(i){case 0:wrong.type=1;break;case 1:wrong.library="other";break;
            case 2:wrong.module="other";break;case 3:wrong.library_version=0;break;default:wrong.module_version=0;}
            if(resolve(wrong))throw std::runtime_error("Libc environment identity mismatch accepted.");
        }return *address;};
    using Error=std::int32_t* (LBP3_GUEST_ABI *)() noexcept;
    using PageSize=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    Error volatile location=reinterpret_cast<Error>(get("9BcDykPmo1I"));
    PageSize volatile pagesize=reinterpret_cast<PageSize>(get("k+AXqu2-eBc"));
    auto* parent=location();
    if(!parent || *parent || pagesize()!=0x4000)throw std::runtime_error("Initial guest errno/page size invalid.");
    const auto saved_native=errno;errno=EDOM;*parent=37;
    if(location()!=parent || *location()!=37 || pagesize()!=0x4000 || errno!=EDOM)
        throw std::runtime_error("Guest errno aliases native errno or getter has side effects.");
    errno=saved_native;
    std::barrier rendezvous(3);
    std::array<std::uintptr_t,2> addresses{};std::array<bool,2> valid{};
    auto worker=[&](unsigned index){auto* value=location();addresses[index]=reinterpret_cast<std::uintptr_t>(value);
        valid[index]=value && !*value;
        if(value)*value=static_cast<std::int32_t>(101+index);
        rendezvous.arrive_and_wait();rendezvous.arrive_and_wait();
        valid[index]=valid[index] && location()==value && *value==static_cast<std::int32_t>(101+index) && pagesize()==0x4000;
    };
    std::thread a(worker,0),b(worker,1);
    rendezvous.arrive_and_wait();
    const bool isolated=addresses[0] && addresses[1] && addresses[0]!=addresses[1] &&
        addresses[0]!=reinterpret_cast<std::uintptr_t>(parent) && addresses[1]!=reinterpret_cast<std::uintptr_t>(parent);
    rendezvous.arrive_and_wait();a.join();b.join();
    if(!isolated || !valid[0] || !valid[1] || *parent!=37)throw std::runtime_error("Guest errno threads are not isolated.");
    bool fresh=false;std::thread next([&]{fresh=*location()==0;});next.join();
    if(!fresh)throw std::runtime_error("New thread inherited stale guest errno.");
    *parent=0;
    output << "Libc environment self-test passed: SysV __error, independent errno in three live threads, stable pointers and fresh-thread reset.\n"
              "Guest page size is 16 KiB; native CRT errno remains separate. No libc code executed.\n";
}

void memory_self_test(std::ostream& output) {
    initialize();
    const auto get=[](const char* nid){HostImport request{nid,"libkernel","libkernel",1,0x0101,2};
        const auto found=resolve(request);if(!found)throw std::runtime_error("Memory API NID unresolved.");
        for(unsigned variant=0;variant<5;++variant){auto wrong=request;
            switch(variant){case 0:wrong.type=1;break;case 1:wrong.library="other";break;
            case 2:wrong.module="other";break;case 3:wrong.library_version=0;break;default:wrong.module_version=0;}
            if(resolve(wrong))throw std::runtime_error("Memory resolver accepted wrong identity.");
        }return *found;};
    memory::self_test(output,get("mL8NDH86iQI"),get("cQke9UuBQOk"),get("3k6kx-zOOSQ"));
}

void heap_api_self_test(std::ostream& output) {
    initialize();
    HostImport request{"p5EcQeEeJAE","libkernel","libkernel",1,0x0101,2};
    const auto found=resolve(request);
    if(!found)throw std::runtime_error("Application heap API NID unresolved.");
    for(unsigned variant=0;variant<5;++variant){auto wrong=request;
        switch(variant){case 0:wrong.type=1;break;case 1:wrong.library="other";break;
        case 2:wrong.module="other";break;case 3:wrong.library_version=0;break;default:wrong.module_version=0;}
        if(resolve(wrong))throw std::runtime_error("Heap API resolver accepted wrong identity.");
    }
    abi::heap_api_self_test(output,*found);
}

void libc_services_self_test(std::ostream& output) {
    initialize();
    using Getter=const void* (LBP3_GUEST_ABI *)() noexcept;
    const auto resolved=resolve({"959qrazPIrg","libkernel","libkernel",1,0x0101,2});
    if(!resolved)throw std::runtime_error("Process parameter NID unresolved.");
    Getter volatile getter=reinterpret_cast<Getter>(*resolved);
    if(getter())throw std::runtime_error("Unexpected process parameter outside scope.");
    std::array<std::byte,64> bytes{};
    bytes[0]=std::byte{0x40};bytes[8]=std::byte{'O'};bytes[9]=std::byte{'R'};
    bytes[10]=std::byte{'B'};bytes[11]=std::byte{'I'};bytes[12]=std::byte{1};
    {
        abi::ScopedProcessParam scope(bytes);
        if(getter()!=bytes.data())throw std::runtime_error("Process parameter pointer differs.");
        const void* child{};
        std::thread reader([&]{child=getter();});reader.join();
        if(child!=bytes.data())throw std::runtime_error("Process parameter not visible to worker.");
        auto nested=bytes;
        {abi::ScopedProcessParam inner(nested);if(getter()!=nested.data())throw std::runtime_error("Nested process parameter failed.");}
        if(getter()!=bytes.data())throw std::runtime_error("Process parameter restoration failed.");
    }
    if(getter())throw std::runtime_error("Process parameter context leaked.");
    for(int variant=0;variant<4;++variant){auto invalid=bytes;
        if(variant==0)invalid[0]=std::byte{0x30};
        if(variant==1)invalid[8]=std::byte{};
        if(variant==2)invalid[12]=std::byte{2};
        bool rejected=false;
        try{abi::ScopedProcessParam bad(std::span<const std::byte>(invalid).first(variant==3 ? 8 : 64));}
        catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Invalid process parameter accepted.");
    }
    for(const auto nid : {"F8bUHwAG284","smWEktiyyG0","iMp8QpE+XO4","1FGvU0i9saQ","cmo1RIYva9o","2Of0f+3mhhE",
                         "9UK1vLZQft4","upoVrzMHFeE","IafI2PxcPnQ","tn3VlD0hG60"})
        if(!resolve({nid,"libkernel","libkernel",1,0x0101,2}))throw std::runtime_error("Mutex NID unresolved.");
    abi::mutex_self_test(output);
    output << "Libc services self-test passed: validated process parameters and mutex API family.\n";
}

void stack_failure_fixture() {
    initialize();
    using Failure=void (LBP3_GUEST_ABI *)() noexcept;
    const HostImport request{"Ou3iL1abvng","libkernel","libkernel",1,0x0101,2};
    auto wrong=request;wrong.type=1;
    if(resolve(wrong))throw std::runtime_error("Stack failure function resolved as data.");
    wrong=request;wrong.module_version=0;
    if(resolve(wrong))throw std::runtime_error("Stack failure function accepted wrong version.");
    const auto target=resolve(request);
    if(!target)throw std::runtime_error("Stack failure NID unresolved.");
    Failure volatile call=reinterpret_cast<Failure>(*target);
    call();
    throw std::runtime_error("Stack failure unexpectedly returned.");
}

void pthread_self_test(std::ostream& output) {
    initialize();
    const auto create=resolve({"6UgtwV+0zb4","libkernel","libkernel",1,0x0101,2});
    const auto join=resolve({"onNY9Byn-W8","libkernel","libkernel",1,0x0101,2});
    if (!create || !join) throw std::runtime_error("Pthread API resolver failed.");
    abi::pthread_self_test(output,reinterpret_cast<abi::PthreadCreate>(*create),reinterpret_cast<abi::PthreadJoin>(*join));
}

void self_test(std::ostream& output) {
    initialize();
    event_flags_self_test(output);
    pad_services_self_test(output);
    live_streaming_self_test(output);
    ime_dialog_self_test(output);
    using Time = std::uint64_t (LBP3_GUEST_ABI *)() noexcept;
    using Sleep = std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t) noexcept;
    using Yield = std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    const HostImport request{"4J2sUJmuHZQ", "libkernel", "libkernel", 1, 0x0101, 2};
    const auto get = [](const HostImport& item) {
        const auto found = resolve(item);
        if (!found) throw std::runtime_error("Expected host service was not resolved.");
        return *found;
    };
    auto guard_request=request;
    guard_request.nid="f7uOxY9mM1U"; guard_request.type=1;
    const auto guard=get(guard_request);
    const auto value=*reinterpret_cast<volatile const std::uintptr_t*>(guard);
    initialize();
    if (!value || get(guard_request)!=guard || *reinterpret_cast<volatile const std::uintptr_t*>(guard)!=value)
        throw std::runtime_error("Guest stack guard data export is unstable.");
    guard_request.type=2;
    if(resolve(guard_request)) throw std::runtime_error("Stack guard object resolved as a function.");
    auto volatile clock = reinterpret_cast<Time>(get(request));
    auto sleep_request = request; sleep_request.nid = "1jfXLRVzisc";
    auto volatile sleep = reinterpret_cast<Sleep>(get(sleep_request));
    auto yield_request = request; yield_request.nid = "6XG4B33N09g";
    auto volatile relinquish = reinterpret_cast<Yield>(get(yield_request));
    auto tls_request = request; tls_request.nid = "vNe1w4diLCs";
    using Getter = std::byte* (LBP3_GUEST_ABI *)(const abi::TlsIndex*) noexcept;
    auto volatile tls_getter = reinterpret_cast<Getter>(get(tls_request));
    const auto stack_attr=resolve({"UTXzJbWhhTE","libkernel","libkernel",1,0x0101,2});
    const auto stack_attr_alias=resolve({"Bvn74vj6oLo","libkernel","libkernel",1,0x0101,2});
    if(!stack_attr||!stack_attr_alias||*stack_attr!=*stack_attr_alias)
        throw std::runtime_error("scePthreadAttrSetstack alias did not share the stack-size implementation.");
    const std::vector<abi::TlsTemplate> templates{{1,16,16,{std::byte{0x35}}}};
    abi::ThreadTls storage(templates);
    {
        abi::ScopedThreadTls attach(storage);
        const abi::TlsIndex index{1,0};
        if (tls_getter(&index) != storage.resolve(1,0,1).data() || *tls_getter(&index) != std::byte{0x35})
            throw std::runtime_error("Host TLS resolver call failed.");
    }
    for (int variant = 0; variant < 6; ++variant) {
        auto wrong = request;
        switch (variant) {
        case 0: wrong.nid = "unknown"; break;
        case 1: wrong.library = "other"; break;
        case 2: wrong.module = "other"; break;
        case 3: wrong.library_version = 2; break;
        case 4: wrong.module_version = 2; break;
        case 5: wrong.type = 1; break;
        }
        if (resolve(wrong)) throw std::runtime_error("Host resolver accepted an unsupported identity.");
    }
    if (sleep(0) || relinquish()) throw std::runtime_error("Yield/zero-sleep failed.");
    LARGE_INTEGER start_lo{}, start_hi{}, end_lo{}, end_hi{};
    QueryPerformanceCounter(&start_lo);
    const auto start = clock();
    QueryPerformanceCounter(&start_hi);
    if (sleep(5000)) throw std::runtime_error("Guest sleep failed.");
    QueryPerformanceCounter(&end_lo);
    const auto end = clock();
    QueryPerformanceCounter(&end_hi);
    const auto minimum_us = static_cast<long double>(end_lo.QuadPart-start_hi.QuadPart)*1000000/frequency.QuadPart;
    const auto maximum_us = static_cast<long double>(end_hi.QuadPart-start_lo.QuadPart)*1000000/frequency.QuadPart;
    // Bracket the guest readings so scheduler preemption does not create false failures.
    if (end < start || end-start < 4000 || static_cast<long double>(end-start)+2 < minimum_us ||
        static_cast<long double>(end-start) > maximum_us+2)
        throw std::runtime_error("Guest sleep/clock elapsed time test failed.");
    auto last = end;
    for (int i = 0; i < 1000; ++i) {
        const auto now = clock();
        if (now < last) throw std::runtime_error("Guest monotonic clock moved backwards.");
        last = now;
    }
    auto load_request=request;load_request.nid="wzvqT4UqKX8";
    using LoadStart=std::int32_t (LBP3_GUEST_ABI *)(const char*,std::uint64_t,const void*,std::uint32_t,const void*,std::int32_t*) noexcept;
    const auto load=reinterpret_cast<LoadStart>(get(load_request));
    const char prelinked_module[]="/app0/sce_module/libSceCompanionUtil.sprx";
    std::int32_t module_result=-1;
    const auto module_handle=load(prelinked_module,0,nullptr,0,nullptr,&module_result);
    if(module_handle<0x100||module_result!=0)throw std::runtime_error("Prelinked app0 module load contract failed.");
    const char unavailable_module[]="/system/module.sprx";
    if(load(unavailable_module,0,nullptr,0,nullptr,&module_result)!=std::bit_cast<std::int32_t>(0x80020002u)||
       load(reinterpret_cast<const char*>(1),0,nullptr,0,nullptr,nullptr)!=std::bit_cast<std::int32_t>(0x8002000eu)||
       load(prelinked_module,0,nullptr,0,nullptr,reinterpret_cast<std::int32_t*>(1))!=std::bit_cast<std::int32_t>(0x8002000eu))
        throw std::runtime_error("Prelinked module loader accepted invalid paths or output pointers.");
    auto wrong_load=load_request;wrong_load.library_version=2;
    if(resolve(wrong_load))throw std::runtime_error("sceKernelLoadStartModule accepted a wrong library version.");
    output << "HLE self-test passed: sceKernelUsleep, sceKernelGetProcessTime, sched_yield, __tls_get_addr and prelinked module loading.\n"
              "Typed SysV calls, elapsed time, monotonic clock, app0 module handles, path/pointer errors and strict resolver identities verified.\n"
              "No game code was executed.\n";
}
}
