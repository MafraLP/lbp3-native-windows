#include "lbp3_host/pthread.hpp"
#include <Windows.h>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <string>
using namespace lbp3::abi;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
void check(bool valid,const std::source_location& location=std::source_location::current()){if(!valid)throw std::runtime_error("pthread affinity assertion failed at line "+std::to_string(location.line()));}
struct Worker {bool passed{};};
void* LBP3_GUEST_ABI run(void* argument) noexcept {
    auto& worker=*static_cast<Worker*>(argument);const auto self=sce_pthread_self();std::uint64_t mask{};
    GROUP_AFFINITY native{};
    worker.passed=!sce_pthread_set_affinity(self,2)&&!sce_pthread_get_affinity(self,&mask)&&mask==2&&
        GetThreadGroupAffinity(GetCurrentThread(),&native)&&std::popcount(native.Mask)==1;
    std::int32_t priority{};worker.passed=worker.passed&&!sce_pthread_set_priority(self,750)&&
        !sce_pthread_get_priority(self,&priority)&&priority==750&&GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_LOWEST;
    return argument;
}
int main(){try{
    GROUP_AFFINITY original{};check(GetThreadGroupAffinity(GetCurrentThread(),&original));
    const auto original_priority=GetThreadPriority(GetCurrentThread());
    const auto self=sce_pthread_self();check(sce_pthread_set_affinity(self,1)==err(22));
    {PthreadRuntime runtime({});ScopedPthreadRuntime scope(runtime);
        std::uint64_t mask{};check(sce_pthread_get_affinity(self,&mask)==0&&mask==0x7f);
        std::int32_t priority{};check(sce_pthread_get_priority(self,&priority)==0&&priority==700);
        for(const auto [guest,native]:{std::pair{300,THREAD_PRIORITY_HIGHEST},std::pair{450,THREAD_PRIORITY_ABOVE_NORMAL},std::pair{701,THREAD_PRIORITY_NORMAL},std::pair{720,THREAD_PRIORITY_BELOW_NORMAL},std::pair{760,THREAD_PRIORITY_LOWEST}}){
            check(sce_pthread_set_priority(self,guest)==0&&sce_pthread_get_priority(self,&priority)==0&&priority==guest&&GetThreadPriority(GetCurrentThread())==native);
        }
        check(sce_pthread_set_priority(self,255)==err(22)&&sce_pthread_set_priority(self,768)==err(22));
        check(sce_pthread_get_priority(self,reinterpret_cast<std::int32_t*>(1))==err(14)&&sce_pthread_set_priority(0,700)==err(3));
        check(sce_pthread_set_affinity(self,1)==0&&sce_pthread_get_affinity(self,&mask)==0&&mask==1);
        GROUP_AFFINITY observed{};check(GetThreadGroupAffinity(GetCurrentThread(),&observed)&&std::popcount(observed.Mask)==1);
        check(sce_pthread_set_affinity(self,0)==err(22)&&sce_pthread_set_affinity(self,0x100)==err(22));
        check(sce_pthread_get_affinity(self,reinterpret_cast<std::uint64_t*>(1))==err(14));
        check(sce_pthread_set_affinity(0,1)==err(3)&&sce_pthread_get_affinity(0,&mask)==err(3));
        Worker worker;GuestHandle handle{};void* result{};
        check(sce_pthread_create(&handle,nullptr,&run,&worker,"affinity")==0);
        check(sce_pthread_join(handle,&result)==0&&result==&worker&&worker.passed);
        check(sce_pthread_get_affinity(handle,&mask)==err(3));
    }
    GROUP_AFFINITY restored{};check(GetThreadGroupAffinity(GetCurrentThread(),&restored)&&restored.Mask==original.Mask&&restored.Group==original.Group);
    check(GetThreadPriority(GetCurrentThread())==original_priority);
    std::cout<<"Pthread affinity tests passed: real native masks, guest logical CPU mapping, controller/worker handles, stale IDs, invalid pointers and controller restoration. Up to eight guest CPUs map onto available host CPUs.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
