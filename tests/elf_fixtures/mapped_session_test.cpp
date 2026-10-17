#include "lbp3_host/elf_mapping.hpp"
#include "lbp3_host/synthetic_elf.hpp"
#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
bool recover_session_fault(EXCEPTION_POINTERS*, void* address) noexcept {
    return lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(address) & ~std::uintptr_t{0xfff},
                                     0x1000, true, true);
}
}

int main() {
    const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("lbp3-mapped-session-" + std::to_string(id));
    try {
        std::filesystem::create_directories(directory);
        const auto image = lbp3::loader::synthetic_elf(false);
        {
            std::ofstream file(directory / "synthetic.elf", std::ios::binary);
            file.write(reinterpret_cast<const char*>(image.data()),
                       static_cast<std::streamsize>(image.size()));
            if (!file) throw std::runtime_error("Could not write synthetic ELF fixture.");
        }
        std::uintptr_t entry{};
        auto* gpu_page = static_cast<volatile unsigned char*>(VirtualAlloc(
            nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!gpu_page) throw std::runtime_error("Could not allocate GPU fault fixture page.");
        {
            lbp3::loader::MappedModuleSession session(directory, std::cout);
            if (session.module_count() != 1 || !(entry = session.entry("synthetic.elf")))
                throw std::runtime_error("Resident session did not expose its mapped entry.");
            session.link(std::cout);
            lbp3::abi::ModuleInfoEx module{};
            if (lbp3::abi::module_from_address(entry, 2, &module) || module.id != 1)
                throw std::runtime_error("Module registry did not remain active with the session.");
            session.enable_gpu_fault_dispatch(&recover_session_fault);
            bool duplicate_rejected{};
            try { session.enable_gpu_fault_dispatch(&recover_session_fault); }
            catch (const std::runtime_error&) { duplicate_rejected = true; }
            if (!duplicate_rejected ||
                !lbp3::memory::gpu_protect(reinterpret_cast<std::uintptr_t>(gpu_page), 0x1000,
                                           true, false))
                throw std::runtime_error("Session GPU fault dispatcher setup failed.");
            gpu_page[0] = 0x6a;
            if (gpu_page[0] != 0x6a)
                throw std::runtime_error("Session GPU fault dispatcher did not resume guest-page access.");
            MEMORY_BASIC_INFORMATION info{};
            if (!VirtualQuery(reinterpret_cast<void*>(entry), &info, sizeof(info)) ||
                info.State != MEM_COMMIT)
                throw std::runtime_error("Mapped entry is not resident during its session.");
        }
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(entry), &info, sizeof(info)) ||
            info.State != MEM_FREE)
            throw std::runtime_error("Mapped image survived session destruction.");
        lbp3::abi::ModuleInfoEx module{};
        if (lbp3::abi::module_from_address(entry, 2, &module) == 0)
            throw std::runtime_error("Module registry survived session destruction.");
        MEMORY_BASIC_INFORMATION gpu_info{};
        if (!VirtualQuery(const_cast<unsigned char*>(gpu_page), &gpu_info, sizeof(gpu_info)) ||
            gpu_info.Protect != PAGE_READWRITE)
            throw std::runtime_error("Session teardown did not restore GPU page permissions.");
        VirtualFree(const_cast<unsigned char*>(gpu_page), 0, MEM_RELEASE);
        std::filesystem::remove_all(directory);
        std::cout << "Mapped session lifetime test passed: entry remained resident until teardown; "
                     "no relocation or guest code execution.\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
