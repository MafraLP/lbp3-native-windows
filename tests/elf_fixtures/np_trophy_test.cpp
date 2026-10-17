#include "lbp3_host/np_trophy.hpp"
#include <exception>
#include <iostream>

int main(){
    try{lbp3::hle::np_trophy_self_test();std::cout<<"NP Trophy tests passed: offline context/handles and empty unlock state.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
