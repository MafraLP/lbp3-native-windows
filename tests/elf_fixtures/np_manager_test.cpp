#include "lbp3_host/np_manager.hpp"
#include <exception>
#include <iostream>

int main(){
    try{lbp3::hle::np_manager_self_test();std::cout<<"NP manager tests passed: offline callback poll and strict NID identity.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
