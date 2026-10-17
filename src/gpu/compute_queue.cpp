#include "lbp3_host/compute_queue.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <array>
#include <bit>
#include <mutex>
#include <stdexcept>
namespace lbp3::gpu {
struct ComputeState {std::array<std::optional<ComputeQueueInfo>,64> queues;};
namespace {
std::mutex registry;ComputeState* active{};
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80d17000u|e);}
bool initialize_read_pointer(std::uint32_t* pointer){
    if(!pointer||reinterpret_cast<std::uintptr_t>(pointer)%4)return false;
    MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(pointer,&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
    const auto p=info.Protect&255;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
    const std::uint32_t zero{};SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),pointer,&zero,4,&n)&&n==4;
}
}
ScopedComputeQueues::ScopedComputeQueues():state_(std::make_unique<ComputeState>()){
    std::lock_guard lock(registry);if(active)throw std::runtime_error("Compute queue scope already active.");active=state_.get();
}
ScopedComputeQueues::~ScopedComputeQueues(){std::lock_guard lock(registry);active=nullptr;}
std::int32_t LBP3_GUEST_ABI map_compute_queue(std::uint32_t pipe,std::uint32_t queue,std::uintptr_t ring,std::uint32_t dwords,std::uint32_t* read_pointer) noexcept {
    try{std::lock_guard lock(registry);if(!active)return error(0xff);
        if(pipe>=7)return error(0);if(queue>=8)return error(1);
        if(!ring||ring%256)return error(3);
        if(!std::has_single_bit(dwords)||dwords>4*1024*1024)return error(2);
        const auto view=memory::direct_view(ring);
        if(!view||!(view->protection&0x10)||static_cast<std::uint64_t>(dwords)*4>view->length-(ring-view->address))return error(3);
        for(const auto& slot:active->queues)if(slot&&slot->pipe==pipe&&slot->queue==queue)return error(0xff);
        for(std::size_t i=0;i<active->queues.size();++i)if(!active->queues[i]){
            if(!initialize_read_pointer(read_pointer))return error(4);
            active->queues[i]=ComputeQueueInfo{pipe,queue,ring,dwords,read_pointer};return static_cast<std::int32_t>(i+1);
        }
        return error(0xff);
    }catch(...){return error(0xff);}
}
std::int32_t LBP3_GUEST_ABI unmap_compute_queue(std::uint32_t id) noexcept {
    try{std::lock_guard lock(registry);if(!active||!id||id>active->queues.size()||!active->queues[id-1])return error(1);
        active->queues[id-1].reset();return 0;
    }catch(...){return error(0xff);}
}
std::optional<ComputeQueueInfo> compute_queue(std::uint32_t id){
    std::lock_guard lock(registry);if(!active||!id||id>active->queues.size())return std::nullopt;return active->queues[id-1];
}
std::int32_t LBP3_GUEST_ABI gnm_add_event(std::int64_t queue,std::uint64_t event,void* userdata) noexcept {
    if(event>6&&event!=0x40)return std::bit_cast<std::int32_t>(0x80020016u);
    return abi::event_subscribe(queue,{event,-14,0x20,0,0,userdata});
}
std::int32_t LBP3_GUEST_ABI gnm_delete_event(std::int64_t queue,std::uint64_t event) noexcept {
    if(event>6&&event!=0x40)return std::bit_cast<std::int32_t>(0x80020016u);
    return abi::event_remove(queue,event,-14);
}
std::optional<std::uintptr_t> resolve_compute_queue(const HostImport& r){
    if(r.type!=2||r.library!="libSceGnmDriver"||r.module!="libSceGnmDriver"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="29oKvKXzEZo")return reinterpret_cast<std::uintptr_t>(&map_compute_queue);
    if(r.nid=="ArSg-TGinhk")return reinterpret_cast<std::uintptr_t>(&unmap_compute_queue);
    if(r.nid=="b0xyllnVY-I")return reinterpret_cast<std::uintptr_t>(&gnm_add_event);
    if(r.nid=="PVT+fuoS9gU")return reinterpret_cast<std::uintptr_t>(&gnm_delete_event);
    return std::nullopt;
}
}
