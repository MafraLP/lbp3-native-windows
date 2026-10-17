#include "lbp3_host/elf_dynamic.hpp"
#include <limits>
#include <iostream>
#include <stdexcept>

int main() {
    using lbp3::loader::add_signed;
    const auto max = std::numeric_limits<std::uint64_t>::max();
    const auto min = std::numeric_limits<std::int64_t>::min();
    if (add_signed(100, -8) != 92 || add_signed(100, 8) != 108 ||
        add_signed(max, 0) != max || add_signed(1ull << 63, min) != 0) return 1;
    for (const auto pair : {std::pair{max, std::int64_t{1}}, std::pair{0ull, std::int64_t{-1}},
                            std::pair{0ull, min}}) {
        bool caught = false;
        try { (void)add_signed(pair.first, pair.second); }
        catch (const std::runtime_error&) { caught = true; }
        if (!caught) return 1;
    }
    std::cout << "Checked positive, negative, INT64_MIN and overflow relocation arithmetic.\n";
}
