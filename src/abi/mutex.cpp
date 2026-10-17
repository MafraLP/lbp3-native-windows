#include "lbp3_host/mutex.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/wait_trace.hpp"
#include <Windows.h>
#include <intrin.h>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <shared_mutex>
#include <unordered_map>

namespace lbp3::abi {
namespace {
using Handle=std::uint64_t;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
template<class T> bool read(const T* p,T& value) {
    SIZE_T n{};return p && ReadProcessMemory(GetCurrentProcess(),p,&value,sizeof(value),&n) && n==sizeof(value);
}
template<class T> bool write(T* p,const T& value) {
    auto at=reinterpret_cast<std::uintptr_t>(p);
    if(!at || at>UINTPTR_MAX-sizeof(T)) return false;
    const auto end=at+sizeof(T);
    while(at<end) {
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        const auto protection=info.Protect&255;
        if(protection!=PAGE_READWRITE && protection!=PAGE_WRITECOPY && protection!=PAGE_EXECUTE_READWRITE && protection!=PAGE_EXECUTE_WRITECOPY) return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base || base+info.RegionSize<=at) return false;
        at=base+info.RegionSize;
    }
    SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),p,&value,sizeof(value),&n) && n==sizeof(value);
}
struct Attr {int type{1};int protocol{};};
// Only the observed libc flags word is exposed. Native synchronization state
// lives separately and is never interpreted from guest-writable bytes.
struct GuestMutexPrefix {std::array<std::byte,0x20> reserved{};std::uint32_t flags{};};
static_assert(offsetof(GuestMutexPrefix,flags)==0x20);
struct GuestMutexArena {
    static constexpr std::size_t page=4096, capacity=4096;
    void* base{};std::size_t issued{};
    ~GuestMutexArena(){if(base)VirtualFree(base,0,MEM_RELEASE);}
    Handle create(){
        if(issued==capacity)return 0;
        if(!base)base=VirtualAlloc(nullptr,page*capacity,MEM_RESERVE,PAGE_NOACCESS);
        if(!base)throw std::bad_alloc();
        auto* address=static_cast<std::byte*>(base)+page*issued;
        if(!VirtualAlloc(address,page,MEM_COMMIT,PAGE_READWRITE))throw std::bad_alloc();
        ++issued; // Never reuse an issued address, including failed publications.
        return reinterpret_cast<Handle>(address);
    }
    void retire(Handle h){
        // Keep the reservation so a stale pointer can never become a new mutex.
        (void)VirtualFree(reinterpret_cast<void*>(h),page,MEM_DECOMMIT);
    }
} guest_mutex_arena;
struct Mutex {
    int type{1}; std::thread::id owner; unsigned depth{},waiters{};
    Handle guest{};
    std::condition_variable changed;
};
struct Rwlock {
    std::shared_mutex native;
    std::unordered_map<std::thread::id, std::pair<unsigned,bool>> owners;
    unsigned inflight{};
    Handle guest{};
};
template<class T> struct Slot {std::uint32_t generation{1};std::shared_ptr<T> object;};
std::array<Slot<Attr>,128> attrs;
std::array<Slot<Mutex>,256> mutexes;
std::array<Slot<Rwlock>,256> rwlocks;
std::mutex table;
constexpr Handle attr_tag=0xa100000000000000ull;
constexpr Handle rwlock_tag=0xa200000000000000ull;

