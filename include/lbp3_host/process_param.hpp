#pragma once
#include "lbp3_host/abi.hpp"
#include <span>
#include <cstddef>

namespace lbp3::abi {
// Configure on the control thread, before workers start; mapped bytes must outlive scope.
class ScopedProcessParam {
public:
    explicit ScopedProcessParam(std::span<const std::byte> bytes);
    ~ScopedProcessParam();
    ScopedProcessParam(const ScopedProcessParam&)=delete;
    ScopedProcessParam& operator=(const ScopedProcessParam&)=delete;
private:
    const void* previous_{};
};
const void* LBP3_GUEST_ABI get_proc_param() noexcept;
}
