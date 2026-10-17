#pragma once
#include <array>
#include <cstdint>
namespace lbp3::abi {
using SignalMask=std::array<std::uint32_t,4>;
static_assert(sizeof(SignalMask)==16);
SignalMask current_signal_mask() noexcept;
void inherit_signal_mask(const SignalMask&) noexcept;
// Internal errno values, not SCE codes. No signal delivery or host signal changes.
int fill_signal_mask(SignalMask*,bool full) noexcept;
int change_signal_mask(int,const SignalMask*,SignalMask*) noexcept;
}
