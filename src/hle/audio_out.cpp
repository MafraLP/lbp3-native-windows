#include "lbp3_host/audio_out.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <mutex>
#include <stdexcept>

namespace lbp3::hle {
namespace {
constexpr std::int32_t not_opened=std::bit_cast<std::int32_t>(0x80260001u);
constexpr std::int32_t invalid_port=std::bit_cast<std::int32_t>(0x80260003u);
constexpr std::int32_t invalid_pointer=std::bit_cast<std::int32_t>(0x80260004u);
constexpr std::int32_t port_full=std::bit_cast<std::int32_t>(0x80260005u);
constexpr std::int32_t invalid_size=std::bit_cast<std::int32_t>(0x80260006u);
constexpr std::int32_t invalid_format=std::bit_cast<std::int32_t>(0x80260007u);
constexpr std::int32_t invalid_frequency=std::bit_cast<std::int32_t>(0x80260008u);
constexpr std::int32_t invalid_volume=std::bit_cast<std::int32_t>(0x80260009u);
constexpr std::int32_t invalid_type=std::bit_cast<std::int32_t>(0x8026000au);
constexpr std::int32_t already_initialized=std::bit_cast<std::int32_t>(0x8026000eu);
constexpr std::int32_t not_initialized=std::bit_cast<std::int32_t>(0x8026000fu);
constexpr std::int32_t frames_max=2048;
struct Port {bool used{};std::int32_t handle{},type{},frames{},channels{},format{};std::array<std::int32_t,8> volume{};std::uint64_t next_ns{};};
struct PortState {std::uint16_t output{};std::uint8_t channel{},reserved{};std::int16_t volume{};std::uint16_t reroute{};std::uint64_t flags{},reserved64[2]{};};
static_assert(sizeof(PortState)==32);
std::mutex mutex;bool initialized{};std::array<Port,25> ports{};
const auto epoch=std::chrono::steady_clock::now();
std::uint64_t now_ns() noexcept {return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch).count());}
bool writable(void* pointer,std::size_t size) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);if(!at||size>UINTPTR_MAX-at)return false;const auto end=at+size;
    while(at<end){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto p=info.Protect&255;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;}
    return true;
}
template<class T> bool write_guest(T* pointer,const T& value) noexcept {SIZE_T n{};return writable(pointer,sizeof(T))&&WriteProcessMemory(GetCurrentProcess(),pointer,&value,sizeof(T),&n)&&n==sizeof(T);}
bool range(std::int32_t type,std::size_t& first,std::size_t& last) noexcept {
    switch(type){case 0:first=0;last=7;return true;case 1:first=8;last=8;return true;case 2:first=9;last=12;return true;case 3:first=13;last=16;return true;case 4:first=17;last=20;return true;case 126:first=21;last=22;return true;case 127:first=23;last=24;return true;default:return false;}
}
Port* lookup(std::int32_t handle) noexcept {const auto id=static_cast<std::uint32_t>(handle)&0xffu;if((static_cast<std::uint32_t>(handle)&0x3f000000u)!=0x20000000u||id>=ports.size())return nullptr;auto& p=ports[id];return p.used&&p.handle==handle?&p:nullptr;}
std::int32_t LBP3_GUEST_ABI audio_init() noexcept {std::lock_guard lock(mutex);if(initialized)return already_initialized;initialized=true;return 0;}
std::int32_t LBP3_GUEST_ABI audio_open(std::int32_t,std::int32_t type,std::int32_t,std::uint32_t frames,std::uint32_t frequency,std::uint32_t parameter) noexcept {
    std::lock_guard lock(mutex);if(!initialized)return not_initialized;if(!frames||frames>frames_max||(frames&0xff))return invalid_size;if(frequency!=48000)return invalid_frequency;
    const auto format=parameter&0xffu;if(format>7)return invalid_format;std::size_t first{},last{};if(!range(type,first,last))return invalid_type;
    constexpr std::array<std::int32_t,8> channels{1,2,8,1,2,8,8,8};for(auto i=first;i<=last;++i)if(!ports[i].used){auto& p=ports[i];p={};p.used=true;p.type=type;p.frames=static_cast<std::int32_t>(frames);p.channels=channels[format];p.format=static_cast<std::int32_t>(format);p.handle=static_cast<std::int32_t>(0x20000000u|(static_cast<std::uint32_t>(type)<<16)|static_cast<std::uint32_t>(i));p.volume.fill(32768);return p.handle;}
    return port_full;
}
std::int32_t LBP3_GUEST_ABI audio_close(std::int32_t handle) noexcept {std::lock_guard lock(mutex);auto* p=lookup(handle);if(!p)return not_opened;p->used=false;return 0;}
std::int32_t LBP3_GUEST_ABI audio_output(std::int32_t handle,const void* data) noexcept {
    std::uint64_t deadline{},period{};std::int32_t samples{};{
        std::lock_guard lock(mutex);auto* p=lookup(handle);if(!p)return not_opened;samples=p->frames*p->channels;period=static_cast<std::uint64_t>(p->frames)*1000000000ull/48000ull;const auto now=now_ns();if(!p->next_ns||now>p->next_ns+8*period)p->next_ns=now;p->next_ns+=period;deadline=p->next_ns;
    }
    for(auto now=now_ns();now<deadline;now=now_ns()){const auto remain=deadline-now;Sleep(static_cast<DWORD>(std::max<std::uint64_t>(1,remain/1000000ull)));}
    return data?samples:0;
}
std::int32_t LBP3_GUEST_ABI audio_volume(std::int32_t handle,std::int32_t flags,const std::int32_t* values) noexcept {
    std::lock_guard lock(mutex);auto* p=lookup(handle);if(!p)return not_opened;if(!values)return invalid_pointer;std::array<std::int32_t,8> copy{};SIZE_T n{};if(!ReadProcessMemory(GetCurrentProcess(),values,copy.data(),sizeof(copy),&n)||n!=sizeof(copy))return invalid_pointer;
    for(std::size_t i=0;i<copy.size();++i)if((flags&(1<<i))&&(copy[i]<0||copy[i]>32768))return invalid_volume;for(std::size_t i=0;i<copy.size();++i)if(flags&(1<<i))p->volume[i]=copy[i];return 0;
}
std::int32_t LBP3_GUEST_ABI audio_state(std::int32_t handle,PortState* output) noexcept {
    std::lock_guard lock(mutex);auto* p=lookup(handle);if(!p)return not_opened;if(!output)return invalid_pointer;PortState state{};if(p->type==2||p->type==3){state.output=0x40;state.channel=1;}else if(p->type==4){state.output=0x04;state.channel=1;state.volume=127;}else{state.output=0x01;state.channel=static_cast<std::uint8_t>(p->channels>2?2:p->channels);}return write_guest(output,state)?0:invalid_pointer;
}
}

