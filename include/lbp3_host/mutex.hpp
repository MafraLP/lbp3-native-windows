#pragma once
#include "lbp3_host/host_resolver.hpp"
#include <iosfwd>
#include <memory>
#include <cstdint>

namespace lbp3::abi {
// Caller has already checked libkernel identity and version. Exact typed NIDs only.
std::optional<std::uintptr_t> resolve_mutex(const HostImport& request);
void mutex_self_test(std::ostream& output);
// Internal condition-wait lease; never exposed in guest memory.
struct MutexWaitLease {std::shared_ptr<void> object;unsigned depth{};};
std::int32_t mutex_release_for_wait(std::uint64_t*,MutexWaitLease&) noexcept;
std::int32_t mutex_resume_after_wait(MutexWaitLease&) noexcept;
}
