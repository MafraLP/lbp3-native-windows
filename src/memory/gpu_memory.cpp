#include "lbp3_host/gpu_memory.hpp"
#include "lbp3_host/low_memory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace lbp3::memory {
namespace {
constexpr std::uintptr_t page_size = 0x1000;
std::mutex protection_mutex;
struct TrackedPage { DWORD original{}; std::uintptr_t allocation_base{}; };
std::map<std::uintptr_t, TrackedPage> original_protections;
std::atomic<GpuFaultHandler> fault_handler{};
std::atomic<RuntimeGpuRange> gpu_map_hook{}, gpu_unmap_hook{}, gpu_invalidate_hook{};

LONG WINAPI dispatch_gpu_fault(EXCEPTION_POINTERS* exception) noexcept {
    if (!exception || !exception->ExceptionRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address = static_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionInformation[1]);
    const auto handler = fault_handler.load(std::memory_order_acquire);
    return handler && handler(exception, reinterpret_cast<void*>(address))
               ? EXCEPTION_CONTINUE_EXECUTION
               : EXCEPTION_CONTINUE_SEARCH;
}

DWORD protection_for(DWORD original, bool read, bool write) {
    if (!read && !write) return PAGE_NOACCESS;
    if (!read && write) throw std::invalid_argument("Write-only GPU fault protection is invalid.");
    if (read && write) return original;
    const DWORD base = original & 0xff;
    const bool executable = base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
                            base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
    return executable ? PAGE_EXECUTE_READ : PAGE_READONLY;
}
}

ScopedGpuMemoryTracking::ScopedGpuMemoryTracking(GpuFaultHandler handler) {
    if (!handler) throw std::invalid_argument("GPU fault handler is null.");
    GpuFaultHandler expected{};
    if (!fault_handler.compare_exchange_strong(expected, handler, std::memory_order_acq_rel))
        throw std::runtime_error("GPU memory fault dispatcher is already active.");
    auto* cookie = AddVectoredExceptionHandler(1, &dispatch_gpu_fault);
    if (!cookie) {
        fault_handler.store(nullptr, std::memory_order_release);
        throw std::runtime_error("Cannot install GPU memory fault dispatcher.");
    }
    handler_cookie_ = cookie;
}

ScopedGpuMemoryTracking::~ScopedGpuMemoryTracking() {
    fault_handler.store(nullptr, std::memory_order_release);
    if (handler_cookie_) RemoveVectoredExceptionHandler(handler_cookie_);
    handler_cookie_ = nullptr;
    std::lock_guard lock(protection_mutex);
    for (const auto& [address, tracked] : original_protections) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
            info.State != MEM_COMMIT ||
            reinterpret_cast<std::uintptr_t>(info.AllocationBase) != tracked.allocation_base)
            continue;
        DWORD ignored{};
        VirtualProtect(reinterpret_cast<void*>(address), page_size, tracked.original, &ignored);
    }
    original_protections.clear();
}

bool gpu_protect(std::uintptr_t address, std::uint64_t size, bool read, bool write) noexcept {
    try {
        if (!address || !size || (!read && write) || size > UINTPTR_MAX - address) return false;
        const auto begin = address & ~(page_size - 1);
        const auto end_address = address + static_cast<std::uintptr_t>(size);
        if (end_address > UINTPTR_MAX - (page_size - 1)) return false;
        const auto end = (end_address + page_size - 1) & ~(page_size - 1);
        std::lock_guard lock(protection_mutex);
        struct Change { std::uintptr_t page; DWORD original; DWORD target; bool was_tracked; };
        std::vector<Change> changes;
        for (auto page = begin; page < end; page += page_size) {
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
                return false;
            auto tracked = original_protections.find(page);
            if (tracked != original_protections.end() &&
                tracked->second.allocation_base != reinterpret_cast<std::uintptr_t>(info.AllocationBase)) {
                original_protections.erase(tracked);
                tracked = original_protections.end();
            }
            if ((info.Protect & PAGE_NOACCESS) && tracked == original_protections.end()) return false;
            const DWORD original = tracked == original_protections.end() ? info.Protect : tracked->second.original;
            changes.push_back({page, original, protection_for(original, read, write),
                               tracked != original_protections.end()});
        }
        std::vector<std::uintptr_t> newly_tracked;
        if (!(read && write)) {
            try {
                newly_tracked.reserve(changes.size());
                for (const auto& change : changes) {
                    if (!change.was_tracked) {
                        MEMORY_BASIC_INFORMATION info{};
                        if (!VirtualQuery(reinterpret_cast<void*>(change.page), &info, sizeof(info)) ||
                            info.State != MEM_COMMIT)
                            throw std::runtime_error("GPU page changed during protection setup.");
                        original_protections.emplace(change.page,
                            TrackedPage{change.original, reinterpret_cast<std::uintptr_t>(info.AllocationBase)});
                        newly_tracked.push_back(change.page);
                    }
                }
            } catch (...) {
                for (const auto page : newly_tracked) original_protections.erase(page);
                return false;
            }
        }
        std::size_t applied{};
        for (; applied < changes.size(); ++applied) {
            const auto& change = changes[applied];
            DWORD ignored{};
            if (!VirtualProtect(reinterpret_cast<void*>(change.page), page_size, change.target, &ignored))
                break;
        }
        if (applied == changes.size()) {
            if (read && write)
                for (const auto& change : changes) original_protections.erase(change.page);
            return true;
        }
        while (applied) {
            const auto& change = changes[--applied];
            DWORD ignored{};
            VirtualProtect(reinterpret_cast<void*>(change.page), page_size, change.original, &ignored);
            if (!change.was_tracked) original_protections.erase(change.page);
        }
        for (const auto page : newly_tracked) original_protections.erase(page);
        return false;
    } catch (...) {
        return false;
    }
}

