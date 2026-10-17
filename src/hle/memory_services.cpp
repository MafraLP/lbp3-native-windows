#include "lbp3_host/memory_services.hpp"
#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <bit>
namespace lbp3::hle {
namespace {
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
bool write(memory::QueryInfo* target,const memory::QueryInfo& value){
    auto at=reinterpret_cast<std::uintptr_t>(target);if(!at||at>UINTPTR_MAX-sizeof(value))return false;
    const auto end=at+sizeof(value);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),target,&value,sizeof(value),&n)&&n==sizeof(value);
}
}
std::int32_t LBP3_GUEST_ABI virtual_query(const void* pointer,std::int32_t flags,memory::QueryInfo* output,std::uint64_t size) noexcept {
    try{
        if(size!=sizeof(memory::QueryInfo)||flags<0||flags>1)return error(22);
        if(!output)return error(14);
        const auto address=reinterpret_cast<std::uintptr_t>(pointer);
        auto result=memory::query_owned(address,flags==1);
        const auto module=abi::query_module_memory(address,flags==1);
        if(module&&(!result||module->start<result->start))result=module;
        if(!result)return error(13);
        return write(output,*result)?0:error(14);
    }catch(...){return error(5);}
}
std::optional<std::uintptr_t> resolve_memory_query(const HostImport& r){
    if(r.type==2&&r.library=="libkernel"&&r.module=="libkernel"&&r.library_version==1&&r.module_version==0x0101&&r.nid=="rVjRvHJ0X6c")return reinterpret_cast<std::uintptr_t>(&virtual_query);
    return std::nullopt;
}
}
