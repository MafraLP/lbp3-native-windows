#pragma once
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string_view>
#include "lbp3_host/host_resolver.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include <cstdint>

namespace lbp3::loader {
// Owns mapped images beyond a single diagnostic call. It does not relocate or
// execute them; this is the lifetime foundation for a future native runtime.
class MappedModuleSession {
public:
    MappedModuleSession(const std::filesystem::path& directory, std::ostream& output);
    ~MappedModuleSession();
    MappedModuleSession(const MappedModuleSession&)=delete;
    MappedModuleSession& operator=(const MappedModuleSession&)=delete;
    std::uintptr_t entry(std::string_view module) const noexcept;
    std::size_t module_count() const noexcept;
    // Applies the same diagnostic partial-link policy as --link-set and keeps
    // TLS, process/module registries and host service contexts alive with images.
    void link(std::ostream& output, HostResolver resolver = nullptr,
              const std::filesystem::path& pending_report = {});
    void enable_gpu_fault_dispatch(memory::GpuFaultHandler handler);
    void mount_guest_filesystem(const std::filesystem::path& app_root,
                                const std::filesystem::path& savedata_root,
                                const std::filesystem::path& temp_root);
    // Initializes the optional ISS GPU backend before imports are linked. Once
    // active, this session's images and services stay resident until process exit.
    void initialize_iss_gpu_backend(const char* title, const char* serial,
                                    const char* user_directory, std::uint32_t sdk_version,
                                    std::uint32_t psf_attributes, std::int32_t width,
                                    std::int32_t height);
private:
    struct State;
    std::unique_ptr<State> state_;
};
// Maps and verifies an image, prints its layout, then releases all memory.
// Does not apply relocations or call guest code.
void check_mapping(const std::filesystem::path& path, std::ostream& output);
// Keep all prepared ELF images resident together until verification completes.
void check_module_set(const std::filesystem::path& directory, std::ostream& output);
// Diagnostic partial link: unresolved host/TLS relocations remain untouched.
void check_link_set(const std::filesystem::path& directory, std::ostream& output,
                    const std::filesystem::path& pending_report = {}, HostResolver resolver = nullptr);
// Generates and executes fixed synthetic fixtures only; no arbitrary ELF execution.
void execution_self_test(const std::filesystem::path& artifacts, std::ostream& output, HostResolver resolver);
// Only the eight SHA-256 pinned LBP3 images; libc DT_INIT only, never eboot.
void probe_lbp3_libc(const std::filesystem::path&,std::ostream&,HostResolver,bool dependencies=false,bool constructors=false,bool entry=false);
}