bool gpu_page_is_protected(std::uintptr_t address) noexcept {
    try {
        const auto page = address & ~(page_size - 1);
        std::lock_guard lock(protection_mutex);
        const auto tracked = original_protections.find(page);
        if (tracked == original_protections.end()) return false;
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(page), &info, sizeof(info)) ||
            info.State != MEM_COMMIT ||
            reinterpret_cast<std::uintptr_t>(info.AllocationBase) != tracked->second.allocation_base) {
            original_protections.erase(tracked);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

void gpu_notify_map(std::uintptr_t address, std::uint64_t size) noexcept {
    if (const auto hook = gpu_map_hook.load(std::memory_order_acquire)) hook(address, size);
}
void gpu_notify_unmap(std::uintptr_t address, std::uint64_t size) noexcept {
    if (const auto hook = gpu_unmap_hook.load(std::memory_order_acquire)) hook(address, size);
}
void gpu_notify_invalidate(std::uintptr_t address, std::uint64_t size) noexcept {
    if (const auto hook = gpu_invalidate_hook.load(std::memory_order_acquire)) hook(address, size);
}

bool gpu_copy_backing(std::uintptr_t guest_address, void* buffer, std::uint64_t size,
                      bool guest_to_host) noexcept {
    try {
        const auto buffer_address = reinterpret_cast<std::uintptr_t>(buffer);
        if (!guest_address || !buffer || !size || size > UINTPTR_MAX - guest_address ||
            size > UINTPTR_MAX - buffer_address)
            return false;
        std::array<std::byte, page_size> staging{};
        std::uint64_t offset{};
        while (offset < size) {
            const auto address = guest_address + static_cast<std::uintptr_t>(offset);
            const auto within = address & (page_size - 1);
            const auto length = static_cast<SIZE_T>(std::min<std::uint64_t>(page_size - within, size - offset));
            auto* host = reinterpret_cast<void*>(buffer_address + static_cast<std::uintptr_t>(offset));
            auto* guest = reinterpret_cast<void*>(address);
            SIZE_T copied{};
            if (!guest_to_host &&
                (!ReadProcessMemory(GetCurrentProcess(), host, staging.data(), length, &copied) || copied != length))
                return false;
            bool copied_ok{};
            {
                std::lock_guard lock(protection_mutex);
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(guest, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                    (info.Protect & PAGE_GUARD))
                    return false;
                const auto page = address & ~(page_size - 1);
                auto tracked = original_protections.find(page);
                if (tracked != original_protections.end() &&
                    tracked->second.allocation_base != reinterpret_cast<std::uintptr_t>(info.AllocationBase)) {
                    original_protections.erase(tracked);
                    tracked = original_protections.end();
                }
                const DWORD original = tracked == original_protections.end() ? info.Protect : tracked->second.original;
                const DWORD current = info.Protect;
                const DWORD base = original & 0xff;
                const bool readable = base != PAGE_NOACCESS && base != PAGE_EXECUTE;
                const bool writable = base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
                    base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
                if ((guest_to_host && !readable) || (!guest_to_host && !writable)) return false;
                bool exposed{};
                if (tracked != original_protections.end()) {
                    const bool executable = base == PAGE_EXECUTE || base == PAGE_EXECUTE_READ ||
                        base == PAGE_EXECUTE_READWRITE || base == PAGE_EXECUTE_WRITECOPY;
                    const DWORD temporary = guest_to_host ? original : (executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
                    DWORD ignored{};
                    if (current != temporary && !VirtualProtect(reinterpret_cast<void*>(page), page_size, temporary, &ignored))
                        return false;
                    exposed = current != temporary;
                }
                if (guest_to_host) {
                    copied_ok = ReadProcessMemory(GetCurrentProcess(), guest, staging.data(), length, &copied) && copied == length;
                } else {
                    copied_ok = WriteProcessMemory(GetCurrentProcess(), guest, staging.data(), length, &copied) && copied == length;
                }
                if (exposed) {
                    DWORD ignored{};
                    if (!VirtualProtect(reinterpret_cast<void*>(page), page_size, current, &ignored)) return false;
                }
            }
            if (!copied_ok) return false;
            if (guest_to_host &&
                (!WriteProcessMemory(GetCurrentProcess(), host, staging.data(), length, &copied) || copied != length))
                return false;
            offset += length;
        }
        return true;
    } catch (...) {
        return false;
    }
}
}

extern "C" void runtime_memory_gpu_protect(std::uintptr_t address, std::uint64_t size,
                                            int read, int write) {
    (void)lbp3::memory::gpu_protect(address, size, read != 0, write != 0);
}

extern "C" int runtime_memory_is_mapped(std::uintptr_t address, std::uint64_t size) {
    if (!address || !size || size > UINTPTR_MAX - address) return 0;
    const auto end = address + static_cast<std::uintptr_t>(size);
    for (auto cursor = address; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return 0;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= cursor) return 0;
        cursor = std::min(end, base + info.RegionSize);
    }
    return 1;
}

