#include "lbp3_host/system_service.hpp"
#include <exception>
#include <iostream>

int main(){
    try{lbp3::hle::system_service_self_test();std::cout<<"System service tests passed: status, safe-area, no-event, splash and parameter queries.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