std::optional<std::uintptr_t> resolve_audio_out(const HostImport& r){
    if(r.type!=2||r.library!="libSceAudioOut"||r.module!="libSceAudioOut"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="JfEPXVxhFqA")return reinterpret_cast<std::uintptr_t>(&audio_init);
    if(r.nid=="ekNvsT22rsY")return reinterpret_cast<std::uintptr_t>(&audio_open);
    if(r.nid=="s1--uE9mBFw")return reinterpret_cast<std::uintptr_t>(&audio_close);
    if(r.nid=="QOQtbeDqsT4")return reinterpret_cast<std::uintptr_t>(&audio_output);
    if(r.nid=="b+uAV89IlxE")return reinterpret_cast<std::uintptr_t>(&audio_volume);
    if(r.nid=="GrQ9s4IrNaQ")return reinterpret_cast<std::uintptr_t>(&audio_state);
    return std::nullopt;
}

void audio_out_self_test(){
    using Init=std::int32_t (LBP3_GUEST_ABI *)();using Open=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t,std::int32_t,std::uint32_t,std::uint32_t,std::uint32_t);using Close=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);using Output=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const void*);
    const auto get=[](const char* nid){return resolve_audio_out({nid,"libSceAudioOut","libSceAudioOut",1,0x0101,2});};
    auto init=get("JfEPXVxhFqA");auto open=get("ekNvsT22rsY");auto close=get("s1--uE9mBFw");auto output=get("QOQtbeDqsT4");if(!init||!open||!close||!output)throw std::runtime_error("AudioOut resolver contract missing.");
    if(reinterpret_cast<Init>(*init)()!=0||reinterpret_cast<Init>(*init)()!=already_initialized)throw std::runtime_error("AudioOut initialization state failed.");
    auto create=reinterpret_cast<Open>(*open);auto handle=create(1,0,0,256,48000,1);if(handle<=0||create(1,0,0,128,48000,1)!=invalid_size||create(1,0,0,256,44100,1)!=invalid_frequency)throw std::runtime_error("AudioOut port validation failed.");
    const auto start=std::chrono::steady_clock::now();if(reinterpret_cast<Output>(*output)(handle,reinterpret_cast<void*>(1))!=512||std::chrono::steady_clock::now()-start<std::chrono::milliseconds(4))throw std::runtime_error("Silent AudioOut pacing/output failed.");
    if(reinterpret_cast<Close>(*close)(handle)!=0||reinterpret_cast<Close>(*close)(handle)!=not_opened)throw std::runtime_error("AudioOut close and handle validation failed.");
}
}
