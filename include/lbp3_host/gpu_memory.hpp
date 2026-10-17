#pragma once
#include <Windows.h>
#include <cstdint>
#include <memory>

namespace lbp3::memory {
using GpuFaultHandler = bool (*)(EXCEPTION_POINTERS*, void*) noexcept;

// Installs an access-violation dispatcher. The handler must filter unrelated faults.
class ScopedGpuMemoryTracking {
public:
    explicit ScopedGpuMemoryTracking(GpuFaultHandler handler);
    ~ScopedGpuMemoryTracking();
    ScopedGpuMemoryTracking(const ScopedGpuMemoryTracking&) = delete;
    ScopedGpuMemoryTracking& operator=(const ScopedGpuMemoryTracking&) = delete;
private:
    void* handler_cookie_{};
};

// Mirrors the ISS AddressSpace::Protect contract. `read/write` describe CPU
// permissions while the GPU owns the page; executable permission is preserved.
bool gpu_protect(std::uintptr_t address, std::uint64_t size, bool read, bool write) noexcept;
bool gpu_page_is_protected(std::uintptr_t address) noexcept;
bool gpu_copy_backing(std::uintptr_t guest_address, void* buffer, std::uint64_t size,
                      bool guest_to_host) noexcept;
void gpu_notify_map(std::uintptr_t address, std::uint64_t size) noexcept;
void gpu_notify_unmap(std::uintptr_t address, std::uint64_t size) noexcept;
void gpu_notify_invalidate(std::uintptr_t address, std::uint64_t size) noexcept;
}

extern "C" void runtime_memory_gpu_protect(std::uintptr_t, std::uint64_t, int, int);
extern "C" int runtime_memory_is_mapped(std::uintptr_t, std::uint64_t);
extern "C" std::uint64_t runtime_memory_clamp(std::uintptr_t, std::uint64_t);
extern "C" int runtime_memory_region(std::uintptr_t, std::uintptr_t*, std::uintptr_t*, int*);
extern "C" int runtime_memory_read_backing(std::uintptr_t, void*, std::uint64_t);
extern "C" int runtime_memory_write_backing(std::uintptr_t, const void*, std::uint64_t);
using RuntimeGpuRange = void (*)(std::uintptr_t, std::uint64_t);
extern "C" void runtime_memory_set_gpu_hooks(RuntimeGpuRange, RuntimeGpuRange, RuntimeGpuRange);
