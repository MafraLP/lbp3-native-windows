#include "lbp3_host/event_flags.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/wait_trace.hpp"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace lbp3::hle {
namespace {
constexpr std::int32_t error(unsigned code) noexcept { return std::bit_cast<std::int32_t>(0x80020000u|code); }
constexpr std::uint32_t wait_and=1, clear_satisfied=2, clear_all=4;
struct EventFlag {std::string name;std::uint32_t attr{};std::uint64_t pattern{};bool deleted{};std::size_t waiters{};std::mutex mutex;std::condition_variable changed;};
std::mutex registry_mutex;
std::map<std::int32_t,std::shared_ptr<EventFlag>> registry;
std::atomic<std::int32_t> next_handle{0x5000};

bool writable(const void* pointer,std::size_t size) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at||size>UINTPTR_MAX-at)return false;
    const auto end=at+size;
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<const void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto protection=info.Protect&0xff;
        if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }
    return true;
}
template<class T> bool read_guest(const T* source,T& value) noexcept {SIZE_T n{};return source&&ReadProcessMemory(GetCurrentProcess(),source,&value,sizeof(value),&n)&&n==sizeof(value);}
template<class T> bool write_guest(T* destination,const T& value) noexcept {SIZE_T n{};return writable(destination,sizeof(value))&&WriteProcessMemory(GetCurrentProcess(),destination,&value,sizeof(value),&n)&&n==sizeof(value);}
bool read_name(const char* source,std::string& result) {
    if(!source)return false;
    const auto base=reinterpret_cast<std::uintptr_t>(source);
    if(!base||base>UINTPTR_MAX-32)return false;
    for(std::size_t i=0;i<32;++i){char c{};SIZE_T n{};
        if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(base+i),&c,1,&n)||n!=1)return false;
        if(!c)return !result.empty();
        result.push_back(c);
    }
    return false;
}
std::shared_ptr<EventFlag> get(std::int32_t handle){std::lock_guard lock(registry_mutex);const auto it=registry.find(handle);return it==registry.end()?nullptr:it->second;}
std::int32_t LBP3_GUEST_ABI create(std::int32_t* output,const char* name,std::uint32_t attr,std::uint64_t pattern,const void* option) noexcept {
    try{
        if(!output||!writable(output,sizeof(*output)))return error(14);
        if(option||attr&~0x3fu)return error(22);
        auto flag=std::make_shared<EventFlag>();if(!read_name(name,flag->name))return error(14);
        flag->attr=attr;flag->pattern=pattern;
        std::lock_guard lock(registry_mutex);
        if(registry.size()>=1024)return error(24);
        auto id=next_handle.fetch_add(1,std::memory_order_relaxed);
        if(id<=0||id==INT32_MAX){next_handle.store(0x5001,std::memory_order_relaxed);id=0x5000;}
        registry.emplace(id,flag);
        if(!write_guest(output,id)){registry.erase(id);return error(14);}
        return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI set(std::int32_t handle,std::uint64_t bits) noexcept {
    auto flag=get(handle);if(!flag)return error(9);
    try{std::lock_guard lock(flag->mutex);if(flag->deleted)return error(9);flag->pattern|=bits;flag->changed.notify_all();return 0;}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI clear(std::int32_t handle,std::uint64_t bits) noexcept {
    auto flag=get(handle);if(!flag)return error(9);
    try{std::lock_guard lock(flag->mutex);if(flag->deleted)return error(9);flag->pattern&=~bits;return 0;}catch(...){return error(5);}
}
std::int32_t wait_impl(std::int32_t handle,std::uint64_t requested,std::uint32_t mode,std::uint64_t* observed,const std::uint32_t* guest_timeout,bool poll,std::uintptr_t caller=0) noexcept {
    if((mode&~7u)||((mode&clear_satisfied)&&(mode&clear_all))||!requested)return error(22);
    if(observed&&!writable(observed,sizeof(*observed)))return error(14);
    std::uint32_t timeout{};const bool has_timeout=guest_timeout!=nullptr;
    if(has_timeout&&!read_guest(guest_timeout,timeout))return error(14);
    auto flag=get(handle);if(!flag)return error(9);
    try{
        std::unique_lock lock(flag->mutex);if(flag->deleted)return error(9);
        const auto matches=[&]{return (mode&wait_and)?((flag->pattern&requested)==requested):((flag->pattern&requested)!=0);};
        if(!matches()&&!poll&&has_timeout&&timeout==0)return error(60);
        ++flag->waiters;
        const auto decrement=[&]{--flag->waiters;};
        bool ready=matches();
        const bool did_block=!ready&&!poll;
        const auto blocked_at=std::chrono::steady_clock::now();
        if(!ready&&!poll){
            if(has_timeout){
                const auto duration=std::chrono::microseconds(timeout);
                ready=flag->changed.wait_for(lock,duration,[&]{return flag->deleted||matches();});
            }else{
                flag->changed.wait(lock,[&]{return flag->deleted||matches();});ready=true;
            }
        }
        decrement();
        const auto wait_result=flag->deleted?error(43):(!ready||!matches()?error(60):0);
        if(did_block)diagnostics::guest_wait_trace("eventflag_wait",
            static_cast<std::uint32_t>(handle),has_timeout?timeout:UINT64_MAX,
            wait_result,std::chrono::steady_clock::now()-blocked_at,caller);
        if(wait_result)return wait_result;
        const auto current=flag->pattern;
        if(observed&&!write_guest(observed,current))return error(14);
        if(mode&clear_all)flag->pattern=0;
        else if(mode&clear_satisfied)flag->pattern&=~(current&requested);
        return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI wait(std::int32_t h,std::uint64_t bits,std::uint32_t mode,std::uint64_t* out,const std::uint32_t* timeout) noexcept {return wait_impl(h,bits,mode,out,timeout,false,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}
std::int32_t LBP3_GUEST_ABI poll(std::int32_t h,std::uint64_t bits,std::uint32_t mode,std::uint64_t* out) noexcept {return wait_impl(h,bits,mode,out,nullptr,true);}
std::int32_t LBP3_GUEST_ABI erase(std::int32_t handle) noexcept {
    std::shared_ptr<EventFlag> flag;
    {std::lock_guard lock(registry_mutex);const auto it=registry.find(handle);if(it==registry.end())return error(9);flag=it->second;registry.erase(it);}
    {std::lock_guard lock(flag->mutex);flag->deleted=true;flag->changed.notify_all();}
    return 0;
}
std::int32_t LBP3_GUEST_ABI cancel(std::int32_t handle,std::uint64_t pattern,std::int32_t* num_waiters) noexcept {
    if(num_waiters&&!writable(num_waiters,sizeof(*num_waiters)))return error(14);
    auto flag=get(handle);if(!flag)return error(9);
    try{std::lock_guard lock(flag->mutex);const auto count=static_cast<std::int32_t>(std::min<std::size_t>(flag->waiters,INT32_MAX));
        if(num_waiters&&!write_guest(num_waiters,count))return error(14);flag->pattern=pattern;flag->changed.notify_all();return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI unsupported_named_open(void*,const char*) noexcept {return error(2);}
std::int32_t LBP3_GUEST_ABI unsupported_named_close(void*) noexcept {return error(9);}
}

std::optional<std::uintptr_t> resolve_event_flags(const HostImport& request){
    if(request.type!=2||request.library!="libkernel"||request.module!="libkernel"||request.library_version!=1||request.module_version!=0x0101)return std::nullopt;
    if(request.nid=="BpFoboUJoZU")return reinterpret_cast<std::uintptr_t>(&create);
    if(request.nid=="IOnSvHzqu6A")return reinterpret_cast<std::uintptr_t>(&set);
    if(request.nid=="JTvBflhYazQ")return reinterpret_cast<std::uintptr_t>(&wait);
    if(request.nid=="7uhBFWRAS60")return reinterpret_cast<std::uintptr_t>(&clear);
    if(request.nid=="9lvj5DjHZiA")return reinterpret_cast<std::uintptr_t>(&poll);
    if(request.nid=="8mql9OcQnd4")return reinterpret_cast<std::uintptr_t>(&erase);
    if(request.nid=="PZku4ZrXJqg")return reinterpret_cast<std::uintptr_t>(&cancel);
    if(request.nid=="1vDaenmJtyA")return reinterpret_cast<std::uintptr_t>(&unsupported_named_open);
    if(request.nid=="s9-RaxukuzQ")return reinterpret_cast<std::uintptr_t>(&unsupported_named_close);
    return std::nullopt;
}

void event_flags_self_test(std::ostream& output){
    using Create=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*,const char*,std::uint32_t,std::uint64_t,const void*) noexcept;
    using Set=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::uint64_t) noexcept;
    using Wait=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::uint64_t,std::uint32_t,std::uint64_t*,const std::uint32_t*) noexcept;
    using Clear=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::uint64_t) noexcept;
    using Erase=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t) noexcept;
    auto get_fn=[](const char* nid){const auto found=resolve_event_flags({nid,"libkernel","libkernel",1,0x0101,2});if(!found)throw std::runtime_error("Event flag NID did not resolve.");return *found;};
    auto make=reinterpret_cast<Create>(get_fn("BpFoboUJoZU"));auto signal=reinterpret_cast<Set>(get_fn("IOnSvHzqu6A"));
    auto await_flag=reinterpret_cast<Wait>(get_fn("JTvBflhYazQ"));auto clear_flag=reinterpret_cast<Clear>(get_fn("7uhBFWRAS60"));auto remove=reinterpret_cast<Erase>(get_fn("8mql9OcQnd4"));
    std::int32_t id{};if(make(&id,"fixture-eventflag",0x20,0,nullptr)||id<=0)throw std::runtime_error("Event flag creation failed.");
    std::uint64_t observed{};const std::uint32_t short_timeout=2000;
    if(await_flag(id,1,0,&observed,&short_timeout)!=error(60))throw std::runtime_error("Event flag timeout was not reported.");
    std::atomic<bool> worker_ok{};
    std::thread worker([&]{std::uint64_t value{};const std::uint32_t wait_us=500000;worker_ok=await_flag(id,3,wait_and|clear_satisfied,&value,&wait_us)==0&&value==3;});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto set_first=signal(id,1);const auto set_second=signal(id,2);worker.join();
    if(set_first||set_second||!worker_ok||clear_flag(id,1)||signal(id,2)||
       await_flag(id,2,0,&observed,&short_timeout)||observed!=2||remove(id))
        throw std::runtime_error("Event flag wait, clear, handle or deletion behavior failed.");
    std::int32_t bad{};if(make(&bad,reinterpret_cast<const char*>(1),0,0,nullptr)!=error(14)||
       signal(id,1)!=error(9)||resolve_event_flags({"BpFoboUJoZU","libScePosix","libkernel",1,0x0101,2}))
        throw std::runtime_error("Event flag pointer, stale handle or resolver identity was accepted.");
    output<<"Event flag tests passed: output handles, bit-pattern waits, AND/OR, timeout, set, clear, deletion, stale IDs and resolver identity.\n";
}
}
