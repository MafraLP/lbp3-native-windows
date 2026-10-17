#include "lbp3_host/iss_backend.hpp"
#include <iostream>
#include <stdexcept>

extern "C" __declspec(thread) void* runtime_fault_recover;
extern "C" [[noreturn]] void tlg_longjmp(unsigned long long buffer[32], int value);

int main() {
    CONTEXT context{};
    context.Rsp = 0x100000;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[1] = 0x12345000;
    EXCEPTION_POINTERS pointers{&record, &context};
    unsigned long long recovery[32]{};
    runtime_fault_recover = recovery;
    if (!lbp3::gpu::iss_backend_handle_fault(&pointers, reinterpret_cast<void*>(0x12345000)) ||
        runtime_fault_recover || context.Rcx != reinterpret_cast<DWORD64>(recovery) ||
        context.Rdx != 1 || context.Rip != reinterpret_cast<DWORD64>(&tlg_longjmp) ||
        context.Rsp != (((0x100000ull - 128) & ~15ull) - 40)) {
        std::cerr << "ISS access-violation recovery context was not prepared correctly.\n";
        return 1;
    }
    std::cout << "ISS fault bridge passed GPU dispatch fallback to Win64 speculative-read recovery.\n";
    return 0;
}
