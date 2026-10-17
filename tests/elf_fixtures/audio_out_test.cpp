#include "lbp3_host/audio_out.hpp"
#include <exception>
#include <iostream>

int main(){
    try{lbp3::hle::audio_out_self_test();std::cout<<"AudioOut tests passed: offline initialization, validated silent port, real-time pacing, close and error handles.\n";return 0;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