Slot<Mutex>* lookup_mutex(Handle handle) {
    if(handle<=2)return nullptr;
    for(auto& slot:mutexes)if(slot.object && slot.object->guest==handle)return &slot;
    return nullptr; // No dereference of a caller-supplied pointer.
}
Slot<Rwlock>* lookup_rwlock(Handle handle) {
    if((handle&0xff00000000000000ull)!=rwlock_tag)return nullptr;
    const auto index=static_cast<std::uint32_t>(handle);
    const auto generation=static_cast<std::uint32_t>((handle>>32)&0xffffff);
    if(!index||index>rwlocks.size()||!generation)return nullptr;
    auto& slot=rwlocks[index-1];
    return slot.object&&slot.generation==generation?&slot:nullptr;
}
std::int32_t allocate_mutex(Handle* output,std::shared_ptr<Mutex> object) {
    for(auto& slot:mutexes)if(!slot.object){
        const auto handle=guest_mutex_arena.create();
        if(!handle)return err(35);
        if(!write(output,handle)){guest_mutex_arena.retire(handle);return err(14);}
        object->guest=handle;slot.object=std::move(object);return 0;
    }
    return err(35);
}
std::int32_t allocate_rwlock(Handle* output,std::shared_ptr<Rwlock> object) {
    for(std::size_t i=0;i<rwlocks.size();++i)if(!rwlocks[i].object&&rwlocks[i].generation&&rwlocks[i].generation<=0xffffff){
        const auto value=rwlock_tag|(static_cast<Handle>(rwlocks[i].generation)<<32)|(i+1);
        if(!write(output,value))return err(14);
        rwlocks[i].object=std::move(object);return 0;
    }
    return err(35);
}
std::int32_t LBP3_GUEST_ABI rwlock_init(Handle* output,Handle*,const char* name) noexcept {
    if(!output)return err(22);
    try{std::lock_guard guard(table);
        if(name){bool done=false;for(unsigned i=0;i<32;++i){char c{};if(!read(name+i,c))return err(14);if(!c){done=true;break;}}if(!done)return err(22);}
        return allocate_rwlock(output,std::make_shared<Rwlock>());
    }catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI rwlock_destroy(Handle* value) noexcept {
    if(!value)return err(22);
    try{std::lock_guard guard(table);Handle h{};if(!read(value,h))return err(14);
        if(!h)return 0;
        auto* slot=lookup_rwlock(h);if(!slot)return err(22);
        if(slot->object->inflight||!slot->object->owners.empty())return err(16);
        if(!write(value,Handle{1}))return err(14);
        slot->object.reset();if(++slot->generation>0xffffff)slot->generation=0;return 0;
    }catch(...){return err(5);}
}
enum class RwOperation { read,write,try_read,try_write };
std::int32_t rwlock_acquire(Handle* value,RwOperation op,std::uintptr_t caller=0) {
    if(!value)return err(22);
    std::shared_ptr<Rwlock> object;Handle guest_handle{};const auto self=std::this_thread::get_id();const bool writer=op==RwOperation::write||op==RwOperation::try_write;
    {
        std::lock_guard guard(table);Handle h{};if(!read(value,h))return err(14);
        if(!h){auto result=allocate_rwlock(value,std::make_shared<Rwlock>());if(result)return result;if(!read(value,h))return err(14);}
        auto* slot=lookup_rwlock(h);if(!slot)return err(22);object=slot->object;
        guest_handle=h;
        const auto owner=object->owners.find(self);
        if(owner!=object->owners.end()&&(owner->second.second||(writer&&owner->second.first)))return op==RwOperation::try_write?err(16):err(11);
        if(owner!=object->owners.end()&&owner->second.first==UINT_MAX)return err(35);
        ++object->inflight;
    }
    bool acquired{};const auto started=std::chrono::steady_clock::now();
    try{
        if(op==RwOperation::read) {object->native.lock_shared();acquired=true;}
        else if(op==RwOperation::write) {object->native.lock();acquired=true;}
        else acquired=writer?object->native.try_lock():object->native.try_lock_shared();
    }catch(...){std::lock_guard guard(table);--object->inflight;throw;}
    {
        std::lock_guard guard(table);--object->inflight;
        if(acquired){auto& owner=object->owners[self];if(writer)owner.second=true;else ++owner.first;}
    }
    const auto result=acquired?0:(op==RwOperation::try_read||op==RwOperation::try_write?err(16):err(5));
    if(op==RwOperation::read||op==RwOperation::write)
        diagnostics::guest_wait_trace(writer?"rwlock_write":"rwlock_read",guest_handle,UINT64_MAX,result,
            std::chrono::steady_clock::now()-started,caller);
    return result;
}
std::int32_t LBP3_GUEST_ABI rwlock_rdlock(Handle* value) noexcept {try{return rwlock_acquire(value,RwOperation::read,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI rwlock_wrlock(Handle* value) noexcept {try{return rwlock_acquire(value,RwOperation::write,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI rwlock_tryrdlock(Handle* value) noexcept {try{return rwlock_acquire(value,RwOperation::try_read);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI rwlock_trywrlock(Handle* value) noexcept {try{return rwlock_acquire(value,RwOperation::try_write);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI rwlock_unlock(Handle* value) noexcept {
    if(!value)return err(22);
    try{std::shared_ptr<Rwlock> object;bool writer{};const auto self=std::this_thread::get_id();
        {std::lock_guard guard(table);Handle h{};if(!read(value,h))return err(14);auto* slot=lookup_rwlock(h);if(!slot)return err(22);object=slot->object;
         auto owner=object->owners.find(self);if(owner==object->owners.end()||(!owner->second.first&&!owner->second.second))return err(1);writer=owner->second.second;
         if(writer)object->native.unlock();else object->native.unlock_shared();
         if(writer)object->owners.erase(owner);else if(!--owner->second.first)object->owners.erase(owner);}
        return 0;
    }catch(...){return err(5);}
}
template<class T,std::size_t N> Slot<T>* lookup(std::array<Slot<T>,N>& slots,Handle handle,Handle tag) {
    if((handle&0xff00000000000000ull)!=tag) return nullptr;
    const auto index=static_cast<std::uint32_t>(handle);
    const auto generation=static_cast<std::uint32_t>((handle>>32)&0xffffff);
    if(!index || index>N || !generation) return nullptr;
    auto& slot=slots[index-1];return slot.object && slot.generation==generation ? &slot : nullptr;
}
template<class T,std::size_t N> std::int32_t allocate(std::array<Slot<T>,N>& slots,Handle* output,Handle tag,std::shared_ptr<T> object) {
    for(std::size_t i=0;i<N;++i) if(!slots[i].object && slots[i].generation && slots[i].generation<=0xffffff) {
        const Handle handle=tag|(static_cast<Handle>(slots[i].generation)<<32)|(i+1);
        if(!write(output,handle)) return err(14);
        slots[i].object=std::move(object);return 0;
    }
    return err(35);
}
std::int32_t LBP3_GUEST_ABI attr_init(Handle* out) noexcept {
    if(!out) return err(22);
    try {std::lock_guard lock(table);Handle old{};if(!read(out,old))return err(14);
        if(lookup(attrs,old,attr_tag))return err(16);
        return allocate(attrs,out,attr_tag,std::make_shared<Attr>());
    }catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI attr_destroy(Handle* value) noexcept {
    if(!value)return err(22);
    try{std::lock_guard lock(table);Handle h{};if(!read(value,h))return err(14);
        auto* slot=lookup(attrs,h,attr_tag);if(!slot)return err(22);
        if(!write(value,Handle{}))return err(14);slot->object.reset();++slot->generation;return 0;
    }catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI attr_type(Handle* value,int type) noexcept {
    if(!value || type<1 || type>4)return err(22);
    // Type 3 is NORMAL: do not silently implement self-deadlock as ERRORCHECK.
    if(type==3)return err(45);
    try{std::lock_guard lock(table);Handle h{};if(!read(value,h))return err(14);
        auto* slot=lookup(attrs,h,attr_tag);if(!slot)return err(22);slot->object->type=type;return 0;
    }catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI attr_protocol(Handle* value,int protocol) noexcept {
    if(!value || protocol<0 || protocol>2)return err(22);
    try{std::lock_guard lock(table);Handle h{};if(!read(value,h))return err(14);
        auto* slot=lookup(attrs,h,attr_tag);if(!slot)return err(22);slot->object->protocol=protocol;return 0;
    }catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI init(Handle* out,Handle* attr,const char* name) noexcept {
    if(!out)return err(22);
    try{std::lock_guard lock(table);Handle old{};if(!read(out,old))return err(14);
        if(lookup_mutex(old))return err(16);
        auto object=std::make_shared<Mutex>();
        if(attr){Handle h{};if(!read(attr,h))return err(14);auto* slot=lookup(attrs,h,attr_tag);if(!slot)return err(22);object->type=slot->object->type;}
        if(name){bool done=false;for(unsigned i=0;i<32;++i){char c{};if(!read(name+i,c))return err(14);if(!c){done=true;break;}}if(!done)return err(22);}
        return allocate_mutex(out,std::move(object));
    }catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}
}
std::int32_t LBP3_GUEST_ABI destroy(Handle* value) noexcept {
    if(!value)return err(22);
    try{std::lock_guard lock(table);Handle h{};if(!read(value,h))return err(14);
        if(h<=1)return write(value,Handle{2}) ? 0 : err(14);
        auto* slot=lookup_mutex(h);if(!slot)return err(22);
        if(slot->object->depth || slot->object->waiters)return err(16);
        if(!write(value,Handle{2}))return err(14);
        guest_mutex_arena.retire(h);slot->object.reset();return 0;
    }catch(...){return err(5);}
}
std::int32_t acquire(Handle* value,int mode,std::uint32_t microseconds,std::uintptr_t caller=0) {
    if(!value)return err(22);
    std::unique_lock lock(table);Handle h{};if(!read(value,h))return err(14);
    // Lazily materialize known static initializer values under the same lock.
    if(h<=1){auto object=std::make_shared<Mutex>();auto result=allocate_mutex(value,std::move(object));if(result)return result;if(!read(value,h))return err(14);}
    auto* slot=lookup_mutex(h);if(!slot)return err(22);
    auto object=slot->object;const auto self=std::this_thread::get_id();
    if(object->depth && object->owner==self){
        if(object->type==2){if(object->depth==UINT_MAX)return err(35);++object->depth;return 0;}
        return mode==1 ? err(16) : err(11);
    }
    if(object->depth){
        if(mode==1)return err(16);
        ++object->waiters;
        const auto started=std::chrono::steady_clock::now();
        bool acquired=true;
        try {
            if(mode==2)acquired=object->changed.wait_for(lock,std::chrono::microseconds(microseconds),[&]{return !object->depth;});
            else object->changed.wait(lock,[&]{return !object->depth;});
        }catch(...){--object->waiters;throw;}
        --object->waiters;
        const auto result=acquired?0:err(60);
        diagnostics::guest_wait_trace(mode==2?"mutex_timedlock":"mutex_lock",h,
            mode==2?microseconds:UINT64_MAX,result,std::chrono::steady_clock::now()-started,caller);
        if(!acquired)return result;
    }
    object->owner=self;object->depth=1;return 0;
}
std::int32_t LBP3_GUEST_ABI lock(Handle* value) noexcept {try{return acquire(value,0,0,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}catch(const std::bad_alloc&){return err(12);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI trylock(Handle* value) noexcept {try{return acquire(value,1,0);}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI timedlock(Handle* value,std::uint32_t us) noexcept {try{return acquire(value,2,us,reinterpret_cast<std::uintptr_t>(_ReturnAddress()));}catch(...){return err(5);}}
std::int32_t LBP3_GUEST_ABI unlock(Handle* value) noexcept {
    if(!value)return err(22);
    try{std::lock_guard guard(table);Handle h{};if(!read(value,h))return err(14);
        auto* slot=lookup_mutex(h);if(!slot)return err(22);
        auto& object=*slot->object;
        if(!object.depth || object.owner!=std::this_thread::get_id())return err(1);
        if(!--object.depth){object.owner={};object.changed.notify_one();}return 0;
    }catch(...){return err(5);}
}
__declspec(noinline) void LBP3_GUEST_ABI mark_libc_mutex(Handle* value) noexcept {
    // Reproduce the two instructions at libc VAs 0x108c/0x108f (also 0x242e).
    asm volatile("movq (%0), %%rax\n\torl $0x100, 0x20(%%rax)" : : "r"(value) : "rax","memory","cc");
}
}

std::optional<std::uintptr_t> resolve_mutex(const HostImport& r) {
    if(r.type!=2)return std::nullopt;
    if(r.nid=="F8bUHwAG284")return reinterpret_cast<std::uintptr_t>(&attr_init);
    if(r.nid=="smWEktiyyG0")return reinterpret_cast<std::uintptr_t>(&attr_destroy);
    if(r.nid=="iMp8QpE+XO4")return reinterpret_cast<std::uintptr_t>(&attr_type);
    if(r.nid=="1FGvU0i9saQ")return reinterpret_cast<std::uintptr_t>(&attr_protocol);
    if(r.nid=="cmo1RIYva9o")return reinterpret_cast<std::uintptr_t>(&init);
    if(r.nid=="2Of0f+3mhhE")return reinterpret_cast<std::uintptr_t>(&destroy);
    if(r.nid=="9UK1vLZQft4")return reinterpret_cast<std::uintptr_t>(&lock);
    if(r.nid=="upoVrzMHFeE")return reinterpret_cast<std::uintptr_t>(&trylock);
    if(r.nid=="IafI2PxcPnQ")return reinterpret_cast<std::uintptr_t>(&timedlock);
    if(r.nid=="tn3VlD0hG60")return reinterpret_cast<std::uintptr_t>(&unlock);
    if(r.nid=="6ULAa0fq4jA")return reinterpret_cast<std::uintptr_t>(&rwlock_init);
    if(r.nid=="BB+kb08Tl9A")return reinterpret_cast<std::uintptr_t>(&rwlock_destroy);
    if(r.nid=="Ox9i0c7L5w0")return reinterpret_cast<std::uintptr_t>(&rwlock_rdlock);
    if(r.nid=="mqdNorrB+gI")return reinterpret_cast<std::uintptr_t>(&rwlock_wrlock);
    if(r.nid=="XD3mDeybCnk")return reinterpret_cast<std::uintptr_t>(&rwlock_tryrdlock);
    if(r.nid=="bIHoZCTomsI")return reinterpret_cast<std::uintptr_t>(&rwlock_trywrlock);
    if(r.nid=="+L98PIbGttk")return reinterpret_cast<std::uintptr_t>(&rwlock_unlock);
    return std::nullopt;
}

std::int32_t mutex_release_for_wait(std::uint64_t* value,MutexWaitLease& lease) noexcept {
    try{std::lock_guard guard(table);Handle h{};
        if(lease.object)return err(22);
        if(!read(value,h))return err(14);
        auto* slot=lookup_mutex(h);if(!slot)return err(22);
        auto object=slot->object;
        if(!object->depth || object->owner!=std::this_thread::get_id())return err(1);
        lease.object=object;lease.depth=object->depth;
        ++object->waiters;object->depth=0;object->owner={};object->changed.notify_one();return 0;
    }catch(...){return err(5);}
}
std::int32_t mutex_resume_after_wait(MutexWaitLease& lease) noexcept {
    try{std::unique_lock guard(table);
        if(!lease.object || !lease.depth)return err(22);
        auto object=std::static_pointer_cast<Mutex>(lease.object);
        object->changed.wait(guard,[&]{return !object->depth;});
        object->owner=std::this_thread::get_id();object->depth=lease.depth;--object->waiters;
        lease.object.reset();lease.depth=0;return 0;
    }catch(...){return err(5);}
}

void mutex_self_test(std::ostream& output) {
    using Unary=std::int32_t (LBP3_GUEST_ABI *)(Handle*) noexcept;
    using Setting=std::int32_t (LBP3_GUEST_ABI *)(Handle*,int) noexcept;
    using Init=std::int32_t (LBP3_GUEST_ABI *)(Handle*,Handle*,const char*) noexcept;
    using Timed=std::int32_t (LBP3_GUEST_ABI *)(Handle*,std::uint32_t) noexcept;
    const auto get=[](const char* nid){const auto p=resolve_mutex({nid,"libkernel","libkernel",1,0x0101,2});
        if(!p)throw std::runtime_error("Mutex resolver missing entry.");return *p;};
    auto volatile attr_init=reinterpret_cast<Unary>(get("F8bUHwAG284"));
    auto volatile attr_destroy=reinterpret_cast<Unary>(get("smWEktiyyG0"));
    auto volatile attr_type=reinterpret_cast<Setting>(get("iMp8QpE+XO4"));
    auto volatile attr_protocol=reinterpret_cast<Setting>(get("1FGvU0i9saQ"));
    auto volatile init=reinterpret_cast<Init>(get("cmo1RIYva9o"));
    auto volatile destroy=reinterpret_cast<Unary>(get("2Of0f+3mhhE"));
    auto volatile lock=reinterpret_cast<Unary>(get("9UK1vLZQft4"));
    auto volatile trylock=reinterpret_cast<Unary>(get("upoVrzMHFeE"));
    auto volatile unlock=reinterpret_cast<Unary>(get("tn3VlD0hG60"));
    auto volatile timedlock=reinterpret_cast<Timed>(get("IafI2PxcPnQ"));
    Handle layout{};
    if(init(&layout,nullptr,"libc-layout"))throw std::runtime_error("Guest mutex layout setup failed.");
    auto* flags=reinterpret_cast<std::uint32_t*>(layout+0x20);
    std::uint32_t bits{};
    if(!read(flags,bits)||bits)throw std::runtime_error("Guest mutex flags not zero initialized.");
    mark_libc_mutex(&layout);
    if(!read(flags,bits)||bits!=0x100 || lock(&layout)||unlock(&layout)||
       !read(flags,bits)||bits!=0x100)throw std::runtime_error("Libc direct flags access corrupted mutex state.");
    Handle interior=layout+8;
    GuestMutexPrefix forged{};Handle forged_handle=reinterpret_cast<Handle>(&forged);
    if(lock(&interior)!=err(22)||lock(&forged_handle)!=err(22))throw std::runtime_error("Unregistered guest mutex pointer accepted.");
    const auto retired=layout;
    if(destroy(&layout))throw std::runtime_error("Guest mutex layout destruction failed.");
    MEMORY_BASIC_INFORMATION retired_info{};
    if(!VirtualQuery(reinterpret_cast<void*>(retired),&retired_info,sizeof(retired_info)) ||
       retired_info.State!=MEM_RESERVE)throw std::runtime_error("Retired guest mutex page remains accessible.");
    for(unsigned n=0;n<64;++n){
        Handle fresh{};Handle stale_copy=retired;
        if(init(&fresh,nullptr,nullptr)||fresh==retired||lock(&stale_copy)!=err(22)||destroy(&fresh))
            throw std::runtime_error("Guest mutex address reused or stale handle accepted.");
    }
    using RwInit=std::int32_t (LBP3_GUEST_ABI *)(Handle*,Handle*,const char*) noexcept;
    auto rwget=[](const char* nid){const auto p=resolve_mutex({nid,"libkernel","libkernel",1,0x0101,2});
        if(!p)throw std::runtime_error("RW-lock resolver missing entry.");return *p;};
    auto volatile rwinit=reinterpret_cast<RwInit>(rwget("6ULAa0fq4jA"));
    auto volatile rwdestroy=reinterpret_cast<Unary>(rwget("BB+kb08Tl9A"));
    auto volatile rdlock=reinterpret_cast<Unary>(rwget("Ox9i0c7L5w0"));
    auto volatile wrlock=reinterpret_cast<Unary>(rwget("mqdNorrB+gI"));
    auto volatile tryrdlock=reinterpret_cast<Unary>(rwget("XD3mDeybCnk"));
    auto volatile trywrlock=reinterpret_cast<Unary>(rwget("bIHoZCTomsI"));
    auto volatile rwunlock=reinterpret_cast<Unary>(rwget("+L98PIbGttk"));
    Handle rw{};
    if(rwinit(&rw,nullptr,"rwlock-test")||!rw||rdlock(&rw)||tryrdlock(&rw)||trywrlock(&rw)!=err(16)||
       rwdestroy(&rw)!=err(16)||rwunlock(&rw)||rwunlock(&rw)||wrlock(&rw)||trywrlock(&rw)!=err(16)||
       rwdestroy(&rw)!=err(16)||rwunlock(&rw)||rwunlock(&rw)!=err(1)||rwdestroy(&rw))
        throw std::runtime_error("Guest RW-lock ownership, exclusion or destruction checks failed.");
    Handle bad=reinterpret_cast<Handle>(&rw);
    if(rdlock(&bad)!=err(22)||rwinit(nullptr,nullptr,nullptr)!=err(22))
        throw std::runtime_error("Guest RW-lock accepted an invalid handle or output pointer.");
    Handle attr{},value{};
    if(attr_init(&attr)||attr_type(&attr,2)||init(&value,&attr,"recursive")||attr_destroy(&attr)||
       lock(&value)||lock(&value)||destroy(&value)!=err(16)||unlock(&value)||unlock(&value))
        throw std::runtime_error("Recursive mutex lifecycle failed.");
    const auto stale=value;
    if(destroy(&value)||lock(&value)!=err(22))throw std::runtime_error("Destroyed mutex accepted.");
    Handle copy=stale;if(trylock(&copy)!=err(22))throw std::runtime_error("Stale mutex handle accepted.");
    value=0;if(init(&value,nullptr,nullptr)||value==stale||lock(&value)||lock(&value)!=err(11)||trylock(&value)!=err(16))
        throw std::runtime_error("Errorcheck mutex failed.");
    std::int32_t other_unlock{},busy{},timeout{};
    std::thread blocked([&]{other_unlock=unlock(&value);busy=trylock(&value);timeout=timedlock(&value,1000);});blocked.join();
    if(other_unlock!=err(1)||busy!=err(16)||timeout!=err(60)||unlock(&value)||destroy(&value))
        throw std::runtime_error("Contended mutex errors failed.");
    Handle shared{};unsigned counter{};
    if(init(&shared,nullptr,nullptr))throw std::runtime_error("Shared mutex setup failed.");
    mark_libc_mutex(&shared);
    std::int32_t errors[2]{};
    auto worker=[&](int i){for(unsigned n=0;n<2000;++n){if((errors[i]=lock(&shared)))return;++counter;if((errors[i]=unlock(&shared)))return;}};
    std::thread a(worker,0),b(worker,1);a.join();b.join();
    if(errors[0]||errors[1]||counter!=4000||destroy(&shared))throw std::runtime_error("Static mutex contention lost updates.");
    Handle lazy{};
    if(lock(&lazy)||unlock(&lazy))throw std::runtime_error("Lazy pointer mutex initialization failed.");
    mark_libc_mutex(&lazy);
    if(lock(&lazy)||unlock(&lazy)||destroy(&lazy))throw std::runtime_error("Lazy guest mutex flags access failed.");
    shared=0;counter=0;errors[0]=errors[1]=0;
    std::thread lazy_a(worker,0),lazy_b(worker,1);lazy_a.join();lazy_b.join();
    if(errors[0]||errors[1]||counter!=4000||destroy(&shared))
        throw std::runtime_error("Concurrent lazy mutex initialization failed.");
    if(attr_init(nullptr)!=err(22)||lock(reinterpret_cast<Handle*>(1))!=err(14)||attr_init(&attr)||
       attr_type(&attr,0)!=err(22)||attr_type(&attr,3)!=err(45)||attr_protocol(&attr,3)!=err(22)||
       attr_protocol(&attr,1)||attr_protocol(&attr,0)||attr_destroy(&attr))throw std::runtime_error("Mutex attribute/pointer validation failed.");
    Handle untouched{};
    if(destroy(&untouched)||untouched!=2||lock(&untouched)!=err(22))throw std::runtime_error("Static destroy tombstone failed.");
    auto* readonly=static_cast<Handle*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
    if(!readonly)throw std::runtime_error("Mutex read-only fixture allocation failed.");
    const auto denied=init(readonly,nullptr,nullptr);
    VirtualFree(readonly,0,MEM_RELEASE);
    if(denied!=err(14))throw std::runtime_error("Read-only mutex output accepted.");
    if(attr_init(&attr))throw std::runtime_error("Attribute setup failed.");
    Handle wrong_kind=attr;
    if(lock(&wrong_kind)!=err(22)||attr_destroy(&attr))throw std::runtime_error("Mutex accepted attribute handle.");
    std::array<Handle,128> all_attrs{};
    for(auto& h:all_attrs)if(attr_init(&h))throw std::runtime_error("Attribute table allocation failed.");
    Handle extra{};
    if(attr_init(&extra)!=err(35))throw std::runtime_error("Attribute table limit ignored.");
    for(auto& h:all_attrs)if(attr_destroy(&h))throw std::runtime_error("Attribute table cleanup failed.");
    std::array<Handle,256> all_mutexes{};
    for(auto& h:all_mutexes)if(init(&h,nullptr,nullptr))throw std::runtime_error("Mutex table allocation failed.");
    if(init(&extra,nullptr,nullptr)!=err(35))throw std::runtime_error("Mutex table limit ignored.");
    for(auto& h:all_mutexes)if(destroy(&h))throw std::runtime_error("Mutex table cleanup failed.");
    // Exhaust the lifetime address budget only after the functional tests.
    // Destroyed addresses stay reserved even when all live slots are free.
    bool exhausted=false;
    for(std::size_t n=0;n<=GuestMutexArena::capacity;++n){
        Handle fresh{};const auto result=init(&fresh,nullptr,nullptr);
        if(result==err(35)){if(fresh)throw std::runtime_error("Exhaustion published a handle.");exhausted=true;break;}
        if(result || destroy(&fresh))throw std::runtime_error("Guest mutex lifetime budget test failed.");
    }
    extra=0;
    if(!exhausted || init(&extra,nullptr,nullptr)!=err(35) || extra)
        throw std::runtime_error("Guest mutex lifetime address budget ignored.");
    output << "Mutex self-test passed: attributes, registered guest pointers, recursion, contention and static initialization.\n"
              "Libc flags at +0x20, retired pages, stale/forged pointers and concurrent marked mutex verified.\n"
              "Ownership, deadlock, busy, timeout and lifetime address budget errors verified.\n";
}
}
