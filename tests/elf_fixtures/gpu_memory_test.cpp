#include "lbp3_host/gpu_memory.hpp"
#include "lbp3_host/low_memory.hpp"
#include <atomic>
#include <iostream>
#include <source_location>
#include <string>
#include <stdexcept>

namespace {
std::atomic<unsigned> handled{};
bool resume_fault(EXCEPTION_POINTERS* exception, void* address) noexcept {
    if (!exception || !exception->ExceptionRecord ||
        exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->NumberParameters < 2 ||
        exception->ExceptionRecord->ExceptionInformation[1] != reinterpret_cast<std::uintptr_t>(address))
        return false;
    if (!lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(address) & ~std::uintptr_t{0xfff},
                                   0x1000, true, true))
        return false;
    handled.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void check(bool value, std::source_location where = std::source_location::current()) {
    if (!value) throw std::runtime_error("GPU page protection assertion failed at line " +
                                         std::to_string(where.line()) + ".");
}
}

int main() {
    auto* page = static_cast<volatile unsigned char*>(lbp3::memory::allocate_low(
        0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(page != nullptr);
    try {
        page[0] = 0x31;
        page[0x1000] = 0x42;
        check(runtime_memory_is_mapped(reinterpret_cast<std::uintptr_t>(page), 0x2000));
        check(!runtime_memory_is_mapped(reinterpret_cast<std::uintptr_t>(page), 0x3000));
        check(runtime_memory_clamp(reinterpret_cast<std::uintptr_t>(page) + 0x1000, 0x2000) == 0x1000);
        std::uintptr_t region_start{}, region_end{};int mapped{};
        check(runtime_memory_region(reinterpret_cast<std::uintptr_t>(page), &region_start, &region_end, &mapped) &&
              mapped == 1 && region_start <= reinterpret_cast<std::uintptr_t>(page) &&
              region_end >= reinterpret_cast<std::uintptr_t>(page) + 0x2000);
        check(!runtime_memory_region(reinterpret_cast<std::uintptr_t>(page), nullptr, &region_end, &mapped));
        check(!runtime_memory_region(reinterpret_cast<std::uintptr_t>(page), &region_start, &region_end, nullptr));
        check(runtime_memory_read_backing(reinterpret_cast<std::uintptr_t>(page),
                                          const_cast<unsigned char*>(page), 1));
        check(!runtime_memory_read_backing(reinterpret_cast<std::uintptr_t>(page), nullptr, 1));
        check(!runtime_memory_write_backing(reinterpret_cast<std::uintptr_t>(page), nullptr, 1));
        check(!runtime_memory_is_mapped(reinterpret_cast<std::uintptr_t>(page), 0));
        check(runtime_memory_clamp(reinterpret_cast<std::uintptr_t>(page), 0) == 0);
        {
            lbp3::memory::ScopedGpuMemoryTracking tracking(&resume_fault);
            bool duplicate_rejected{};
            try { lbp3::memory::ScopedGpuMemoryTracking duplicate(&resume_fault); }
            catch (const std::runtime_error&) { duplicate_rejected = true; }
            check(duplicate_rejected);
            check(!lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(page), 0x1000,
                                            false, true));
            check(lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(page), 0x1000,
                                           true, false));
            check(lbp3::memory::gpu_page_is_protected(reinterpret_cast<std::uintptr_t>(page)));
            page[0] = 0x53; // VEH restores write access and retries this instruction.
            check(page[0] == 0x53 && handled.load() == 1);
            check(!lbp3::memory::gpu_page_is_protected(reinterpret_cast<std::uintptr_t>(page)));
            check(lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(page), 0x1000, false, false));
            unsigned char backing{};
            check(runtime_memory_read_backing(reinterpret_cast<std::uintptr_t>(page), &backing, 1) && backing == 0x53);
            const unsigned char update=0x61;
            check(runtime_memory_write_backing(reinterpret_cast<std::uintptr_t>(page), &update, 1));
            check(lbp3::memory::gpu_page_is_protected(reinterpret_cast<std::uintptr_t>(page)));
            const auto value = page[0]; // Read fault follows the same recovery path.
            check(value == update && handled.load() == 2);
            check(lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(page), 0x2000,
                                           true, false));
            check(lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(page), 0x2000,
                                           true, true));
            check(page[0] == update && page[0x1000] == 0x42);
        }
        MEMORY_BASIC_INFORMATION info{};
        check(VirtualQuery(const_cast<unsigned char*>(page), &info, sizeof(info)) &&
              info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE);
        VirtualFree(const_cast<unsigned char*>(page), 0, MEM_RELEASE);
        std::cout << "GPU memory test passed: write/read protection faults resumed, permissions restored, "
                     "invalid write-only requests rejected.\n";
        return 0;
    } catch (const std::exception& error) {
        VirtualFree(const_cast<unsigned char*>(page), 0, MEM_RELEASE);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
