#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
#include <string_view>
#include <filesystem>

namespace lbp3::hle {
// Call once on the controlling host thread before resolving exports or guest calls.
void initialize();
std::optional<std::uintptr_t> resolve(const HostImport& request);
// Calls only our synthetic/host service functions, not game instructions.
void self_test(std::ostream& output);
void pthread_self_test(std::ostream& output);
void libc_services_self_test(std::ostream& output);
void heap_api_self_test(std::ostream& output);
void memory_self_test(std::ostream& output);
void libc_environment_self_test(std::ostream& output);
void thread_services_self_test(std::ostream& output);
void posix_memory_self_test(std::ostream& output);
void condition_self_test(std::ostream& output);
void signal_mask_self_test(std::ostream& output);
void module_registry_self_test(std::ostream& output);
void filesystem_self_test(const std::filesystem::path&,std::ostream& output);
// Destructive synthetic fixture: terminates this process with a diagnostic code.
void stack_failure_fixture();
void fatal_service_fixture(std::wstring_view mode);
}
