#include "lbp3_host/gnm_commands.hpp"
#include "lbp3_host/video_out.hpp"
#include "shadps4/gnm_initial_state.hpp"
#include <Windows.h>
#include <array>
#include <algorithm>
#include <vector>
namespace lbp3::gpu {
namespace {
template<std::size_t N> bool publish(std::uint32_t* output,const std::array<std::uint32_t,N>& data){
    auto at=reinterpret_cast<std::uintptr_t>(output);if(!at||at%4||at>UINTPTR_MAX-sizeof(data))return false;
    const auto end=at+sizeof(data);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),output,data.data(),sizeof(data),&n)&&n==sizeof(data);
}
// Without the resident ISS backend there is no GPU queue to drain. Keep the
// import callable but report the same explicit not-initialized error used by
// command submission; never claim completion or frame presentation.
std::int32_t LBP3_GUEST_ABI submit_done_unavailable() noexcept {
    return static_cast<std::int32_t>(0x80d11000u);
}
std::int32_t LBP3_GUEST_ABI driver_capture_in_progress() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI user_pa_enabled() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI debug_hardware_status() noexcept {return 0;}
constexpr std::uint32_t packet(unsigned opcode,unsigned body,bool compute=false){return 0xc0000000u|((body-1)<<16)|(opcode<<8)|(compute?2u:0u);}
}
std::uint32_t LBP3_GUEST_ABI dispatch_initial_state(std::uint32_t* output,std::uint32_t capacity) noexcept {
    if(capacity<256)return 0;
    std::array<std::uint32_t,256> commands{};std::size_t at{};
    const auto set_register=[&](std::uint32_t index,std::uint32_t value){commands[at++]=packet(0x76,2,true);commands[at++]=index;commands[at++]=value;};
    // Base PS4, not Neo: enable shader engines 0/1 and set compute resource limits.
    set_register(0x216,0xffffffffu);set_register(0x217,0xffffffffu);set_register(0x215,0x170);
    commands[at++]=packet(0x58,6); // ACQUIRE_MEM: synchronize compute caches.
    commands[at++]=0x28000000u;at+=4;commands[at++]=0xa;
    commands[at]=packet(0x10,static_cast<unsigned>(commands.size()-at-1));
    return publish(output,commands)?256:0;
}
std::uint32_t LBP3_GUEST_ABI draw_initial_state(std::uint32_t* output,std::uint32_t capacity) noexcept {
    if(capacity<256)return 0;
    std::array<std::uint32_t,256> commands{};
    auto cursor=std::copy(reference::ClearStateSequence.begin(),reference::ClearStateSequence.end(),commands.begin());
    cursor=std::copy(reference::InitSequence.begin()+2,reference::InitSequence.end(),cursor);
    const auto remaining=static_cast<unsigned>(commands.end()-cursor-1);
    *cursor=packet(0x10,remaining);
    return publish(output,commands)?256:0;
}
std::int32_t LBP3_GUEST_ABI insert_wait_flip(std::uint32_t* output,std::uint32_t size,std::int32_t handle,std::uint32_t index) noexcept {
    if(size!=7||index>=16)return -1;
    std::uintptr_t labels{};if(video_buffer_labels(handle,&labels)!=16)return -1;
    const auto address=labels+index*8;
    const std::array<std::uint32_t,7> commands{packet(0x3c,6),0x13,static_cast<std::uint32_t>(address),static_cast<std::uint32_t>(address>>32),0,0xffffffffu,10};
    return publish(output,commands)?0:-1;
}
std::int32_t LBP3_GUEST_ABI set_compute_shader(std::uint32_t* output,std::uint32_t size,const std::uint32_t* registers) noexcept {
    if(size<25||!registers)return -1;
    std::array<std::uint32_t,7> values{};SIZE_T n{};
    if(!ReadProcessMemory(GetCurrentProcess(),registers,values.data(),sizeof(values),&n)||n!=sizeof(values)||values[1])return -1;
    std::array<std::uint32_t,25> commands{};
    commands[0]=packet(0x76,3);commands[1]=0x20c;commands[2]=values[0];commands[3]=0;
    commands[4]=packet(0x76,3);commands[5]=0x212;commands[6]=values[2];commands[7]=values[3];
    commands[8]=packet(0x76,4);commands[9]=0x207;commands[10]=values[4];commands[11]=values[5];commands[12]=values[6];
    commands[13]=packet(0x10,11);
    return publish(output,commands)?0:-1;
}
std::int32_t LBP3_GUEST_ABI pop_marker(std::uint32_t* output,std::uint32_t size) noexcept {
    if(size!=6)return -1;
    const std::array<std::uint32_t,6> commands{packet(0x10,5),0x68750002,0,0,0,0};
    return publish(output,commands)?0:-1;
}
std::int32_t LBP3_GUEST_ABI push_marker(std::uint32_t* output,std::uint32_t size,const char* marker) noexcept {
    if(!output||!marker||!size)return -1;
    try{
        constexpr std::size_t max_marker=4096;
        std::array<char,max_marker+1> text{};std::size_t length{};
        for(;length<max_marker;++length){SIZE_T read{};
            if(!ReadProcessMemory(GetCurrentProcess(),marker+length,&text[length],1,&read)||read!=1)return -1;
            if(!text[length])break;
        }
        if(length==max_marker)return -1;
        const auto packet_size=((length+8)>>2)+((length+12)>>3)*2;
        if(packet_size+2!=size)return -1;
        std::vector<std::uint32_t> commands(size);
        commands[0]=packet(0x10,static_cast<unsigned>(packet_size));
        commands[1]=0x68750001u;
        std::memcpy(reinterpret_cast<std::byte*>(commands.data()+2),text.data(),length+1);
        auto at=reinterpret_cast<std::uintptr_t>(output);
        const auto bytes=static_cast<std::size_t>(size)*sizeof(std::uint32_t);
        if(at%alignof(std::uint32_t)||bytes>UINTPTR_MAX-at)return -1;
        const auto end=at+bytes;
        while(at<end){MEMORY_BASIC_INFORMATION info{};
            if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||
               info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return -1;
            const auto protection=info.Protect&255;
            if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
               protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return -1;
            const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return -1;
            at=base+info.RegionSize;
        }
        SIZE_T written{};
        return WriteProcessMemory(GetCurrentProcess(),output,commands.data(),bytes,&written)&&written==bytes?0:-1;
    }catch(...){return -1;}
}
std::optional<std::uintptr_t> resolve_gnm_commands(const HostImport& r){
    if(r.type!=2||r.library!="libSceGnmDriver"||r.module!="libSceGnmDriver"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="nF6bFRUBRAU")return reinterpret_cast<std::uintptr_t>(&dispatch_initial_state);
    if(r.nid=="Idffwf3yh8s")return reinterpret_cast<std::uintptr_t>(&draw_initial_state);
    if(r.nid=="1qXLHIpROPE")return reinterpret_cast<std::uintptr_t>(&insert_wait_flip);
    if(r.nid=="KXltnCwEJHQ")return reinterpret_cast<std::uintptr_t>(&set_compute_shader);
    if(r.nid=="7qZVNgEu+SY")return reinterpret_cast<std::uintptr_t>(&pop_marker);
    if(r.nid=="W1Etj-jlW7Y")return reinterpret_cast<std::uintptr_t>(&push_marker);
    if(r.nid=="xbxNatawohc")return reinterpret_cast<std::uintptr_t>(&submit_and_flip_command_buffers);
    if(r.nid=="yvZ73uQUqrk")return reinterpret_cast<std::uintptr_t>(&submit_done_unavailable);
    if(r.nid=="TLV4mswiZ4A")return reinterpret_cast<std::uintptr_t>(&driver_capture_in_progress);
    if(r.nid=="jg33rEKLfVs")return reinterpret_cast<std::uintptr_t>(&user_pa_enabled);
    if(r.nid=="qpGITzPE+Zc")return reinterpret_cast<std::uintptr_t>(&debug_hardware_status);
    return std::nullopt;
}
}
