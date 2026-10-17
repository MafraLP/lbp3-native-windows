#include "lbp3_host/video_out.hpp"
#include "lbp3_host/direct_memory.hpp"
#include "lbp3_host/event_queue.hpp"
#include <Windows.h>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace lbp3::gpu;
constexpr std::int32_t err(unsigned code){return std::bit_cast<std::int32_t>(0x80290000u|code);}
void check(bool value){if(!value)throw std::runtime_error("VideoOut assertion failed");}
int main(int argc,char**){try{
    if(argc>1){video_attribute(nullptr,0,0,0,0,0,0);return 1;}
    check(video_open(-1,0,0,nullptr)==err(0x18));
    // Warm up GDI once before comparing per-port resource ownership.
    auto warm=CreateCompatibleDC(nullptr);check(warm);DeleteDC(warm);
    const auto original=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    std::int32_t stale{};
    {ScopedVideoOut context(64,32);
        lbp3::abi::ScopedEventQueues events;lbp3::abi::EventQueueHandle eq{};check(lbp3::abi::event_queue_create(&eq,"video-events")==0);
        lbp3::memory::ScopedDirectMemory direct(0x100000);std::int64_t physical{};
        check(lbp3::memory::direct_allocate(0,0x100000,0x10000,0,0,&physical)==0);
        void* frame{};check(lbp3::memory::direct_map(&frame,0x10000,0x33,0,physical,0)==0);
        check(video_open(-2,0,0,nullptr)==err(1)&&video_open(-1,1,0,nullptr)==err(1));
        check(video_open(-1,0,1,nullptr)==err(1)&&video_open(-1,0,0,&stale)==err(0x1a));
        const auto handle=video_open(-1,0,0,nullptr);check(handle>0);stale=handle;
        check(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)>=original+2);
        check(video_open(-1,0,0,nullptr)==err(9));
        check(video_window_margins(handle,0,0)==0&&video_window_margins(handle,-1,0)==err(1)&&video_window_margins(handle,1080,0)==0);
        check(video_add_flip_event(0,handle,nullptr)==err(0xc));
        check(video_add_flip_event(eq,handle,&stale)==0);
        lbp3::abi::KernelEvent event{};int count{};const std::uint32_t poll{};
        check(lbp3::abi::event_queue_wait(eq,&event,1,&count,&poll)!=0&&count==0);
        check(lbp3::abi::event_signal(eq,6,-13,123,nullptr)==0);
        check(lbp3::abi::event_queue_wait(eq,&event,1,&count,&poll)==0&&count==1&&event.ident==6&&event.filter==-13&&event.userdata==&stale&&event.data==123);
        check(video_delete_flip_event(eq,handle)==0&&video_delete_flip_event(eq,handle)==err(0xd));
        check(video_add_flip_event(eq,handle,nullptr)==0);
        Resolution size{};check(video_resolution(handle,&size)==0&&size.full_width==64&&size.full_height==32&&size.pane_width==64&&size.refresh_rate==3);
        check(video_resolution(handle,reinterpret_cast<Resolution*>(1))==err(2));
        bool queried=false;std::thread worker([&]{Resolution actual{};queried=video_resolution(handle,&actual)==0&&actual.pane_height==32;});worker.join();check(queried);
        FlipStatus status{};check(video_flip_status(handle,&status)==0&&status.count==0&&status.current==-1&&status.argument==-1&&status.pending==0);
        check(video_flip_rate(handle,2)==0&&video_flip_rate(handle,3)==err(1));
        BufferAttribute attribute{};video_attribute(&attribute,0x80000000,1,0,64,32,96);
        check(attribute.pixel_format==0x80000000&&attribute.tiling==1&&attribute.width==64&&attribute.height==32&&attribute.pitch==96&&attribute.reserved1==0&&attribute.option==0);
        void* addresses[]{frame};
        const auto group=video_register_buffers(handle,0,addresses,1,&attribute);check(group==0);
        const auto registration=video_buffer(handle,0);check(registration&&registration->address==reinterpret_cast<std::uintptr_t>(frame)&&registration->attribute.pitch==96);
        check(video_register_buffers(handle,0,addresses,1,&attribute)==err(0x10));
        check(video_register_buffers(handle,16,addresses,1,&attribute)==err(1));
        check(video_register_buffers(handle,1,reinterpret_cast<void* const*>(1),1,&attribute)==err(2));
        auto invalid=attribute;invalid.pitch=32;check(video_register_buffers(handle,1,addresses,1,&invalid)==err(4));
        invalid=attribute;invalid.tiling=2;check(video_register_buffers(handle,1,addresses,1,&invalid)==err(7));
        invalid=attribute;invalid.reserved1=1;check(video_register_buffers(handle,1,addresses,1,&invalid)==err(1));
        void* misaligned[]{static_cast<char*>(frame)+1};check(video_register_buffers(handle,1,misaligned,1,&attribute)==err(0x15));
        std::uintptr_t labels{};check(video_buffer_labels(handle,&labels)==16&&labels%8==0&&*reinterpret_cast<std::uint64_t*>(labels)==0);
        for(int slot=1;slot<4;++slot)check(video_register_buffers(handle,slot,addresses,1,&attribute)==slot);
        check(video_register_buffers(handle,4,addresses,1,&attribute)==err(0xf));
        for(int slot=0;slot<4;++slot)check(video_unregister_buffers(handle,slot)==0&&!video_buffer(handle,slot));
        check(video_unregister_buffers(handle,0)==err(1));
        check(video_close(handle)==0&&video_close(handle)==err(0xb)&&video_resolution(handle,&size)==err(0xb));
        check(lbp3::abi::event_signal(eq,6,-13,0,nullptr)!=0);
        const auto reopened=video_open(0,0,0,nullptr);check(reopened>0&&reopened!=handle);stale=reopened;
    }
    check(video_close(stale)==err(0xb)&&GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)==original);
    for(const auto nid:{"Up36PTk687E","uquVH4-Du78","6kPnj51T62Y","SbU3dwp80lQ","CBiu4mCE1DA","i6-sR91Wt-4","w3BY+tAEiQY","N5KDtkIjjJ4","OcQybQejHEY","MTxxrOCeSig"}){
        lbp3::HostImport request{nid,"libSceVideoOut","libSceVideoOut",1,0,2};check(resolve_video_out(request).has_value());request.module_version=0x0101;check(!resolve_video_out(request));
    }
    std::cout<<"VideoOut tests passed: owned GDI output surface, resource cleanup, mode queries, margins contract, buffer registration, labels and queue event subscription. Presentation and GPU execution remain unsupported.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
