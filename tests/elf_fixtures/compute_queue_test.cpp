#include "lbp3_host/compute_queue.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3;
constexpr std::int32_t err(unsigned e){return std::bit_cast<std::int32_t>(0x80d17000u|e);}
void check(bool value){if(!value)throw std::runtime_error("compute queue assertion failed");}
int main(){try{
    memory::ScopedDirectMemory memory(0x100000);std::int64_t physical{};void* ring{};
    check(memory::direct_allocate(0,0x100000,0x10000,0,0,&physical)==0&&memory::direct_map(&ring,0x10000,0x33,0,physical,0)==0);
    const auto address=reinterpret_cast<std::uintptr_t>(ring);std::uint32_t read=77;
    check(gpu::map_compute_queue(0,0,address,1024,&read)==err(0xff));
    {gpu::ScopedComputeQueues scope;
        abi::ScopedEventQueues events;abi::EventQueueHandle eq{};check(abi::event_queue_create(&eq,"gpu-events")==0);
        check(gpu::gnm_add_event(eq,0x40,&read)==0&&gpu::gnm_add_event(eq,7,nullptr)==std::bit_cast<std::int32_t>(0x80020016u));
        check(abi::event_signal(eq,0x40,-14,88,nullptr)==0);
        abi::KernelEvent event{};int received{};const std::uint32_t poll{};
        check(abi::event_queue_wait(eq,&event,1,&received,&poll)==0&&received==1&&event.filter==-14&&event.data==88&&event.userdata==&read);
        check(gpu::gnm_delete_event(eq,0x40)==0&&gpu::gnm_delete_event(eq,0x40)!=0);
        const auto id=gpu::map_compute_queue(0,0,address,1024,&read);check(id>0&&read==0);
        const auto queue=gpu::compute_queue(id);check(queue&&queue->ring==address&&queue->dwords==1024&&queue->read_pointer==&read);
        check(gpu::map_compute_queue(0,0,address,1024,&read)==err(0xff));
        check(gpu::map_compute_queue(7,0,address,1024,&read)==err(0));
        check(gpu::map_compute_queue(0,8,address,1024,&read)==err(1));
        check(gpu::map_compute_queue(1,0,address+1,1024,&read)==err(3));
        check(gpu::map_compute_queue(1,0,address,3,&read)==err(2));
        check(gpu::map_compute_queue(1,0,address,32768,&read)==err(3));
        check(gpu::map_compute_queue(1,0,address,1024,reinterpret_cast<std::uint32_t*>(4))==err(4));
        auto* readonly=static_cast<std::uint32_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));check(readonly);
        check(gpu::map_compute_queue(1,0,address,1024,readonly)==err(4));VirtualFree(readonly,0,MEM_RELEASE);
        bool inspected=false;std::thread worker([&]{inspected=gpu::compute_queue(id).has_value();});worker.join();check(inspected);
        check(gpu::unmap_compute_queue(id)==0&&!gpu::compute_queue(id)&&gpu::unmap_compute_queue(id)==err(1));
        check(gpu::map_compute_queue(1,0,address,1024,&read)>0);
    }
    check(!gpu::compute_queue(1));
    check(gpu::resolve_compute_queue({"29oKvKXzEZo","libSceGnmDriver","libSceGnmDriver",1,0x0101,2}).has_value());
    check(!gpu::resolve_compute_queue({"29oKvKXzEZo","libSceGnmDriver","libSceGnmDriver",1,0,2}));
    std::cout<<"Compute queue registration tests passed: GPU-visible rings, IDs, sizes, read-pointer validation, scoped cleanup and resolver identity. No PM4/shader execution is implemented by registration.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
