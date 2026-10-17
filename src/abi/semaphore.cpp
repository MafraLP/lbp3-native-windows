#include "lbp3_host/semaphore.hpp"
#include "lbp3_host/wait_trace.hpp"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <list>
#include <map>
#include <memory>
#include <mutex>

namespace lbp3::abi {
namespace {
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
template<class T> bool read(const T* p,T& v){SIZE_T n{};return p&&ReadProcessMemory(GetCurrentProcess(),p,&v,sizeof(v),&n)&&n==sizeof(v);}
template<class T> bool writable(T* p){
    auto at=reinterpret_cast<std::uintptr_t>(p);if(!at||at>UINTPTR_MAX-sizeof(T))return false;
    const auto end=at+sizeof(T);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto prot=info.Protect&255;
        if(prot!=PAGE_READWRITE&&prot!=PAGE_WRITECOPY&&prot!=PAGE_EXECUTE_READWRITE&&prot!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }return true;
}
template<class T> bool write(T* p,T v){SIZE_T n{};return writable(p)&&WriteProcessMemory(GetCurrentProcess(),p,&v,sizeof(v),&n)&&n==sizeof(v);}
struct Waiter {std::int32_t need{},result{};bool done{};};
struct Semaphore {std::int32_t count{},initial{},maximum{};std::list<Waiter*> waiters;std::condition_variable changed;};
std::mutex registry;
std::map<std::uint32_t,std::shared_ptr<Semaphore>> objects;
std::uint32_t next_id{1};
void dispatch(Semaphore& sem){
    // Tokens are reserved while holding the registry lock, before waking workers.
    for(auto i=sem.waiters.begin();i!=sem.waiters.end();){auto& waiter=**i;
        if(waiter.need>sem.count){++i;continue;}
        sem.count-=waiter.need;waiter.done=true;i=sem.waiters.erase(i);
    }sem.changed.notify_all();
}
std::int32_t wait_impl(std::uint32_t id,std::int32_t need,std::uint32_t* timeout,bool blocking,std::uintptr_t caller=0){
    std::unique_lock lock(registry);const auto found=objects.find(id);if(found==objects.end())return error(3);
    auto sem=found->second;if(need<=0||need>sem->maximum)return error(22);
    std::uint32_t usec{};if(timeout&&(!read(timeout,usec)||!writable(timeout)))return error(14);
    const auto start=std::chrono::steady_clock::now();
    const auto remaining=[&]{const auto spent=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        return spent>=usec?0u:usec-static_cast<std::uint32_t>(spent);};
    if(sem->count>=need){sem->count-=need;if(timeout&&!write(timeout,remaining()))return error(14);return 0;}
    if(!blocking)return error(16);
    if(timeout&&!usec)return error(60);
    Waiter waiter{need};sem->waiters.push_back(&waiter);
    const auto blocked_at=std::chrono::steady_clock::now();
    try {
        if(timeout){
            if(!sem->changed.wait_until(lock,start+std::chrono::microseconds(usec),[&]{return waiter.done;})){
                sem->waiters.remove(&waiter);waiter.result=error(60);
            }
        }else sem->changed.wait(lock,[&]{return waiter.done;});
    }catch(...){sem->waiters.remove(&waiter);return error(5);}
    if(timeout&&!write(timeout,remaining()))return error(14);
    diagnostics::guest_wait_trace("semaphore_wait",id,timeout?usec:UINT64_MAX,waiter.result,
        std::chrono::steady_clock::now()-blocked_at,caller);
    return waiter.result;
}
}
std::int32_t LBP3_GUEST_ABI sem_create(std::uint32_t* output,const char* name,std::uint32_t attr,std::int32_t initial,std::int32_t maximum,const void* options) noexcept {
    if(!output||!name||attr>2||initial<0||maximum<=0||initial>maximum)return error(22);
    // Priority ordering requires the guest scheduler; do not silently approximate it.
    if(attr!=1||options)return error(45);
    if(!writable(output))return error(14);
    bool terminated=false;for(unsigned i=0;i<32;++i){char c{};if(!read(name+i,c))return error(14);if(!c){terminated=true;break;}}
    if(!terminated)return error(63);
    try {std::lock_guard lock(registry);if(objects.size()>=256||!next_id)return error(28);
        auto sem=std::make_shared<Semaphore>();sem->count=sem->initial=initial;sem->maximum=maximum;
        const auto id=next_id++;objects.emplace(id,sem);
        if(!write(output,id)){objects.erase(id);return error(14);}
        diagnostics::guest_sync_event("semaphore_create",id,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(initial))<<32)|static_cast<std::uint32_t>(maximum),
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI sem_delete(std::uint32_t id) noexcept {
    try {std::lock_guard lock(registry);auto found=objects.find(id);if(found==objects.end())return error(3);
        auto sem=found->second;for(auto* waiter:sem->waiters){waiter->done=true;waiter->result=error(13);}
        sem->waiters.clear();objects.erase(found);sem->changed.notify_all();
        diagnostics::guest_sync_event("semaphore_delete",id,objects.size(),
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI sem_signal(std::uint32_t id,std::int32_t count) noexcept {
    try {std::lock_guard lock(registry);const auto found=objects.find(id);if(found==objects.end())return error(3);
        auto& sem=*found->second;if(count<=0||count>sem.maximum-sem.count)return error(22);
        sem.count+=count;dispatch(sem);
        // The startup investigation targets semaphore 0x13; tracing all the
        // worker queue signals floods the log before this gate can be studied.
        if(id==0x13)diagnostics::guest_sync_event("semaphore_signal",id,
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(count))<<32)|static_cast<std::uint32_t>(sem.count),
            reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI sem_poll(std::uint32_t id,std::int32_t need) noexcept {try{return wait_impl(id,need,nullptr,false);}catch(...){return error(12);}}
std::int32_t LBP3_GUEST_ABI sem_wait(std::uint32_t id,std::int32_t need,std::uint32_t* timeout) noexcept {try{return wait_impl(id,need,timeout,true,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}catch(...){return error(12);}}
std::int32_t LBP3_GUEST_ABI sem_cancel(std::uint32_t id,std::int32_t count,std::int32_t* number) noexcept {
    try {std::lock_guard lock(registry);const auto found=objects.find(id);if(found==objects.end())return error(3);
        auto& sem=*found->second;if(count < -1||count>sem.maximum)return error(22);
        if(number&&!write(number,static_cast<std::int32_t>(sem.waiters.size())))return error(14);
        for(auto* waiter:sem.waiters){waiter->done=true;waiter->result=error(85);}
        sem.waiters.clear();sem.count=count<0?sem.initial:count;sem.changed.notify_all();return 0;
    }catch(...){return error(5);}
}
std::optional<std::uintptr_t> resolve_semaphore(const HostImport& r){
    if(r.type!=2||r.library!="libkernel"||r.module!="libkernel"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="188x57JYp0g")return reinterpret_cast<std::uintptr_t>(&sem_create);
    if(r.nid=="R1Jvn8bSCW8")return reinterpret_cast<std::uintptr_t>(&sem_delete);
    if(r.nid=="4czppHBiriw")return reinterpret_cast<std::uintptr_t>(&sem_signal);
    if(r.nid=="12wOHk8ywb0")return reinterpret_cast<std::uintptr_t>(&sem_poll);
    if(r.nid=="Zxa0VhQVTsk")return reinterpret_cast<std::uintptr_t>(&sem_wait);
    if(r.nid=="4DM06U2BNEY")return reinterpret_cast<std::uintptr_t>(&sem_cancel);
    return std::nullopt;
}
std::optional<std::size_t> semaphore_waiter_count(std::uint32_t id){
    std::lock_guard lock(registry);const auto found=objects.find(id);
    if(found==objects.end())return std::nullopt;
    return found->second->waiters.size();
}
}
