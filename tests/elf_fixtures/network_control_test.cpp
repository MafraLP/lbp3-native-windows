#include "lbp3_host/network_control.hpp"
#include <exception>
#include <iostream>

int main(){
    try{lbp3::hle::network_control_self_test();std::cout<<"Network control tests passed: disconnected state, error results, NAT metadata, callback handles and offline network/HTTP contexts.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
