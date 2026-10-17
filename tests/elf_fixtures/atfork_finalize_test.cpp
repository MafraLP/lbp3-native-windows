#include "lbp3_host/thread_services.hpp"
#include "lbp3_host/module_registry.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::abi;
unsigned invoked{};
void LBP3_GUEST_ABI callback_a() noexcept {++invoked;}
void LBP3_GUEST_ABI callback_b() noexcept {++invoked;}
void check(bool valid){if(!valid)throw std::runtime_error("atfork finalization assertion failed");}
int main(){try{
    check(register_atfork(nullptr,nullptr,nullptr)==22);
    ModuleInfoEx a{},b{};a.id=1;b.id=2;a.name[0]='a';b.name[0]='b';
    a.segment_count=b.segment_count=1;
    a.segments[0]={reinterpret_cast<std::uintptr_t>(&callback_a),1,5};
    b.segments[0]={reinterpret_cast<std::uintptr_t>(&callback_b),1,5};
    const ModuleInfoEx records[]{a,b};ScopedModuleRegistry modules(records);
    {
        ScopedThreadServices services;
        pthread_cxa_finalize(&a);check(atfork_handler_count()==0);
        check(register_atfork(&callback_a,nullptr,nullptr)==0);
        check(register_atfork(nullptr,&callback_b,nullptr)==0);
        check(register_atfork(nullptr,nullptr,&callback_a)==0);
        check(register_atfork(reinterpret_cast<ForkCallback>(1),nullptr,nullptr)==14);
        check(atfork_handler_count()==3);
        pthread_cxa_finalize(&a);check(atfork_handler_count()==1&&invoked==0);
        pthread_cxa_finalize(&a);check(atfork_handler_count()==1);
        pthread_cxa_finalize(&b);check(atfork_handler_count()==0&&invoked==0);
        // Finalization removes handlers; it never calls them or runs TLS destructors.
        for(unsigned i=0;i<256;++i)check(register_atfork(&callback_b,nullptr,nullptr)==0);
        check(register_atfork(&callback_b,nullptr,nullptr)==12);
        pthread_cxa_finalize(&b);check(atfork_handler_count()==0);
        std::thread worker([&]{check(register_atfork(nullptr,&callback_a,nullptr)==0);});worker.join();
        check(atfork_handler_count()==1);
    }
    {ScopedThreadServices clean;check(atfork_handler_count()==0);}
    std::cout<<"Atfork finalization tests passed: module selection, callback removal without execution, idempotence, capacity, concurrency and scope cleanup. Process fork is not implemented.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
