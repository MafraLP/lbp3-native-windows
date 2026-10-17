#pragma once
#include <iosfwd>

#if !defined(__clang__) || !defined(_M_X64)
#error "The verified guest ABI bridge requires clang-cl x64."
#endif
#define LBP3_GUEST_ABI __attribute__((sysv_abi))

namespace lbp3::abi {
// Executes synthetic compiler-generated ABI fixtures only, never game code.
void self_test(std::ostream& output);
}
