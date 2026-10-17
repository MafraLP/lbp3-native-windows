#include "lbp3_host/guest_fs.hpp"
#include "lbp3_host/tls.hpp"
#include <array>
#include <atomic>
#include <stdexcept>
namespace lbp3::abi {
namespace {std::atomic<const ScopedGuestFs*> active{};}
ScopedGuestFs::ScopedGuestFs(std::uintptr_t first,std::uintptr_t last,std::uint64_t id):begin(first),end(last),tls_id(id){
    if(!first||last<first||last-first<9||!id)throw std::runtime_error("Invalid guest FS emulation range.");
    const ScopedGuestFs* expected{};
    if(!active.compare_exchange_strong(expected,this))throw std::runtime_error("Guest FS emulation already active.");
}
ScopedGuestFs::~ScopedGuestFs(){active.store(nullptr);}
bool emulate_guest_fs(EXCEPTION_POINTERS* fault) noexcept {
    const auto* scope=active.load();if(!scope||!fault||!fault->ExceptionRecord||!fault->ContextRecord)return false;
    const auto& exception=*fault->ExceptionRecord;auto& context=*fault->ContextRecord;
    if(exception.ExceptionCode!=EXCEPTION_ACCESS_VIOLATION||exception.NumberParameters<2||exception.ExceptionInformation[0]||exception.ExceptionInformation[1])return false;
    if(context.Rip<scope->begin||context.Rip>=scope->end||scope->end-context.Rip<9)return false;
    constexpr std::array<unsigned char,9> instruction{0x64,0x48,0x8b,0x04,0x25,0,0,0,0};
    std::array<unsigned char,9> bytes{};SIZE_T copied{};
    if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(context.Rip),bytes.data(),bytes.size(),&copied)||copied!=bytes.size()||bytes!=instruction)return false;
    auto* pointer=current_static_thread_pointer(scope->tls_id);if(!pointer)return false;
    context.Rax=reinterpret_cast<DWORD64>(pointer);context.Rip+=instruction.size();return true;
}
}
