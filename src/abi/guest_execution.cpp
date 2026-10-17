#include "lbp3_host/guest_execution.hpp"
#include <Windows.h>

namespace lbp3::abi {
namespace {
int capture(EXCEPTION_POINTERS* info,std::uintptr_t low,std::uintptr_t high,ExecutionResult* out) noexcept {
    const auto* record=info->ExceptionRecord;
    const auto rip=static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if(record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || rip<low || rip>=high || record->NumberParameters<2)
        return EXCEPTION_CONTINUE_SEARCH;
    out->code=record->ExceptionCode;out->rip=rip;
    out->access=record->ExceptionInformation[0];out->address=record->ExceptionInformation[1];
    out->thread_id=GetCurrentThreadId();
    return EXCEPTION_EXECUTE_HANDLER;
}
}
// Keep this function free of C++ objects requiring destructors: SEH only here.
__declspec(noinline) void execute_fixture(GuestThreadEntry entry,void* argument,std::uintptr_t low,
                                          std::uintptr_t high,ExecutionResult* out) noexcept {
    __try { out->value=entry(argument); }
    __except(capture(GetExceptionInformation(),low,high,out)) { out->fault=true; }
}
}
