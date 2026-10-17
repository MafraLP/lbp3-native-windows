#include "lbp3_host/low_memory.hpp"
#include <bit>
namespace lbp3::memory {
void* allocate_low(std::uint64_t length,DWORD allocation,DWORD protection) noexcept {
    if(!length||length>guest_address_limit-guest_address_min||!(allocation&MEM_RESERVE)){
        SetLastError(ERROR_INVALID_PARAMETER);return nullptr;
    }
    using Allocate2=void* (WINAPI *)(HANDLE,void*,SIZE_T,ULONG,ULONG,MEM_EXTENDED_PARAMETER*,ULONG);
    static const auto allocate2=std::bit_cast<Allocate2>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),"VirtualAlloc2"));
    if(allocate2){
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.LowestStartingAddress=reinterpret_cast<void*>(guest_address_min);
        requirements.HighestEndingAddress=reinterpret_cast<void*>(guest_address_limit-1);
        requirements.Alignment=0x10000;
        MEM_EXTENDED_PARAMETER parameter{};parameter.Type=MemExtendedParameterAddressRequirements;parameter.Pointer=&requirements;
        if(auto* result=allocate2(GetCurrentProcess(),nullptr,static_cast<SIZE_T>(length),allocation,protection,&parameter,1))return result;
    }
    // Compatibility fallback: examine free regions and reserve atomically at
    // their start. A competing allocation only causes a retry, never overwrite.
    auto cursor=guest_address_min;
    for(unsigned attempts=0;attempts<4096&&cursor<guest_address_limit;++attempts){
        MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(cursor),&info,sizeof(info)))break;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base)break;const auto end=base+info.RegionSize;
        if(end<=cursor)break;
        if(info.State==MEM_FREE){
            const auto candidate=(cursor+0xffff)&~std::uintptr_t{0xffff};
            if(candidate<guest_address_limit&&candidate<=end&&length<=end-candidate&&length<=guest_address_limit-candidate){
                if(auto* result=VirtualAlloc(reinterpret_cast<void*>(candidate),static_cast<SIZE_T>(length),allocation,protection))return result;
                if(GetLastError()!=ERROR_INVALID_ADDRESS)return nullptr;
                cursor=candidate+0x10000;continue;
            }
        }
        cursor=end;
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;
}
}
