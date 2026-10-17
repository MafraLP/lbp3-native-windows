#include "lbp3_host/condition.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/mutex.hpp"
#include "lbp3_host/wait_trace.hpp"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>

namespace lbp3::abi {
namespace {
using Handle=std::uint64_t;
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
template<class T> bool read(const T* source,T& value){SIZE_T n{};
    return source && ReadProcessMemory(GetCurrentProcess(),source,&value,sizeof(T),&n) && n==sizeof(T);}
bool write(Handle* output,Handle value){
    auto at=reinterpret_cast<std::uintptr_t>(output);if(!at||at>UINTPTR_MAX-8)return false;
    const auto end=at+8;
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),output,&value,8,&n)&&n==8;
}
struct Waiter {bool notified{};};
struct Condition {std::condition_variable changed;std::list<std::shared_ptr<Waiter>> waiters;};
struct Slot {std::uint32_t generation{1};std::shared_ptr<Condition> object;};
std::array<Slot,128> slots;
std::mutex registry;
constexpr Handle tag=0xa300000000000000ull;
Slot* lookup(Handle h){
    if((h&0xff00000000000000ull)!=tag)return nullptr;
    const auto index=static_cast<std::uint32_t>(h),generation=static_cast<std::uint32_t>((h>>32)&0xffffff);
    if(!index||index>slots.size()||!generation)return nullptr;
    auto& slot=slots[index-1];return slot.object&&slot.generation==generation ? &slot : nullptr;
}
std::int32_t allocate(Handle* output){
    for(std::size_t i=0;i<slots.size();++i){auto& slot=slots[i];
        if(!slot.object&&slot.generation<=0xffffff){
            auto object=std::make_shared<Condition>();const auto h=tag|(static_cast<Handle>(slot.generation)<<32)|(i+1);
            if(!write(output,h))return error(14);slot.object=std::move(object);return 0;
        }}return error(35);
}
std::int32_t ensure(Handle* pointer,std::shared_ptr<Condition>& object){
    Handle h{};if(!read(pointer,h))return error(14);
    if(!h){const auto result=allocate(pointer);if(result)return result;if(!read(pointer,h))return error(14);}
    auto* slot=lookup(h);if(!slot)return error(22);object=slot->object;return 0;
}
std::int32_t LBP3_GUEST_ABI init(Handle* output,const void* attr,const char* name) noexcept {
    if(!output)return error(22);if(attr)return error(45);
    try{std::lock_guard lock(registry);Handle old{};if(!read(output,old))return error(14);
        if(lookup(old))return error(16);
        if(name){bool done=false;for(unsigned i=0;i<32;++i){char c{};if(!read(name+i,c))return error(14);if(!c){done=true;break;}}if(!done)return error(63);}
        return allocate(output);
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI destroy(Handle* pointer) noexcept {
    if(!pointer)return error(22);
    try{std::lock_guard lock(registry);Handle h{};if(!read(pointer,h))return error(14);
        if(!h)return write(pointer,2)?0:error(14);
        auto* slot=lookup(h);if(!slot)return error(22);
        if(!slot->object->waiters.empty())return error(16);
        if(!write(pointer,2))return error(14);slot->object.reset();++slot->generation;return 0;
    }catch(...){return error(5);}
}
std::int32_t wake(Handle* pointer,bool all){
    if(!pointer)return error(22);
    std::lock_guard lock(registry);std::shared_ptr<Condition> object;
    if(const auto result=ensure(pointer,object))return result;
    for(auto& waiter:object->waiters)if(!waiter->notified){waiter->notified=true;if(!all)break;}
    object->changed.notify_all();return 0;
}
std::int32_t LBP3_GUEST_ABI signal(Handle* pointer) noexcept {try{return wake(pointer,false);}catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}}
std::int32_t LBP3_GUEST_ABI broadcast(Handle* pointer) noexcept {try{return wake(pointer,true);}catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}}
std::int32_t do_wait(Handle* pointer,Handle* mutex,bool timed,std::uint64_t microseconds,std::uintptr_t caller=0) noexcept {
    if(!pointer||!mutex)return error(22);
    MutexWaitLease lease;
    try{std::unique_lock lock(registry);std::shared_ptr<Condition> object;
        if(const auto result=ensure(pointer,object))return result;
        auto waiter=std::make_shared<Waiter>();object->waiters.push_back(waiter);
        if(const auto result=mutex_release_for_wait(mutex,lease)){object->waiters.remove(waiter);return result;}
        const auto started=std::chrono::steady_clock::now();
        bool failed=false,expired=false;
        try{
            if(timed){const auto now=std::chrono::steady_clock::now();
                const auto available=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::time_point::max()-now).count();
                const auto deadline=microseconds>static_cast<std::uint64_t>(available)?std::chrono::steady_clock::time_point::max():now+std::chrono::microseconds(microseconds);
                expired=!object->changed.wait_until(lock,deadline,[&]{return waiter->notified;});
            }else object->changed.wait(lock,[&]{return waiter->notified;});
        }catch(...){failed=true;}
        lock.unlock();const auto resumed=mutex_resume_after_wait(lease);lock.lock();
        object->waiters.remove(waiter);
        const auto result=resumed ? resumed : failed ? error(5) : expired ? error(60) : 0;
        diagnostics::guest_wait_trace(timed?"condition_timedwait":"condition_wait",
            reinterpret_cast<std::uintptr_t>(pointer),timed?microseconds:UINT64_MAX,result,
            std::chrono::steady_clock::now()-started,caller);
        return result;
    }catch(const std::bad_alloc&){if(lease.object)(void)mutex_resume_after_wait(lease);return error(12);}
    catch(...){if(lease.object)(void)mutex_resume_after_wait(lease);return error(5);}
}
std::int32_t LBP3_GUEST_ABI wait(Handle* pointer,Handle* mutex) noexcept {return do_wait(pointer,mutex,false,0,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}
std::int32_t LBP3_GUEST_ABI timedwait(Handle* pointer,Handle* mutex,std::uint64_t microseconds) noexcept {return do_wait(pointer,mutex,true,microseconds,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}
}
std::optional<std::uintptr_t> resolve_condition(const HostImport& request){
    if(request.type!=2)return std::nullopt;
    if(request.nid=="2Tb92quprl0")return reinterpret_cast<std::uintptr_t>(&init);
    if(request.nid=="g+PZd2hiacg")return reinterpret_cast<std::uintptr_t>(&destroy);
    if(request.nid=="WKAXJ4XBPQ4")return reinterpret_cast<std::uintptr_t>(&wait);
    if(request.nid=="BmMjYxmew1w")return reinterpret_cast<std::uintptr_t>(&timedwait);
    if(request.nid=="kDh-NfxgMtE")return reinterpret_cast<std::uintptr_t>(&signal);
    if(request.nid=="JGgj7Uvrl+A")return reinterpret_cast<std::uintptr_t>(&broadcast);
    return std::nullopt;
}
}
