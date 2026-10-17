#include "lbp3_host/mutex.hpp"
#include <iostream>

int main() {
    lbp3::abi::mutex_self_test(std::cout);
    std::cout << "RW-lock fixture passed through init, shared/exclusive ownership, try-lock, errors and destroy.\n";
}
