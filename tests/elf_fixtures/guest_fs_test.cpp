#include "lbp3_host/guest_fs.hpp"
#include "lbp3_host/tls.hpp"
#include <array>
#include <barrier>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::abi;
LONG WINAPI handler(EXCEPTION_POINTERS* fault){return emulate_guest_fs(fault)?EXCEPTION_CONTINUE_EXECUTION:EXCEPTION_CONTINUE_SEARCH;}
void check(bool value){if(!value)throw std::runtime_error("guest FS assertion failed");}
int main(){try{
    constexpr std::array<unsigned char,10> fixture{0x64,0x48,0x8b,0x04,0x25,0,0,0,0,0xc3};
    auto* code=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);check(code);
    std::memcpy(code,fixture.data(),fixture.size());DWORD old{};check(VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old));
    check(FlushInstructionCache(GetCurrentProcess(),code,fixture.size()));
    auto* hook=AddVectoredExceptionHandler(1,&handler);check(hook);
    const auto start=reinterpret_cast<std::uintptr_t>(code);
    using Read=std::uintptr_t (LBP3_GUEST_ABI *)();Read volatile read=reinterpret_cast<Read>(code);
    EXCEPTION_RECORD record{};CONTEXT context{};EXCEPTION_POINTERS fault{&record,&context};
    record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;record.NumberParameters=2;context.Rip=start;
    check(!emulate_guest_fs(&fault));
    {
        ScopedGuestFs scope(start,start+10,1);
        check(!emulate_guest_fs(&fault)); // No attached TLS must not be invented.
        const std::vector<TlsTemplate> templates{{1,512,64,{std::byte{0x42}}},{2,64,16,{}}};
        ThreadTls storage(templates);ScopedThreadTls attached(storage);
        auto* teb=NtCurrentTeb();auto bytes=storage.resolve(1,0,512);
        check(read()==reinterpret_cast<std::uintptr_t>(bytes.data()+512)&&NtCurrentTeb()==teb);
        check(read()-0x1a0==reinterpret_cast<std::uintptr_t>(storage.resolve(1,0x60,1).data()));
        context.Rip=start;record.ExceptionInformation[0]=1;check(!emulate_guest_fs(&fault));
        record.ExceptionInformation[0]=0;record.ExceptionInformation[1]=8;check(!emulate_guest_fs(&fault));
        record.ExceptionInformation[1]=0;context.Rip=start+1;check(!emulate_guest_fs(&fault));
        context.Rip=start+9;check(!emulate_guest_fs(&fault));
        std::barrier sync(2);std::array<std::uintptr_t,2> pointers{};std::array<bool,2> valid{};
        auto work=[&](unsigned i){ThreadTls tls(templates);ScopedThreadTls local(tls);auto block=tls.resolve(1,0,512);
            block[0x60]=static_cast<std::byte>(i+1);pointers[i]=read();sync.arrive_and_wait();
            valid[i]=pointers[i]==reinterpret_cast<std::uintptr_t>(block.data()+512)&&pointers[0]!=pointers[1]&&block[0x60]==static_cast<std::byte>(i+1);
        };
        std::thread a(work,0),b(work,1);a.join();b.join();check(valid[0]&&valid[1]);
    }
    context.Rip=start;check(!emulate_guest_fs(&fault));
    RemoveVectoredExceptionHandler(hook);VirtualFree(code,0,MEM_RELEASE);
    std::cout<<"Guest FS tests passed: exact faulting MOV RAX,FS:[0], main-module static offsets, concurrent TLS isolation, unchanged Windows TEB and rejection outside the contract. Other FS instructions remain unsupported.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
