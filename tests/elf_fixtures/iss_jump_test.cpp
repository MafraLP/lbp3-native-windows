#include <iostream>
#include <stdexcept>

extern "C" int tlg_jump_bridge_self_test();

int main() {
    if (tlg_jump_bridge_self_test() != 37) {
        std::cerr << "ISS Win64 jump bridge did not restore the recovery value.\n";
        return 1;
    }
    std::cout << "ISS Win64 jump bridge passed the synthetic recovery roundtrip.\n";
    return 0;
}
