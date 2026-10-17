#include "lbp3_host/abi.hpp"
#include <cstdint>
#include <ostream>
#include <stdexcept>

namespace lbp3::abi {
namespace {
using U64 = std::uint64_t;
using Callback = U64 (LBP3_GUEST_ABI *)(U64, U64);
// Deliberately noinline: the test must cross actual calling-convention boundaries.
__declspec(noinline) U64 host_eight(U64 a, U64 b, U64 c, U64 d, U64 e, U64 f, U64 g, U64 h) {
    return a + 3*b + 5*c + 7*d + 11*e + 13*f + 17*g + 19*h;
}
__declspec(noinline) U64 LBP3_GUEST_ABI guest_eight(U64 a, U64 b, U64 c, U64 d, U64 e, U64 f, U64 g, U64 h) {
    return host_eight(a,b,c,d,e,f,g,h);
}
__declspec(noinline) double host_float(double a, double b, double c, double d, double e, double f,
                                      double g, double h, double i) {
    return a + 2*b + 3*c + 4*d + 5*e + 6*f + 7*g + 8*h + 9*i;
}
__declspec(noinline) double LBP3_GUEST_ABI guest_float(double a, double b, double c, double d, double e, double f,
                                                     double g, double h, double i) {
    return host_float(a,b,c,d,e,f,g,h,i);
}
__declspec(noinline) double host_mixed(U64 a, double b, U64 c, double d, U64 e, double f) {
    return static_cast<double>(a+3*c+5*e) + 2*b+4*d+6*f;
}
__declspec(noinline) double LBP3_GUEST_ABI guest_mixed(U64 a, double b, U64 c, double d, U64 e, double f) {
    return host_mixed(a,b,c,d,e,f);
}
__declspec(noinline) U64 LBP3_GUEST_ABI callback(U64 a, U64 b) { return 7*a+11*b; }
__declspec(noinline) U64 host_callback(Callback fn, U64 a, U64 b) { return fn(a,b)+1; }
__declspec(noinline) U64 LBP3_GUEST_ABI guest_callback(Callback fn, U64 a, U64 b) {
    return host_callback(fn,a,b);
}
}

void self_test(std::ostream& output) {
    // Volatile function pointers prevent devirtualization and ABI-eliding inlining.
    auto volatile integers = &guest_eight;
    auto volatile floating = &guest_float;
    auto volatile mixed = &guest_mixed;
    auto volatile callbacks = &guest_callback;
    for (U64 i = 0; i < 1000; ++i) {
        if (integers(1+i,2,3,4,5,6,7,8) != 454+i)
            throw std::runtime_error("ABI integer/stack argument test failed.");
        if (floating(1,2,3,4,5,6,7,8,9) != 285)
            throw std::runtime_error("ABI floating-point/stack argument test failed.");
        if (mixed(1,2.5,3,4.5,5,6.5) != 97)
            throw std::runtime_error("ABI mixed argument test failed.");
        if (callbacks(&callback, 13+i,17) != 279+7*i)
            throw std::runtime_error("ABI callback roundtrip failed.");
    }
    output << "ABI self-test passed: Win64 -> SysV -> Win64 and callback roundtrip.\n"
              "Integer, floating-point, mixed and stack arguments verified (1000 rounds).\n"
              "Synthetic fixtures only. No game code was executed.\n";
}
}
