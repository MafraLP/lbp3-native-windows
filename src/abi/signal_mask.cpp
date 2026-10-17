#include "lbp3_host/signal_mask.hpp"
#include <Windows.h>

namespace lbp3::abi {
namespace {
thread_local SignalMask mask{};
void unmaskable(SignalMask& value){value[0]&=~((1u<<8)|(1u<<16));} // SIGKILL=9, SIGSTOP=17.
bool read(const SignalMask* source,SignalMask& value){SIZE_T n{};
    return source&&ReadProcessMemory(GetCurrentProcess(),source,&value,sizeof(value),&n)&&n==sizeof(value);}
bool write(SignalMask* target,const SignalMask& value){
    auto at=reinterpret_cast<std::uintptr_t>(target);if(!at||at>UINTPTR_MAX-sizeof(value))return false;
    const auto end=at+sizeof(value);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),target,&value,sizeof(value),&n)&&n==sizeof(value);
}
}
SignalMask current_signal_mask() noexcept {return mask;}
void inherit_signal_mask(const SignalMask& initial) noexcept {mask=initial;unmaskable(mask);}
int fill_signal_mask(SignalMask* output,bool full) noexcept {
    SignalMask value{};if(full)value.fill(UINT32_MAX);
    return write(output,value)?0:14;
}
int change_signal_mask(int how,const SignalMask* input,SignalMask* previous) noexcept {
    SignalMask value{};
    // Snapshot input first: POSIX permits aliased input/output storage.
    if(input){if(how<1||how>3)return 22;if(!read(input,value))return 14;}
    if(previous&&!write(previous,mask))return 14;
    if(!input)return 0;
    for(unsigned i=0;i<mask.size();++i){
        if(how==1)mask[i]|=value[i];
        else if(how==2)mask[i]&=~value[i];
        else mask[i]=value[i];
    }unmaskable(mask);return 0;
}
}