extern "C" std::uint64_t runtime_memory_clamp(std::uintptr_t address, std::uint64_t size) {
    if (!address || !size || size > UINTPTR_MAX - address) return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
        return 0;
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= address) return 0;
    return std::min<std::uint64_t>(size, base + info.RegionSize - address);
}

namespace {
template<class T> bool publish_output(T* destination, T value) noexcept {
    if (!destination) return false;
    SIZE_T copied{};
    return WriteProcessMemory(GetCurrentProcess(), destination, &value, sizeof(value), &copied) && copied == sizeof(value);
}
}

extern "C" int runtime_memory_region(std::uintptr_t address, std::uintptr_t* start,
                                      std::uintptr_t* end, int* mapped) {
    if (!address || !start || !end || !mapped) return 0;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) return 0;
    const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    if (info.RegionSize > UINTPTR_MAX - base) return 0;
    return publish_output(start, base) && publish_output(end, base + info.RegionSize) &&
        publish_output(mapped, info.State == MEM_COMMIT ? 1 : 0) ? 1 : 0;
}

extern "C" int runtime_memory_read_backing(std::uintptr_t address, void* output, std::uint64_t size) {
    return lbp3::memory::gpu_copy_backing(address, output, size, true) ? 1 : 0;
}

extern "C" int runtime_memory_write_backing(std::uintptr_t address, const void* input, std::uint64_t size) {
    return lbp3::memory::gpu_copy_backing(address, const_cast<void*>(input), size, false) ? 1 : 0;
}

extern "C" void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap,
                                               RuntimeGpuRange invalidate) {
    lbp3::memory::gpu_map_hook.store(map, std::memory_order_release);
    lbp3::memory::gpu_unmap_hook.store(unmap, std::memory_order_release);
    lbp3::memory::gpu_invalidate_hook.store(invalidate, std::memory_order_release);
    if (!map) return;
    auto cursor = lbp3::memory::guest_address_min;
    while (cursor < lbp3::memory::guest_address_limit) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info))) break;
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (info.RegionSize > UINTPTR_MAX - base || base + info.RegionSize <= cursor) break;
        const auto end = std::min<std::uintptr_t>(lbp3::memory::guest_address_limit, base + info.RegionSize);
        if (info.State == MEM_COMMIT) map(cursor, end - cursor);
        cursor = end;
    }
}
