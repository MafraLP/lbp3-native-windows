#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/low_memory.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <map>
#include <mutex>
#include <stdexcept>
#include <array>
#include <cstdio>
namespace lbp3::memory {
namespace {
constexpr std::uint64_t page=0x4000;
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
bool aligned(std::uint64_t n){return n&&!(n&(page-1));}
bool power(std::uint64_t n){return aligned(n)&&!(n&(n-1));}
bool read(const void* p,void* value,std::size_t size){SIZE_T n{};return p&&ReadProcessMemory(GetCurrentProcess(),p,value,size,&n)&&n==size;}
bool writable(const void* p){auto at=reinterpret_cast<std::uintptr_t>(p);if(!at||at>UINTPTR_MAX-8)return false;
    const auto end=at+8;
    while(at<end){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto prot=info.Protect&255;if(prot!=PAGE_READWRITE&&prot!=PAGE_WRITECOPY&&prot!=PAGE_EXECUTE_READWRITE&&prot!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }return true;
}
template<class T> bool write(T* p,T value){static_assert(sizeof(T)==8);SIZE_T n{};return writable(p)&&WriteProcessMemory(GetCurrentProcess(),p,&value,8,&n)&&n==8;}
struct Allocation {std::uint64_t length{};HANDLE section{};int type{};~Allocation(){if(section)CloseHandle(section);}};
struct View {std::uint64_t physical{},length{};std::array<char,32> name{};int protection{};};
std::mutex registry;
DirectState* active{};
}
struct DirectState {std::uint64_t capacity{};std::map<std::uint64_t,std::unique_ptr<Allocation>> allocations;std::map<std::uintptr_t,View> views;};
ScopedDirectMemory::ScopedDirectMemory(std::uint64_t capacity):state_(std::make_unique<DirectState>()){
    if(!aligned(capacity)||capacity>static_cast<std::uint64_t>(INT64_MAX))throw std::runtime_error("Invalid direct memory capacity.");
    std::lock_guard lock(registry);if(active)throw std::runtime_error("Direct memory scope already active.");state_->capacity=capacity;active=state_.get();
}
ScopedDirectMemory::~ScopedDirectMemory(){std::lock_guard lock(registry);for(const auto& [address,view]:state_->views){gpu_notify_unmap(address,view.length);UnmapViewOfFile(reinterpret_cast<void*>(address));}active=nullptr;}
std::uint64_t LBP3_GUEST_ABI direct_size() noexcept {std::lock_guard lock(registry);return active?active->capacity:0;}
std::int32_t LBP3_GUEST_ABI direct_allocate(std::int64_t low,std::int64_t high,std::uint64_t length,std::uint64_t alignment,int type,std::int64_t* output) noexcept {
    try{std::lock_guard lock(registry);if(!active||low<0||high<0||!aligned(length)||type<0||type>10)return error(22);
        if(!alignment)alignment=page;if(!power(alignment)||alignment>active->capacity)return error(22);
        if(!writable(output))return error(14);
        const auto end=std::min<std::uint64_t>(high,active->capacity);
        if(static_cast<std::uint64_t>(low)>=end||length>end-static_cast<std::uint64_t>(low)||active->allocations.size()>=256)return error(28);
        auto start=(static_cast<std::uint64_t>(low)+alignment-1)&~(alignment-1);
        for(const auto& [address,allocation]:active->allocations){
            if(start> end||length>end-start)return error(28);
            if(start<address+allocation->length&&address<start+length)start=(address+allocation->length+alignment-1)&~(alignment-1);
        }
        if(start>end||length>end-start)return error(28);
        auto allocation=std::make_unique<Allocation>();allocation->length=length;allocation->type=type;
        // Pagefile-backed sections preserve coherent aliases without copying views.
        allocation->section=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,static_cast<DWORD>(length>>32),static_cast<DWORD>(length),nullptr);
        if(!allocation->section)return error(12);
        active->allocations.emplace(start,std::move(allocation));
        if(!write(output,static_cast<std::int64_t>(start))){active->allocations.erase(start);return error(14);}return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI direct_map_named(void** output,std::uint64_t length,int protection,int flags,std::int64_t physical,std::uint64_t alignment,const char* name) noexcept {
    try{std::lock_guard lock(registry);if(!active||physical<0||static_cast<std::uint64_t>(physical)%page||!aligned(length))return error(22);
        if(!alignment)alignment=page;if(!power(alignment)||alignment>active->capacity)return error(22);
        if(!writable(output))return error(14);void* hint{};if(!read(output,&hint,8))return error(14);
        View view{static_cast<std::uint64_t>(physical),length,{},protection};bool terminated=false;
        for(std::size_t i=0;i<view.name.size();++i){if(!read(name?name+i:nullptr,&view.name[i],1))return error(14);if(!view.name[i]){terminated=true;break;}}
        if(!terminated)return error(63);
        // With flags==0, addr is a placement hint rather than a fixed mapping
        // request. Try it exactly when aligned and free; if the OS cannot place
        // the section there, return a safe alternate address through *output.
        if(flags)return error(45);
        // GPU visibility is retained as guest metadata. Views share the same
        // section bytes; GPU command execution is a separate, unimplemented API.
        if(protection&~0x37)return error(22);
        DWORD access{},native{};switch(protection&7){case 0:access=FILE_MAP_READ;native=PAGE_NOACCESS;break;case 1:access=FILE_MAP_READ;native=PAGE_READONLY;break;case 3:access=FILE_MAP_WRITE;native=PAGE_READWRITE;break;default:return error(45);}
        auto found=active->allocations.upper_bound(static_cast<std::uint64_t>(physical));if(found==active->allocations.begin())return error(22);--found;
        const auto offset=static_cast<std::uint64_t>(physical)-found->first;
        if(offset>=found->second->length||length>found->second->length-offset)return error(22);
        if(offset%0x10000)return error(45); // Windows section-offset granularity; no silent rounding.
        if(active->views.size()>=256)return error(12);
        void* address{};
        alignment=std::max<std::uint64_t>(alignment,0x10000);
        if(hint&&reinterpret_cast<std::uintptr_t>(hint)%alignment==0){
            address=MapViewOfFileEx(found->second->section,access,static_cast<DWORD>(offset>>32),static_cast<DWORD>(offset),static_cast<SIZE_T>(length),hint);
        }
        if(!address){
            if(length>SIZE_MAX-alignment)return error(22);
            for(unsigned retry=0;retry<8&&!address;++retry){
                auto* reservation=allocate_low(length+alignment,MEM_RESERVE,PAGE_NOACCESS);if(!reservation)break;
                const auto base=reinterpret_cast<std::uintptr_t>(reservation);
                const auto target=(base+alignment-1)&~(alignment-1);VirtualFree(reservation,0,MEM_RELEASE);
                // Exact placement fails on a race; it never replaces another mapping.
                address=MapViewOfFileEx(found->second->section,access,static_cast<DWORD>(offset>>32),static_cast<DWORD>(offset),static_cast<SIZE_T>(length),reinterpret_cast<void*>(target));
            }
        }
        if(!address)return error(12);
        if((protection&7)==0){DWORD old{};if(!VirtualProtect(address,static_cast<SIZE_T>(length),native,&old)){UnmapViewOfFile(address);return error(5);}}
        const auto key=reinterpret_cast<std::uintptr_t>(address);
        try{active->views.emplace(key,view);}catch(...){UnmapViewOfFile(address);throw;}
        if(!write(output,address)){active->views.erase(key);UnmapViewOfFile(address);return error(14);}
        gpu_notify_map(key,length);return 0;
    }catch(const std::bad_alloc&){return error(12);}catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI direct_map(void** output,std::uint64_t length,int protection,int flags,std::int64_t physical,std::uint64_t alignment) noexcept {
    return direct_map_named(output,length,protection,flags,physical,alignment,"anon");
}
std::int32_t direct_unmap(void* address,std::uint64_t length) noexcept {
    try{std::lock_guard lock(registry);if(!active)return error(22);const auto found=active->views.find(reinterpret_cast<std::uintptr_t>(address));
        if(found==active->views.end())return error(22);if(found->second.length!=length)return error(45);
        gpu_notify_unmap(found->first,found->second.length);
        if(!UnmapViewOfFile(address))return error(5);active->views.erase(found);return 0;
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI direct_release(std::int64_t physical,std::uint64_t length) noexcept {
    try{std::lock_guard lock(registry);if(!active||physical<0||static_cast<std::uint64_t>(physical)%page||!aligned(length))return error(22);
        const auto found=active->allocations.find(static_cast<std::uint64_t>(physical));if(found==active->allocations.end())return error(22);
        if(found->second->length!=length)return error(45);
        for(auto it=active->views.begin();it!=active->views.end();){const auto& view=it->second;
            if(view.physical>=static_cast<std::uint64_t>(physical)&&view.physical-static_cast<std::uint64_t>(physical)<length){
                gpu_notify_unmap(it->first,view.length);
                if(!UnmapViewOfFile(reinterpret_cast<void*>(it->first)))return error(5);it=active->views.erase(it);
            }else ++it;
        }active->allocations.erase(found);return 0;
    }catch(...){return error(5);}
}
std::optional<std::uintptr_t> resolve_direct_memory(const HostImport& r){
    if(r.type!=2||r.library!="libkernel"||r.module!="libkernel"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="pO96TwzOm5E")return reinterpret_cast<std::uintptr_t>(&direct_size);
    if(r.nid=="rTXw65xmLIA")return reinterpret_cast<std::uintptr_t>(&direct_allocate);
    if(r.nid=="L-Q3LEjIbgA")return reinterpret_cast<std::uintptr_t>(&direct_map);
    if(r.nid=="NcaWUxfMNIQ")return reinterpret_cast<std::uintptr_t>(&direct_map_named);
    if(r.nid=="MBuItvba6z8")return reinterpret_cast<std::uintptr_t>(&direct_release);
    return std::nullopt;
}
std::optional<DirectViewInformation> direct_view(std::uintptr_t address,bool next){
    std::lock_guard lock(registry);if(!active)return std::nullopt;
    auto it=active->views.upper_bound(address);
    if(it!=active->views.begin()){const auto previous=std::prev(it);if(address-previous->first<previous->second.length)it=previous;}
    if(it==active->views.end()||(!next&&address<it->first))return std::nullopt;
    const auto& view=it->second;auto allocation=active->allocations.upper_bound(view.physical);
    if(allocation==active->allocations.begin())return std::nullopt;--allocation;
    return DirectViewInformation{it->first,view.physical,view.length,view.protection,allocation->second->type,view.name};
}
}
