#pragma once
#include "lbp3_host/abi.hpp"
#include <array>
#include <cstddef>
#include <optional>
#include <iosfwd>

namespace lbp3::abi {
// Address slots, not callable native function pointers. Only malloc/free are
// consumed by the synthetic test; other signatures remain unvalidated here.
using HeapApiTable=std::array<std::uintptr_t,10>;
class ScopedHeapApi {
public:
    // Control thread only: construct before workers, destroy after joining them
    // and before unmapping callback code. Concurrent/nested contexts rejected.
    ScopedHeapApi();
    ~ScopedHeapApi();
    ScopedHeapApi(const ScopedHeapApi&)=delete;
    ScopedHeapApi& operator=(const ScopedHeapApi&)=delete;
private:
    friend std::int32_t LBP3_GUEST_ABI set_application_heap_api(const std::uintptr_t*) noexcept;
    friend std::optional<HeapApiTable> heap_api_snapshot();
    std::optional<HeapApiTable> installed_;
};
std::int32_t LBP3_GUEST_ABI set_application_heap_api(const std::uintptr_t* table) noexcept;
// A copy for scoped consumers, never retain/use callbacks after the scope ends.
std::optional<HeapApiTable> heap_api_snapshot();
void heap_api_self_test(std::ostream& output,std::uintptr_t setter);
}
