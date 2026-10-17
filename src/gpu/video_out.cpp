#include "lbp3_host/video_out.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <bit>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
namespace lbp3::gpu {
namespace {
constexpr std::int32_t error(unsigned code){return std::bit_cast<std::int32_t>(0x80290000u|code);}
template<class T> bool write(T* output,const T& value){
    auto at=reinterpret_cast<std::uintptr_t>(output);if(!at||at>UINTPTR_MAX-sizeof(T))return false;
    const auto end=at+sizeof(T);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto prot=info.Protect&255;
        if(prot!=PAGE_READWRITE&&prot!=PAGE_WRITECOPY&&prot!=PAGE_EXECUTE_READWRITE&&prot!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),output,&value,sizeof(value),&n)&&n==sizeof(value);
}
struct Port {
    std::int32_t handle{};HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};void* pixels{};int rate{};FlipStatus flip;
    std::array<std::optional<BufferAttribute>,4> groups;
    struct Slot {std::uintptr_t address{};int group{-1};};std::array<Slot,16> slots;
    alignas(8) std::array<std::uint64_t,16> labels{};
    std::vector<abi::EventQueueHandle> flip_queues;
    ~Port(){for(const auto queue:flip_queues)(void)abi::event_remove(queue,6,-13);if(dc&&previous&&previous!=HGDI_ERROR)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);}
};
std::mutex registry;
VideoState* active{};
std::int32_t next_handle{1};
}
struct VideoState {Resolution resolution;std::unique_ptr<Port> port;};
ScopedVideoOut::ScopedVideoOut(std::uint32_t width,std::uint32_t height):state_(std::make_unique<VideoState>()){
    if(!width||!height||width>4096||height>2160)throw std::runtime_error("Invalid video output dimensions.");
    state_->resolution.full_width=state_->resolution.pane_width=static_cast<std::int32_t>(width);
    state_->resolution.full_height=state_->resolution.pane_height=static_cast<std::int32_t>(height);
    std::lock_guard lock(registry);if(active)throw std::runtime_error("VideoOut scope already active.");active=state_.get();
}
ScopedVideoOut::~ScopedVideoOut(){std::lock_guard lock(registry);active=nullptr;}
std::int32_t LBP3_GUEST_ABI video_open(std::int32_t user,int bus,int index,const void* options) noexcept {
    try{std::lock_guard lock(registry);if(!active)return error(0x18);
        if(user < -1||bus!=0||index!=0)return error(1);if(options)return error(0x1a);
        if(active->port)return error(9);if(next_handle==INT32_MAX)return error(0xf);
        auto port=std::make_unique<Port>();
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=active->resolution.full_width;info.bmiHeader.biHeight=-active->resolution.full_height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        port->dc=CreateCompatibleDC(nullptr);
        port->bitmap=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&port->pixels,nullptr,0);
        if(!port->dc||!port->bitmap||!port->pixels)return std::bit_cast<std::int32_t>(0x8029100cu);
        port->previous=SelectObject(port->dc,port->bitmap);
        if(!port->previous||port->previous==HGDI_ERROR)return error(0xfe);
        std::memset(port->pixels,0,static_cast<std::size_t>(active->resolution.full_width)*active->resolution.full_height*4);
        port->handle=next_handle++;const auto handle=port->handle;active->port=std::move(port);return handle;
    }catch(const std::bad_alloc&){return std::bit_cast<std::int32_t>(0x8029100cu);}catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_close(std::int32_t handle) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);active->port.reset();return 0;}catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_resolution(std::int32_t handle,Resolution* output) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        return write(output,active->resolution)?0:error(2);
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_flip_status(std::int32_t handle,FlipStatus* output) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        return write(output,active->port->flip)?0:error(2);
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_flip_rate(std::int32_t handle,int rate) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        if(rate<0||rate>2)return error(1);active->port->rate=rate;return 0;
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_register_buffers(std::int32_t handle,int start,void* const* addresses,int count,const BufferAttribute* input) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        auto& port=*active->port;if(start<0||start>=16||count<=0||count>16-start)return error(1);
        BufferAttribute attribute{};std::array<std::uintptr_t,16> pointers{};SIZE_T n{};
        if(!input||!ReadProcessMemory(GetCurrentProcess(),input,&attribute,sizeof(attribute),&n)||n!=sizeof(attribute))return error(2);
        if(!addresses||!ReadProcessMemory(GetCurrentProcess(),addresses,pointers.data(),static_cast<SIZE_T>(count)*8,&n)||n!=static_cast<SIZE_T>(count)*8)return error(2);
        if(attribute.reserved0||attribute.reserved1)return error(1);
        if(attribute.option)return error(0x1a);
        if(attribute.aspect)return error(8);
        if(!attribute.width||!attribute.height||attribute.width>4096||attribute.height>2160)return error(5);
        if(attribute.pitch<attribute.width||attribute.pitch>8192)return error(4);
        if(attribute.tiling<0||attribute.tiling>1)return error(7);
        unsigned bytes_per_pixel=4;
        switch(attribute.pixel_format){case 0x80000000u:case 0x80002200u:case 0x88060000u:case 0x88000000u:break;
            case 0xc1060000u:bytes_per_pixel=8;break;default:return error(3);}
        // Register the storage and layout; tiled conversion/presentation is not
        // claimed here. Conservative tile padding must fit the owned section.
        const auto rows=attribute.tiling==0?(static_cast<std::uint64_t>(attribute.height)+63)&~63ull:attribute.height;
        const auto pitch=attribute.tiling==0?(static_cast<std::uint64_t>(attribute.pitch)+127)&~127ull:attribute.pitch;
        const auto length=rows*pitch*bytes_per_pixel;
        int group=-1;for(unsigned i=0;i<port.groups.size();++i)if(!port.groups[i]){group=static_cast<int>(i);break;}
        if(group<0)return error(0xf);
        for(int i=0;i<count;++i){
            if(port.slots[start+i].group>=0)return error(0x10);
            if(!pointers[i]||pointers[i]%256)return error(0x15);
            const auto view=memory::direct_view(pointers[i]);
            if(!view||!(view->protection&0x10)||length>view->length-(pointers[i]-view->address))return error(0x13);
        }
        port.groups[group]=attribute;
        for(int i=0;i<count;++i){port.slots[start+i]={pointers[i],group};port.labels[start+i]=0;}
        return group;
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_unregister_buffers(std::int32_t handle,int group) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        auto& port=*active->port;if(group<0||group>=4||!port.groups[group])return error(1);
        for(unsigned i=0;i<port.slots.size();++i)if(port.slots[i].group==group){port.slots[i]={};port.labels[i]=0;if(port.flip.current==static_cast<int>(i))port.flip.current=-1;}
        port.groups[group].reset();return 0;
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_buffer_labels(std::int32_t handle,std::uintptr_t* output) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        return write(output,reinterpret_cast<std::uintptr_t>(active->port->labels.data()))?16:error(2);
    }catch(...){return error(0xfe);}
}
std::optional<RegisteredVideoBuffer> video_buffer(std::int32_t handle,int index){
    std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle||index<0||index>=16)return std::nullopt;
    const auto& slot=active->port->slots[index];if(slot.group<0)return std::nullopt;
    return RegisteredVideoBuffer{slot.address,*active->port->groups[slot.group],slot.group};
}
std::int32_t LBP3_GUEST_ABI video_add_flip_event(std::int64_t queue,std::int32_t handle,void* userdata) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        auto& subscriptions=active->port->flip_queues;
        const bool exists=std::find(subscriptions.begin(),subscriptions.end(),queue)!=subscriptions.end();
        if(!exists)subscriptions.push_back(queue);
        const auto result=abi::event_subscribe(queue,{6,-13,0x20,0,0,userdata});
        if(result){if(!exists)subscriptions.pop_back();return result==std::bit_cast<std::int32_t>(0x80020009u)?error(0xc):std::bit_cast<std::int32_t>(0x8029100cu);}
        return 0;
    }catch(...){return std::bit_cast<std::int32_t>(0x8029100cu);}
}
std::int32_t LBP3_GUEST_ABI video_delete_flip_event(std::int64_t queue,std::int32_t handle) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        const auto result=abi::event_remove(queue,6,-13);
        if(result)return result==std::bit_cast<std::int32_t>(0x80020009u)?error(0xc):error(0xd);
        std::erase(active->port->flip_queues,queue);return 0;
    }catch(...){return error(0xfe);}
}
std::int32_t LBP3_GUEST_ABI video_window_margins(std::int32_t handle,int top,int bottom) noexcept {
    try{std::lock_guard lock(registry);if(!active||!active->port||active->port->handle!=handle)return error(0xb);
        if(top<0||bottom<0)return error(1);
        // The console's current known implementation accepts margins without
        // changing the output mode; this is the same explicit no-op contract.
        return 0;
    }catch(...){return error(0xfe);}
}
void LBP3_GUEST_ABI video_attribute(BufferAttribute* output,std::uint32_t format,std::uint32_t tiling,std::uint32_t aspect,std::uint32_t width,std::uint32_t height,std::uint32_t pitch) noexcept {
    const BufferAttribute value{format,static_cast<std::int32_t>(tiling),static_cast<std::int32_t>(aspect),width,height,pitch,0,0,0};
    if(!write(output,value)){
        std::fputs("Guest diagnostic stop: sceVideoOutSetBufferAttribute output is inaccessible.\n",stderr);
        for(;;)TerminateProcess(GetCurrentProcess(),0xe04c4205u);
    }
}
std::optional<std::uintptr_t> resolve_video_out(const HostImport& r){
    if(r.type!=2||r.library!="libSceVideoOut"||r.module!="libSceVideoOut"||r.library_version!=1||r.module_version!=0)return std::nullopt;
    if(r.nid=="Up36PTk687E")return reinterpret_cast<std::uintptr_t>(&video_open);
    if(r.nid=="uquVH4-Du78")return reinterpret_cast<std::uintptr_t>(&video_close);
    if(r.nid=="6kPnj51T62Y")return reinterpret_cast<std::uintptr_t>(&video_resolution);
    if(r.nid=="SbU3dwp80lQ")return reinterpret_cast<std::uintptr_t>(&video_flip_status);
    if(r.nid=="CBiu4mCE1DA")return reinterpret_cast<std::uintptr_t>(&video_flip_rate);
    if(r.nid=="i6-sR91Wt-4")return reinterpret_cast<std::uintptr_t>(&video_attribute);
    if(r.nid=="w3BY+tAEiQY")return reinterpret_cast<std::uintptr_t>(&video_register_buffers);
    if(r.nid=="N5KDtkIjjJ4")return reinterpret_cast<std::uintptr_t>(&video_unregister_buffers);
    if(r.nid=="OcQybQejHEY")return reinterpret_cast<std::uintptr_t>(&video_buffer_labels);
    if(r.nid=="HXzjK9yI30k")return reinterpret_cast<std::uintptr_t>(&video_add_flip_event);
    if(r.nid=="-Ozn0F1AFRg")return reinterpret_cast<std::uintptr_t>(&video_delete_flip_event);
    if(r.nid=="MTxxrOCeSig")return reinterpret_cast<std::uintptr_t>(&video_window_margins);
    return std::nullopt;
}
}
