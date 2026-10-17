#include "lbp3_host/memory_services.hpp"
#include "lbp3_host/flexible_memory.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
void check(bool valid){if(!valid)throw std::runtime_error("memory query assertion failed");}
int main(){try{
    memory::QueryInfo info{};
    check(hle::virtual_query(reinterpret_cast<void*>(1),0,&info,sizeof(info))==err(13));
    memory::ScopedFlexibleMemory flexible(0x10000);memory::ScopedDirectMemory direct(0x100000);
    void* flex{};check(memory::map_named(&flex,0x4000,3,0,"flex-test")==0);
    std::int64_t physical{};check(memory::direct_allocate(0,0x100000,0x10000,0,3,&physical)==0);
    void* view{};check(memory::direct_map_named(&view,0x10000,0x33,0,physical,0,"direct-test")==0);
    check(hle::virtual_query(static_cast<char*>(view)+16,0,&info,sizeof(info))==0&&info.start==reinterpret_cast<std::uintptr_t>(view)&&info.end-info.start==0x10000&&info.offset==static_cast<std::uint64_t>(physical)&&info.protection==0x33&&info.type==3&&info.flags==0x12&&!std::strcmp(info.name.data(),"direct-test"));
    check(hle::virtual_query(flex,0,&info,sizeof(info))==0&&info.flags==0x11&&info.protection==3&&!std::strcmp(info.name.data(),"flex-test"));
    abi::ModuleInfoEx module{};module.id=1;module.name[0]='m';module.segment_count=1;module.segments[0]={0x10000,0x4000,5};
    const abi::ScopedModuleRegistry modules(std::span(&module,1));
    check(hle::virtual_query(reinterpret_cast<void*>(0x10100),0,&info,sizeof(info))==0&&info.start==0x10000&&info.end==0x14000&&info.protection==5&&info.flags==0x10&&info.name[0]=='m');
    check(hle::virtual_query(nullptr,1,&info,sizeof(info))==0&&info.start==0x10000);
    check(hle::virtual_query(nullptr,0,&info,sizeof(info))==err(13));
    bool child=false;std::thread worker([&]{memory::QueryInfo queried{};child=!hle::virtual_query(view,0,&queried,sizeof(queried))&&queried.type==3;});worker.join();check(child);
    check(hle::virtual_query(view,2,&info,sizeof(info))==err(22)&&hle::virtual_query(view,0,&info,71)==err(22));
    check(hle::virtual_query(view,0,reinterpret_cast<memory::QueryInfo*>(1),sizeof(info))==err(14));
    auto* readonly=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY);check(readonly);
    check(hle::virtual_query(view,0,static_cast<memory::QueryInfo*>(readonly),sizeof(info))==err(14));VirtualFree(readonly,0,MEM_RELEASE);
    check(memory::unmap(view,0x10000)==0&&hle::virtual_query(view,0,&info,sizeof(info))==err(13));
    check(memory::direct_release(physical,0x10000)==0&&memory::unmap(flex,0x4000)==0);
    check(hle::resolve_memory_query({"rVjRvHJ0X6c","libkernel","libkernel",1,0x0101,2}).has_value());
    check(!hle::resolve_memory_query({"rVjRvHJ0X6c","libkernel","libkernel",1,0x0101,1}));
    std::cout<<"Memory query tests passed: direct/flexible/module ranges, offset/type/GPU flags/names, next-range search, scope consistency, unmapping, cross-thread queries and protected pointers. Native host regions are not exposed.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
