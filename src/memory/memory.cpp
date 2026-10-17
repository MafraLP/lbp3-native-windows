#include "lbp3_host/flexible_memory.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/memory_query.hpp"
#include "lbp3_host/low_memory.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include <Windows.h>
#include <array>
#include <bit>
#include <map>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace lbp3::memory {
struct Region {std::uint64_t length{};DWORD protection{};std::array<char,32> name{};};
struct FlexibleState {std::map<std::uintptr_t,Region> regions;std::uint64_t used{},quota{};};
namespace {
constexpr std::uint64_t page=0x4000;
std::mutex registry;
FlexibleState* active{};
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
bool valid_length(std::uint64_t n){return n && !(n&(page-1));}
bool read_bytes(const void* source,void* dest,SIZE_T size){
    SIZE_T copied{};return source && ReadProcessMemory(GetCurrentProcess(),source,dest,size,&copied) && copied==size;
}
bool writable_pointer(void** address){
    auto at=reinterpret_cast<std::uintptr_t>(address);
    if(!at || at>UINTPTR_MAX-sizeof(void*))return false;
    const auto end=at+sizeof(void*);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE && p!=PAGE_WRITECOPY && p!=PAGE_EXECUTE_READWRITE && p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base || base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }return true;
}
bool publish(void** address,void* value){
    SIZE_T copied{};return writable_pointer(address) &&
        WriteProcessMemory(GetCurrentProcess(),address,&value,sizeof(value),&copied) && copied==sizeof(value);
}
}
ScopedFlexibleMemory::ScopedFlexibleMemory(std::uint64_t quota):state_(std::make_unique<FlexibleState>()){
    if(!valid_length(quota))throw std::runtime_error("Invalid flexible memory quota.");
    std::lock_guard lock(registry);
    if(active)throw std::runtime_error("A flexible memory context is already active.");
    state_->quota=quota;active=state_.get();
}
ScopedFlexibleMemory::~ScopedFlexibleMemory(){
    std::lock_guard lock(registry);
    for(const auto& [address,region]:state_->regions){gpu_notify_unmap(address,region.length);VirtualFree(reinterpret_cast<void*>(address),0,MEM_RELEASE);}
    active=nullptr;
}
std::int32_t LBP3_GUEST_ABI map_named(void** output,std::uint64_t length,int protection,int flags,const char* name) noexcept {
    try{std::lock_guard lock(registry);
        if(!active)return error(22);
        if(!valid_length(length))return error(22);
        if(!writable_pointer(output))return error(14);
        void* hint{};if(!read_bytes(output,&hint,sizeof(hint)))return error(14);
        // Observed libc calls request arbitrary placement, flags=0, CPU RW.
        if(flags || hint)return error(45);
        DWORD native{};
        switch(protection){case 0:native=PAGE_NOACCESS;break;case 1:native=PAGE_READONLY;break;
            case 3:native=PAGE_READWRITE;break;default:return error(45);}
        Region region{length,native,{}};
        if(!name)return error(14);
        bool terminated=false;
        for(std::size_t i=0;i<region.name.size();++i){
            if(!read_bytes(name+i,&region.name[i],1))return error(14);
            if(!region.name[i]){terminated=true;break;}
        }
        if(!terminated)return error(63);
        if(length>active->quota-active->used || active->regions.size()>=256)return error(12);
        auto* address=allocate_low(length,MEM_RESERVE|MEM_COMMIT,native);
        if(!address)return error(12);
        const auto key=reinterpret_cast<std::uintptr_t>(address);
        try{active->regions.emplace(key,region);}catch(...){VirtualFree(address,0,MEM_RELEASE);throw;}
        if(!publish(output,address)){
            active->regions.erase(key);VirtualFree(address,0,MEM_RELEASE);return error(14);
        }
        active->used+=length;gpu_notify_map(key,length);return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI unmap(void* address,std::uint64_t length) noexcept {
    try{std::lock_guard lock(registry);
        const auto start=reinterpret_cast<std::uintptr_t>(address);
        if(!active || !start || start&(page-1) || !valid_length(length) || length>UINTPTR_MAX-start)return error(22);
        const auto it=active->regions.find(start);
        if(it==active->regions.end())return direct_unmap(address,length);
        if(it->second.length!=length)return error(45); // Partial unmapping requires interval splitting.
        gpu_notify_unmap(start,length);
        if(!VirtualFree(address,0,MEM_RELEASE))return error(5);
        active->used-=length;active->regions.erase(it);return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI lock_pages(void* address,std::uint64_t length) noexcept {
    try{std::lock_guard lock(registry);
        const auto start=reinterpret_cast<std::uintptr_t>(address);
        if(!active || !start || start&(page-1) || !valid_length(length) || length>UINTPTR_MAX-start)return error(22);
        auto it=active->regions.upper_bound(start);
        if(it==active->regions.begin())return error(22);
        --it;const auto offset=start-it->first;
        if(offset>=it->second.length || length>it->second.length-offset)return error(22);
        if(it->second.protection==PAGE_NOACCESS)return error(14);
        // Real host page locking, never report success without locking.
        return VirtualLock(address,static_cast<SIZE_T>(length)) ? 0 : error(12);
    }catch(...){return error(5);}
}
bool owns_writable_range(std::uintptr_t start,std::uint64_t length) noexcept {
    try{std::lock_guard lock(registry);if(!active||!length||length>UINTPTR_MAX-start)return false;
        auto it=active->regions.upper_bound(start);if(it==active->regions.begin())return false;--it;
        const auto offset=start-it->first;
        return offset<it->second.length&&length<=it->second.length-offset&&it->second.protection==PAGE_READWRITE;
    }catch(...){return false;}
}
std::optional<QueryInfo> query_owned(std::uintptr_t address,bool next){
    std::lock_guard lock(registry);std::optional<QueryInfo> result;
    if(active){auto it=active->regions.upper_bound(address);
        if(it!=active->regions.begin()){auto previous=std::prev(it);if(address-previous->first<previous->second.length)it=previous;}
        if(it!=active->regions.end()&&(next||address>=it->first)){
            const auto& region=it->second;const int protection=region.protection==PAGE_READWRITE?3:region.protection==PAGE_READONLY?1:0;
            result=QueryInfo{it->first,it->first+region.length,0,protection,0,0x11,region.name};
        }
    }
    if(const auto view=direct_view(address,next))if(!result||view->address<result->start)
        result=QueryInfo{view->address,view->address+view->length,view->physical,view->protection,view->memory_type,0x12,view->name};
    return result;
}

void self_test(std::ostream& output,std::uintptr_t mapper,std::uintptr_t unmapper,std::uintptr_t locker){
    using Map=std::int32_t (LBP3_GUEST_ABI *)(void**,std::uint64_t,int,int,const char*) noexcept;
    using Range=std::int32_t (LBP3_GUEST_ABI *)(void*,std::uint64_t) noexcept;
    Map volatile map=reinterpret_cast<Map>(mapper);
    Range volatile release=reinterpret_cast<Range>(unmapper),pin=reinterpret_cast<Range>(locker);
    void* p{};
    if(map(&p,page,3,0,"outside")!=error(22))throw std::runtime_error("Flexible map escaped its context.");
    void* orphan{};
    {
        ScopedFlexibleMemory scope(2*page);
        bool rejected=false;try{ScopedFlexibleMemory nested;}catch(const std::runtime_error&){rejected=true;}
        if(!rejected)throw std::runtime_error("Nested memory context accepted.");
        if(map(&p,2*page,3,0,"libc-heap") || reinterpret_cast<std::uintptr_t>(p)%page)
            throw std::runtime_error("Flexible map failed or is misaligned.");
        auto* bytes=static_cast<std::byte*>(p);
        for(std::size_t i=0;i<2*page;++i)if(bytes[i]!=std::byte{})throw std::runtime_error("Flexible map not zero initialized.");
        bytes[0]=std::byte{0x12};bytes[2*page-1]=std::byte{0x34};
        void* extra{};
        if(map(&extra,page,3,0,"quota")!=error(12)||extra || release(p,page)!=error(45) ||
           release(bytes+1,page)!=error(22) || pin(bytes+2*page,page)!=error(22))
            throw std::runtime_error("Flexible quota/range checks failed.");
        const auto pinned=pin(bytes+page,page);
        if(pinned && pinned!=error(12))throw std::runtime_error("Unexpected host page locking result.");
        if(bytes[0]!=std::byte{0x12} || bytes[2*page-1]!=std::byte{0x34})throw std::runtime_error("Rejected range operation damaged map.");
        const auto old=p;
        if(release(p,2*page)||release(old,2*page)!=error(22))throw std::runtime_error("Flexible release/double release failed.");
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(old,&info,sizeof(info))||info.State!=MEM_FREE)throw std::runtime_error("Flexible map not released.");
        p=nullptr;
        const std::array<char,32> long_name{};auto nonterminated=long_name;nonterminated.fill('x');
        if(map(nullptr,page,3,0,"bad")!=error(14)||map(&p,1,3,0,"bad")!=error(22)||
           map(&p,UINT64_MAX,3,0,"bad")!=error(22)||map(&p,page,7,0,"bad")!=error(45)||
           map(&p,page,3,16,"bad")!=error(45)||map(&p,page,3,0,nullptr)!=error(14)||
           map(&p,page,3,0,reinterpret_cast<const char*>(1))!=error(14)||
           map(&p,page,3,0,nonterminated.data())!=error(63)||p)
            throw std::runtime_error("Flexible map invalid inputs accepted.");
        p=reinterpret_cast<void*>(page);
        if(map(&p,page,3,0,"hint")!=error(45)||p!=reinterpret_cast<void*>(page))throw std::runtime_error("Unsupported address request accepted.");
        p=nullptr;
        auto* readonly=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));
        if(!readonly)throw std::runtime_error("Read-only output fixture failed.");
        const auto denied=map(readonly,page,3,0,"readonly");VirtualFree(readonly,0,MEM_RELEASE);
        if(denied!=error(14))throw std::runtime_error("Read-only memory output accepted.");
        int foreign{};
        if(release(&foreign,page)!=error(22)||pin(&foreign,page)!=error(22))throw std::runtime_error("Foreign host memory accepted.");
        if(map(&p,page,1,0,"ro"))throw std::runtime_error("Read-only flexible map failed.");
        if(!VirtualQuery(p,&info,sizeof(info))||info.Protect!=PAGE_READONLY||release(p,page))throw std::runtime_error("Read-only protection mismatch.");
        p=nullptr;
        if(map(&p,page,0,0,"none"))throw std::runtime_error("No-access flexible map failed.");
        if(!VirtualQuery(p,&info,sizeof(info))||info.Protect!=PAGE_NOACCESS||pin(p,page)!=error(14)||release(p,page))
            throw std::runtime_error("No-access protection mismatch.");
        std::int32_t errors[2]{};
        auto worker=[&](unsigned index){for(unsigned n=0;n<32;++n){void* block{};
            if((errors[index]=map(&block,page,3,0,"worker")))return;
            static_cast<std::byte*>(block)[page-1]=std::byte{0x77};
            if((errors[index]=release(block,page)))return;
        }};
        std::thread a(worker,0),b(worker,1);a.join();b.join();
        if(errors[0]||errors[1])throw std::runtime_error("Concurrent flexible memory lifecycle failed.");
        if(map(&orphan,page,3,0,"scope-cleanup"))throw std::runtime_error("Scope cleanup fixture failed.");
    }
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(orphan,&info,sizeof(info))||info.State!=MEM_FREE || release(orphan,page)!=error(22))
        throw std::runtime_error("Memory scope leaked mappings or registry.");
    {ScopedFlexibleMemory clean(page);void* fresh{};
        if(map(&fresh,page,3,0,"fresh")||release(fresh,page))throw std::runtime_error("Memory quota leaked into next context.");}
    {ScopedFlexibleMemory limited(257*page);std::array<void*,256> blocks{};
        for(auto& block:blocks)if(map(&block,page,3,0,"table"))throw std::runtime_error("Memory table capacity fixture failed.");
        void* extra{};
        if(map(&extra,page,3,0,"table-full")!=error(12)||extra)throw std::runtime_error("Memory mapping count limit ignored.");
        for(auto block:blocks)if(release(block,page))throw std::runtime_error("Memory table capacity cleanup failed.");
    }
    output << "Flexible memory self-test passed: SysV map/mlock/munmap, 16 KiB alignment, zero fill, protections and quota.\n"
              "Ownership, unsupported partial unmap, invalid pointers, concurrent workers and scope cleanup verified.\n"
              "Host lock may return ENOMEM when Windows working-set quota is insufficient. No libc code executed.\n";
}
}
