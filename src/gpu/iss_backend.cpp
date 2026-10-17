#include "lbp3_host/iss_backend.hpp"
#include "lbp3_host/filesystem_exports.hpp"
#include "lbp3_host/guest_filesystem.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#if defined(LBP3_HAS_ISS_GPU)
#include "common/ntapi.h"
#include "tlggpu.h"

extern "C" {
extern std::uint64_t runtime_disabled_optimizations;
extern __declspec(thread) void* runtime_fault_recover;
std::uint64_t runtime_process_time_us();
std::uint64_t runtime_process_time_counter();
std::uint64_t runtime_tsc_frequency();
std::uint64_t runtime_read_tsc();
std::int32_t* runtime_errno();
void runtime_report();
void runtime_restart();
void runtime_thread_attach_host(const char* name);
int runtime_file_translate(const char* guest, char* output, std::size_t capacity);
}

extern "C" std::uint64_t runtime_disabled_optimizations{};
extern "C" __declspec(thread) void* runtime_fault_recover{};

namespace {
const auto process_epoch = std::chrono::steady_clock::now();
std::atomic<bool> backend_initialized{};

std::uint64_t elapsed_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - process_epoch).count());
}
}

extern "C" std::uint64_t runtime_process_time_us() { return elapsed_ns() / 1000; }
extern "C" std::uint64_t runtime_process_time_counter() { return elapsed_ns(); }
extern "C" std::uint64_t runtime_tsc_frequency() { return 1000000000ull; }
extern "C" std::uint64_t runtime_read_tsc() { return elapsed_ns(); }
extern "C" std::int32_t* runtime_errno() { return lbp3::hle::guest_errno_address(); }
extern "C" void runtime_report() {
    OutputDebugStringW(L"ISS GPU backend requested runtime report.\n");
}
extern "C" void runtime_restart() {
    TerminateProcess(GetCurrentProcess(), 0xe04c4401u);
}
extern "C" void runtime_thread_attach_host(const char* name) {
    if (!name) return;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
    if (length <= 1 || length > 128) return;
    wchar_t wide[128]{};
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, length))
        (void)SetThreadDescription(GetCurrentThread(), wide);
}
extern "C" int runtime_file_translate(const char* guest, char* output, std::size_t capacity) {
    return lbp3::io::translate_path(guest, output, capacity) == 0 ? 0 : -1;
}
extern "C" [[noreturn]] void tlg_longjmp(unsigned long long buffer[32], int value);

namespace lbp3::gpu {
int initialize_iss_backend(const char* title, const char* serial, const char* user_directory,
                           std::uint32_t sdk_version, std::uint32_t psf_attributes,
                           std::int32_t width, std::int32_t height) {
    if (backend_initialized.load(std::memory_order_acquire)) return ERROR_ALREADY_INITIALIZED;
    const TlgGpuConfig config{title, serial, user_directory, sdk_version, psf_attributes, width, height};
    Common::NtApi::Initialize();
    tlggpu_register_kernel();
    const int status = tlggpu_init(&config);
    if (status == 0) backend_initialized.store(true, std::memory_order_release);
    return status;
}

std::optional<std::uintptr_t> resolve_iss_backend_import(const HostImport& request) {
    // The vendored ISS VideoOut exports use module version 0 in the retail
    // ELF import table; libkernel/GNM exports use 0x0101. Keep both contracts
    // scoped to their exact library/module pair because tlggpu_resolve itself
    // indexes by NID only.
    const bool video_out = request.library == "libSceVideoOut" &&
                           request.module == "libSceVideoOut" &&
                           request.module_version == 0;
    const bool versioned_gpu = request.module_version == 0x0101 &&
                               request.library == request.module &&
                               (request.library == "libSceGnmDriver" ||
                                request.library == "libkernel" ||
                                request.library == "libSceAvPlayer");
    if (!backend_initialized.load(std::memory_order_acquire) || request.type != 2 ||
        request.library_version != 1 || (!video_out && !versioned_gpu))
        return std::nullopt;
    const std::string scoped = std::string(request.nid) + "#" + std::string(request.library) +
                               "#" + std::string(request.module);
    const auto address = tlggpu_resolve(scoped.c_str());
    if (!address) return std::nullopt;
    return address;
}

bool iss_backend_handle_fault(EXCEPTION_POINTERS* exception, void* address) noexcept {
    if (tlggpu_handle_fault(exception, address) != 0) return true;
    if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2 || !runtime_fault_recover)
        return false;
    auto* context = exception->ContextRecord;
    auto* buffer = static_cast<unsigned long long*>(runtime_fault_recover);
    runtime_fault_recover = nullptr;
    context->Rsp = ((context->Rsp - 128) & ~static_cast<DWORD64>(15)) - 40;
    context->Rcx = reinterpret_cast<DWORD64>(buffer);
    context->Rdx = 1;
    context->Rip = reinterpret_cast<DWORD64>(&tlg_longjmp);
    return true;
}
}
#else
namespace lbp3::gpu {
int initialize_iss_backend(const char*, const char*, const char*, std::uint32_t, std::uint32_t,
                           std::int32_t, std::int32_t) { return ERROR_NOT_SUPPORTED; }
std::optional<std::uintptr_t> resolve_iss_backend_import(const HostImport&) { return std::nullopt; }
bool iss_backend_handle_fault(EXCEPTION_POINTERS*, void*) noexcept { return false; }
}
#endif
