#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include "lbp3_host/low_memory.hpp"
#include <Windows.h>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::memory;
unsigned mapped_notifications{},unmapped_notifications{},invalidated_notifications{};
std::uintptr_t last_map{};
void on_gpu_map(std::uintptr_t address,std::uint64_t){++mapped_notifications;last_map=address;}
void on_gpu_unmap(std::uintptr_t,std::uint64_t){++unmapped_notifications;}
void on_gpu_invalidate(std::uintptr_t,std::uint64_t){++invalidated_notifications;}
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
void check(bool value){if(!value)throw std::runtime_error("direct memory assertion failed");}
int main(){try{
    check(!direct_size());
    void* orphan{};
    {
        ScopedDirectMemory scope(16*1024*1024);check(direct_size()==16*1024*1024);
        runtime_memory_set_gpu_hooks(&on_gpu_map,&on_gpu_unmap,&on_gpu_invalidate);
        check(mapped_notifications>0&&last_map>=guest_address_min&&last_map<guest_address_limit);
        bool nested=false;try{ScopedDirectMemory other;}catch(...){nested=true;}check(nested);
        std::int64_t a=-1,b=-1;const auto end=static_cast<std::int64_t>(direct_size());
        check(direct_allocate(0,end,0x20000,0x200000,0,&a)==0&&a==0);
        check(direct_allocate(0,end,0x10000,0x4000,3,&b)==0&&b==0x20000);
        std::int64_t extra=-77;
        check(direct_allocate(0,0x20000,0x4000,0,0,&extra)==err(28)&&extra==-77);
        check(direct_allocate(-1,end,0x4000,0,0,&extra)==err(22));
        check(direct_allocate(0,end,0x4001,0,0,&extra)==err(22));
        check(direct_allocate(0,end,0x4000,0x5000,0,&extra)==err(22));
        check(direct_allocate(0,end,0x4000,0,0,reinterpret_cast<std::int64_t*>(1))==err(14));
        void* first{};void* alias{};
        check(direct_map(&first,0x20000,3,0,a,0x200000)==0&&reinterpret_cast<std::uintptr_t>(first)%0x200000==0);
        check(mapped_notifications>1&&last_map==reinterpret_cast<std::uintptr_t>(first));
        gpu_notify_invalidate(reinterpret_cast<std::uintptr_t>(first),0x1000);
        check(invalidated_notifications==1);
        auto* bytes=static_cast<unsigned char*>(first);check(bytes[0]==0&&bytes[0x1ffff]==0);bytes[0]=17;bytes[0x1ffff]=33;
        check(reinterpret_cast<std::uintptr_t>(first)>=guest_address_min&&reinterpret_cast<std::uintptr_t>(first)+0x20000<=guest_address_limit);
        check(direct_map(&alias,0x20000,3,0,a,0)==0&&alias!=first);
        auto* second=static_cast<unsigned char*>(alias);check(second[0]==17&&second[0x1ffff]==33);
        check(reinterpret_cast<std::uintptr_t>(alias)+0x20000<=guest_address_limit);
        std::thread worker([&]{second[0x10000]=99;});worker.join();check(bytes[0x10000]==99);
        void* tail{};check(direct_map(&tail,0x10000,1,0,a+0x10000,0)==0&&static_cast<unsigned char*>(tail)[0]==99);
        void* denied{};check(direct_map(&denied,0x4000,3,0,a+0x4000,0)==err(45)&&!denied);
        void* shared{};check(direct_map_named(&shared,0x4000,0x33,0,a,0,"shared-cpu-gpu")==0);
        const auto view=direct_view(reinterpret_cast<std::uintptr_t>(shared));
        check(view&&view->protection==0x33&&view->physical==static_cast<std::uint64_t>(a)&&view->length==0x4000&&view->name[0]=='s');
        check(static_cast<unsigned char*>(shared)[0]==17);
        // A non-fixed hint is honored when available, and safely falls back
        // without replacing a pre-existing reservation when occupied.
        constexpr std::uintptr_t preferred=0x400000000ull;
        void* hinted=reinterpret_cast<void*>(preferred);
        check(direct_map_named(&hinted,0x10000,3,0,a,0x10000,"hint-free")==0);
        check(reinterpret_cast<std::uintptr_t>(hinted)==preferred);
        check(static_cast<unsigned char*>(hinted)[0]==17);
        check(direct_unmap(hinted,0x10000)==0);
        void* occupied=VirtualAlloc(reinterpret_cast<void*>(preferred),0x10000,MEM_RESERVE,PAGE_NOACCESS);
        if(occupied==reinterpret_cast<void*>(preferred)){
            void* fallback=occupied;
            check(direct_map_named(&fallback,0x10000,3,0,a,0x10000,"hint-occupied")==0);
            check(fallback!=occupied&&static_cast<unsigned char*>(fallback)[0]==17);
            check(direct_unmap(fallback,0x10000)==0);
            check(VirtualFree(occupied,0,MEM_RELEASE)!=0);
        }else if(occupied){VirtualFree(occupied,0,MEM_RELEASE);}
        check(direct_map_named(&denied,0x4000,3,0,a,0,nullptr)==err(14));
        check(direct_map_named(&denied,0x4000,3,0,a,0,reinterpret_cast<const char*>(1))==err(14));
        check(direct_map(&denied,0x40000,3,0,a,0)==err(22));
        check(direct_map(reinterpret_cast<void**>(1),0x4000,3,0,a,0)==err(14));
        check(direct_unmap(first,0x4000)==err(45)&&bytes[0]==17);
        check(direct_unmap(first,0x20000)==0&&second[0]==17);
        check(direct_release(a,0x4000)==err(45));
        check(direct_release(a,0x20000)==0&&direct_unmap(alias,0x20000)==err(22));
        MEMORY_BASIC_INFORMATION info{};check(VirtualQuery(alias,&info,sizeof(info))&&info.State==MEM_FREE);
        check(direct_release(a,0x20000)==err(22));
        check(direct_release(b,0x10000)==0);
        check(mapped_notifications>=4&&unmapped_notifications>=2);
        runtime_memory_set_gpu_hooks(nullptr,nullptr,nullptr);
        check(direct_allocate(0,end,0x10000,0,0,&a)==0&&a==0);
        check(direct_map(&orphan,0x10000,3,0,a,0)==0);
        for(const auto nid:{"pO96TwzOm5E","rTXw65xmLIA","L-Q3LEjIbgA","MBuItvba6z8","NcaWUxfMNIQ"}){
            lbp3::HostImport r{nid,"libkernel","libkernel",1,0x0101,2};check(resolve_direct_memory(r).has_value());r.type=1;check(!resolve_direct_memory(r));
        }
    }
    MEMORY_BASIC_INFORMATION info{};check(!direct_size()&&VirtualQuery(orphan,&info,sizeof(info))&&info.State==MEM_FREE);
    std::cout<<"Direct memory tests passed: aligned physical intervals, allocation failures, coherent aliases, advisory placement hints, subviews, guest GPU visibility metadata, release, pointer validation and scope cleanup. GPU command execution/partial release remain unsupported.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
