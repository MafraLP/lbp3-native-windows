#include "lbp3_host/pad_services.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <Xinput.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace lbp3::hle {
namespace {
constexpr std::int32_t invalid_arg=static_cast<std::int32_t>(0x80920001u);
constexpr std::int32_t invalid_handle=static_cast<std::int32_t>(0x80920003u);
constexpr std::int32_t already_opened=static_cast<std::int32_t>(0x80920004u);
constexpr std::int32_t not_initialized=static_cast<std::int32_t>(0x80920005u);
constexpr std::int32_t pad_handle=1;
constexpr std::uint32_t btn_l3=0x2,btn_r3=0x4,btn_options=0x8,btn_up=0x10,btn_right=0x20,btn_down=0x40,btn_left=0x80,
    btn_l2=0x100,btn_r2=0x200,btn_l1=0x400,btn_r1=0x800,btn_triangle=0x1000,btn_circle=0x2000,btn_cross=0x4000,btn_square=0x8000;
enum KeyboardKey : std::uint32_t {
    key_w=1u<<0,key_a=1u<<1,key_s=1u<<2,key_d=1u<<3,
    key_up=1u<<4,key_left=1u<<5,key_down_arrow=1u<<6,key_right=1u<<7,
    key_space=1u<<8,key_lshift=1u<<9,key_e=1u<<10,key_q=1u<<11,
    key_1=1u<<12,key_3=1u<<13,key_r=1u<<14,key_f=1u<<15,
    key_z=1u<<16,key_c=1u<<17,key_enter=1u<<18,
    key_i=1u<<19,key_j=1u<<20,key_k=1u<<21,key_l=1u<<22,
};
struct PadTouch {std::uint16_t x{},y{};std::uint8_t id{},reserved[3]{};};
struct PadData {
    std::uint32_t buttons{};std::uint8_t left_x{},left_y{},right_x{},right_y{},l2{},r2{},analog_padding[2]{};
    float orientation[4]{},acceleration[3]{},angular_velocity[3]{};
    std::uint8_t touch_count{},touch_reserved[3]{};std::uint32_t touch_held_time{};PadTouch touches[2]{};
    std::uint8_t connected{},pad0[3]{};std::uint64_t timestamp{};std::uint8_t extension[16]{};
    std::uint8_t connected_count{},reserved[2]{},unique_length{},unique[12]{};
};
struct ControllerInfo {float pixel_density{};std::uint16_t resolution_x{},resolution_y{};std::uint8_t dead_zone_left{},dead_zone_right{},connection_type{},connected_count{},connected{},pad[3]{};std::int32_t device_class{};std::uint8_t reserved[8]{};};
static_assert(sizeof(PadData)==120&&offsetof(PadData,touches)==60&&offsetof(PadData,timestamp)==80);
static_assert(sizeof(PadTouch)==8&&sizeof(ControllerInfo)==28);
std::mutex state_mutex;bool initialized{},opened{};
std::atomic<unsigned> pad_trace_count{};
std::atomic<std::uint64_t> pad_trace_last_state{UINT64_MAX};
bool writable(void* pointer,std::size_t length) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);if(!at||length>UINTPTR_MAX-at)return false;const auto end=at+length;
    while(at<end){MEMORY_BASIC_INFORMATION info{};if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&0xff;if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }return true;
}
bool write_guest(void* destination,const void* source,std::size_t size) noexcept {SIZE_T n{};return writable(destination,size)&&WriteProcessMemory(GetCurrentProcess(),destination,source,size,&n)&&n==size;}
bool read_guest(const void* source,void* destination,std::size_t size) noexcept {SIZE_T n{};return source&&ReadProcessMemory(GetCurrentProcess(),source,destination,size,&n)&&n==size;}
std::uint8_t axis(SHORT value) noexcept {const float f=std::clamp(static_cast<float>(value)/32767.0f,-1.0f,1.0f);const auto out=static_cast<int>(std::lround(f*127.5f+127.5f));return static_cast<std::uint8_t>(std::clamp(out,0,255));}
std::uint8_t trigger(BYTE value) noexcept {return value;}
bool key_down(int key) noexcept {return (GetAsyncKeyState(key)&0x8000)!=0;}
std::uint32_t keyboard_state() noexcept {
    std::uint32_t keys{};
    constexpr std::pair<int,KeyboardKey> mappings[]={
        {0x57,key_w},{0x41,key_a},{0x53,key_s},{0x44,key_d},
        {VK_UP,key_up},{VK_LEFT,key_left},{VK_DOWN,key_down_arrow},{VK_RIGHT,key_right},
        {VK_SPACE,key_space},{VK_LSHIFT,key_lshift},{0x45,key_e},{0x51,key_q},
        {0x31,key_1},{0x33,key_3},{0x52,key_r},{0x46,key_f},{0x5a,key_z},{0x43,key_c},
        {VK_RETURN,key_enter},{0x49,key_i},{0x4a,key_j},{0x4b,key_k},{0x4c,key_l},
    };
    for(const auto [vk,bit]:mappings)if(key_down(vk))keys|=bit;
    return keys;
}
std::uint8_t axis_keys(std::uint32_t keys,KeyboardKey negative,KeyboardKey positive) noexcept {
    const bool n=(keys&negative)!=0,p=(keys&positive)!=0;
    return n==p?128:static_cast<std::uint8_t>(n?0:255);
}
void apply_keyboard(PadData& data,std::uint32_t keys) noexcept {
    data.connected=1;data.connected_count=1;
    if(!keys)return;
    data.left_x=axis_keys(keys,key_a,key_d);data.left_y=axis_keys(keys,key_w,key_s);
    data.right_x=axis_keys(keys,key_left,key_right);data.right_y=axis_keys(keys,key_up,key_down_arrow);
    if(keys&key_space)data.buttons|=btn_cross;if(keys&key_lshift)data.buttons|=btn_circle;
    if(keys&key_e)data.buttons|=btn_square;if(keys&key_q)data.buttons|=btn_triangle;
    if(keys&key_1)data.buttons|=btn_l1;if(keys&key_3)data.buttons|=btn_r1;
    if(keys&key_r){data.buttons|=btn_l2;data.l2=255;}if(keys&key_f){data.buttons|=btn_r2;data.r2=255;}
    if(keys&key_z)data.buttons|=btn_l3;if(keys&key_c)data.buttons|=btn_r3;
    if(keys&key_enter)data.buttons|=btn_options;
    if(keys&key_i)data.buttons|=btn_up;if(keys&key_l)data.buttons|=btn_right;
    if(keys&key_k)data.buttons|=btn_down;if(keys&key_j)data.buttons|=btn_left;
}
std::uint8_t connected_count() noexcept {std::uint8_t count{};for(DWORD i=0;i<XUSER_MAX_COUNT;++i){XINPUT_STATE s{};if(XInputGetState(i,&s)==ERROR_SUCCESS)++count;}return count?count:1;}
void sample(PadData& data) noexcept {
    data.left_x=data.left_y=data.right_x=data.right_y=128;data.timestamp=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    XINPUT_STATE state{};if(XInputGetState(0,&state)!=ERROR_SUCCESS){apply_keyboard(data,keyboard_state());return;}
    data.connected=1;data.connected_count=connected_count();const auto& g=state.Gamepad;const auto b=g.wButtons;
    if(b&XINPUT_GAMEPAD_LEFT_THUMB)data.buttons|=btn_l3;if(b&XINPUT_GAMEPAD_RIGHT_THUMB)data.buttons|=btn_r3;
    if(b&XINPUT_GAMEPAD_START)data.buttons|=btn_options;if(b&XINPUT_GAMEPAD_DPAD_UP)data.buttons|=btn_up;
    if(b&XINPUT_GAMEPAD_DPAD_RIGHT)data.buttons|=btn_right;if(b&XINPUT_GAMEPAD_DPAD_DOWN)data.buttons|=btn_down;if(b&XINPUT_GAMEPAD_DPAD_LEFT)data.buttons|=btn_left;
    if(b&XINPUT_GAMEPAD_LEFT_SHOULDER)data.buttons|=btn_l1;if(b&XINPUT_GAMEPAD_RIGHT_SHOULDER)data.buttons|=btn_r1;
    if(b&XINPUT_GAMEPAD_A)data.buttons|=btn_cross;if(b&XINPUT_GAMEPAD_B)data.buttons|=btn_circle;if(b&XINPUT_GAMEPAD_X)data.buttons|=btn_square;if(b&XINPUT_GAMEPAD_Y)data.buttons|=btn_triangle;
    data.left_x=axis(g.sThumbLX);data.left_y=static_cast<std::uint8_t>(255-axis(g.sThumbLY));data.right_x=axis(g.sThumbRX);data.right_y=static_cast<std::uint8_t>(255-axis(g.sThumbRY));
    data.l2=trigger(g.bLeftTrigger);data.r2=trigger(g.bRightTrigger);if(data.l2>30)data.buttons|=btn_l2;if(data.r2>30)data.buttons|=btn_r2;
}
std::int32_t LBP3_GUEST_ABI pad_init() noexcept {std::lock_guard lock(state_mutex);initialized=true;return 0;}
std::int32_t LBP3_GUEST_ABI camera_is_attached() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI move_init() noexcept {return 0;}
std::size_t LBP3_GUEST_ABI move_tracker_working_memory_size() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI move_tracker_init(void*,void*,std::int32_t,std::int32_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI pad_open(std::int32_t user,std::int32_t type,std::int32_t index,const void*) noexcept {
    std::lock_guard lock(state_mutex);if(!initialized)return not_initialized;if(user!=1||(type!=0&&type!=2)||index!=0)return invalid_arg;if(opened)return already_opened;opened=true;return pad_handle;
}
std::int32_t LBP3_GUEST_ABI pad_close(std::int32_t handle) noexcept {std::lock_guard lock(state_mutex);if(handle!=pad_handle||!opened)return invalid_handle;opened=false;return 0;}
std::int32_t LBP3_GUEST_ABI pad_read_state(std::int32_t handle,PadData* output) noexcept {
    {std::lock_guard lock(state_mutex);if(handle!=pad_handle||!opened)return invalid_handle;}if(!output||!writable(output,sizeof(PadData)))return invalid_arg;
    PadData data{};sample(data);
    static const bool trace_enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"LBP3_PAD_TRACE",value,static_cast<DWORD>(std::size(value)))>0;}();
    if(trace_enabled){
        const auto n=pad_trace_count.fetch_add(1,std::memory_order_relaxed);
        std::uint64_t signature=1469598103934665603ull;
        const std::uint8_t state[]={
            static_cast<std::uint8_t>(data.buttons),static_cast<std::uint8_t>(data.buttons>>8),
            static_cast<std::uint8_t>(data.buttons>>16),static_cast<std::uint8_t>(data.buttons>>24),
            data.connected,data.left_x,data.left_y,data.right_x,data.right_y,data.l2,data.r2,
        };
        for(const auto byte:state){signature^=byte;signature*=1099511628211ull;}
        const auto previous=pad_trace_last_state.exchange(signature,std::memory_order_relaxed);
        if(n<8||previous!=signature)
            std::cerr<<"[LBP3_PAD_TRACE] read="<<(n+1)<<" buttons=0x"<<std::hex<<data.buttons<<std::dec<<" connected="<<static_cast<unsigned>(data.connected)<<" axes="<<static_cast<unsigned>(data.left_x)<<','<<static_cast<unsigned>(data.left_y)<<','<<static_cast<unsigned>(data.right_x)<<','<<static_cast<unsigned>(data.right_y)<<"\n";
    }
    return write_guest(output,&data,sizeof(data))?0:invalid_arg;
}
std::int32_t LBP3_GUEST_ABI pad_info(std::int32_t handle,ControllerInfo* output) noexcept {
    {std::lock_guard lock(state_mutex);if(handle!=pad_handle||!opened)return invalid_handle;}if(!output||!writable(output,sizeof(ControllerInfo)))return invalid_arg;
    ControllerInfo info{};info.pixel_density=44.86f;info.resolution_x=1920;info.resolution_y=943;info.dead_zone_left=info.dead_zone_right=2;
    info.connected_count=connected_count();info.connected=info.connected_count?1:0;return write_guest(output,&info,sizeof(info))?0:invalid_arg;
}
std::int32_t LBP3_GUEST_ABI pad_vibration(std::int32_t handle,const std::uint8_t* parameter) noexcept {
    {std::lock_guard lock(state_mutex);if(handle!=pad_handle||!opened)return invalid_handle;}std::array<std::uint8_t,2> amplitudes{};if(!read_guest(parameter,amplitudes.data(),amplitudes.size()))return invalid_arg;
    XINPUT_VIBRATION vibration{};vibration.wLeftMotorSpeed=static_cast<WORD>(amplitudes[0]*257u);vibration.wRightMotorSpeed=static_cast<WORD>(amplitudes[1]*257u);(void)XInputSetState(0,&vibration);return 0;
}
std::int32_t LBP3_GUEST_ABI pad_lightbar(std::int32_t handle,const std::uint8_t* rgb) noexcept {
    {std::lock_guard lock(state_mutex);if(handle!=pad_handle||!opened)return invalid_handle;}std::array<std::uint8_t,3> color{};if(!read_guest(rgb,color.data(),color.size()))return invalid_arg;return 0;
}
std::int32_t LBP3_GUEST_ABI pad_ok(std::int32_t handle) noexcept {std::lock_guard lock(state_mutex);return handle==pad_handle&&opened?0:invalid_handle;}
std::int32_t LBP3_GUEST_ABI pad_ok_flag(std::int32_t handle,std::uint8_t) noexcept {return pad_ok(handle);}
std::int32_t LBP3_GUEST_ABI pad_set_motion(std::int32_t handle,std::uint8_t) noexcept {return pad_ok(handle);}
}

std::optional<std::uintptr_t> resolve_pad_services(const HostImport& request){
    if(request.type==2&&request.library=="libSceCamera"&&request.module=="libSceCamera"&&request.library_version==1&&request.module_version==0x0101&&request.nid=="p6n3Npi3YY4")
        return reinterpret_cast<std::uintptr_t>(&camera_is_attached);
    if(request.type==2&&request.library=="libSceMove"&&request.module=="libSceMove"&&request.library_version==1&&request.module_version==0x0101&&request.nid=="j1ITE-EoJmE")
        return reinterpret_cast<std::uintptr_t>(&move_init);
    if(request.type==2&&request.library=="libSceMoveTracker"&&request.module=="libSceMoveTracker"&&request.library_version==1&&request.module_version==0x0101&&request.nid=="gg1d4KsyVVs")
        return reinterpret_cast<std::uintptr_t>(&move_tracker_working_memory_size);
    if(request.type==2&&request.library=="libSceMoveTracker"&&request.module=="libSceMoveTracker"&&request.library_version==1&&request.module_version==0x0101&&request.nid=="F4w2atwG428")
        return reinterpret_cast<std::uintptr_t>(&move_tracker_init);
    if(request.type!=2||request.library!="libScePad"||request.module!="libScePad"||request.library_version!=1||request.module_version!=0x0101)return std::nullopt;
    if(request.nid=="hv1luiJrqQM")return reinterpret_cast<std::uintptr_t>(&pad_init);
    if(request.nid=="xk0AcarP3V4")return reinterpret_cast<std::uintptr_t>(&pad_open);
    if(request.nid=="6ncge5+l5Qs")return reinterpret_cast<std::uintptr_t>(&pad_close);
    if(request.nid=="YndgXqQVV7c")return reinterpret_cast<std::uintptr_t>(&pad_read_state);
    if(request.nid=="gjP9-KQzoUk")return reinterpret_cast<std::uintptr_t>(&pad_info);
    if(request.nid=="yFVnOdGxvZY")return reinterpret_cast<std::uintptr_t>(&pad_vibration);
    if(request.nid=="RR4novUEENY")return reinterpret_cast<std::uintptr_t>(&pad_lightbar);
    if(request.nid=="DscD1i9HX1w"||request.nid=="rIZnR6eSpvk")return reinterpret_cast<std::uintptr_t>(&pad_ok);
    if(request.nid=="r44mAxdSG+U"||request.nid=="vDLMoJLde8I")return reinterpret_cast<std::uintptr_t>(&pad_ok_flag);
    if(request.nid=="clVvL4ZDntw")return reinterpret_cast<std::uintptr_t>(&pad_set_motion);
    return std::nullopt;
}
void pad_services_self_test(std::ostream& output){
    using Init=std::int32_t (LBP3_GUEST_ABI *)() noexcept;using Open=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t,std::int32_t,const void*) noexcept;
    using Close=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t) noexcept;using Read=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,PadData*) noexcept;
    using Info=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,ControllerInfo*) noexcept;
    auto lookup=[](const char* nid){const auto p=resolve_pad_services({nid,"libScePad","libScePad",1,0x0101,2});if(!p)throw std::runtime_error("Pad import unresolved in fixture.");return *p;};
    auto init=reinterpret_cast<Init>(lookup("hv1luiJrqQM"));auto open=reinterpret_cast<Open>(lookup("xk0AcarP3V4"));auto close=reinterpret_cast<Close>(lookup("6ncge5+l5Qs"));
    auto read=reinterpret_cast<Read>(lookup("YndgXqQVV7c"));auto info=reinterpret_cast<Info>(lookup("gjP9-KQzoUk"));
    if(open(1,0,0,nullptr)!=not_initialized||init()||open(2,0,0,nullptr)!=invalid_arg||open(1,0,1,nullptr)!=invalid_arg)throw std::runtime_error("Pad init/open validation failed.");
    const auto handle=open(1,0,0,nullptr);if(handle!=pad_handle||open(1,0,0,nullptr)!=already_opened)throw std::runtime_error("Pad handle lifecycle failed.");
    PadData data{};if(read(handle,&data)||data.left_x!=128||data.left_y!=128||data.right_x!=128||data.right_y!=128||data.touch_count>2||data.connected>1)
        throw std::runtime_error("Pad state ABI layout or neutral state failed.");
    PadData keyboard{};keyboard.left_x=keyboard.left_y=keyboard.right_x=keyboard.right_y=128;
    apply_keyboard(keyboard,0);
    if(!keyboard.connected||keyboard.connected_count!=1||keyboard.buttons||keyboard.left_x!=128||keyboard.left_y!=128)
        throw std::runtime_error("Idle keyboard fallback must remain connected with a neutral state.");
    keyboard={};keyboard.left_x=keyboard.left_y=keyboard.right_x=keyboard.right_y=128;
    apply_keyboard(keyboard,key_space|key_w|key_d|key_up|key_i);
    if(!keyboard.connected||keyboard.connected_count!=1||keyboard.left_x!=255||keyboard.left_y!=0||
       keyboard.right_x!=128||keyboard.right_y!=0||(keyboard.buttons&(btn_cross|btn_up))!=(btn_cross|btn_up))
        throw std::runtime_error("Keyboard pad mapping failed for Cross, d-pad and analog axes.");
    PadData opposed{};opposed.left_x=opposed.left_y=opposed.right_x=opposed.right_y=128;
    apply_keyboard(opposed,key_a|key_d);
    if(opposed.left_x!=128||opposed.left_y!=128)throw std::runtime_error("Opposing keyboard directions failed to cancel.");
    ControllerInfo controller{};if(info(handle,&controller)||controller.resolution_x!=1920||controller.resolution_y!=943||controller.connected>1||controller.connected_count>4)
        throw std::runtime_error("Pad controller information ABI failed.");
    if(read(handle,reinterpret_cast<PadData*>(1))!=invalid_arg||read(handle,nullptr)!=invalid_arg||close(handle)||close(handle)!=invalid_handle||read(handle,&data)!=invalid_handle)
        throw std::runtime_error("Pad pointer/close validation failed.");
    if(resolve_pad_services({"hv1luiJrqQM","libkernel","libScePad",1,0x0101,2})||resolve_pad_services({"hv1luiJrqQM","libScePad","libScePad",1,0x0100,2}))
        throw std::runtime_error("Pad resolver accepted wrong module identity.");
    using CameraCheck=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    const auto camera=resolve_pad_services({"p6n3Npi3YY4","libSceCamera","libSceCamera",1,0x0101,2});
    if(!camera||reinterpret_cast<CameraCheck>(*camera)()!=0||resolve_pad_services({"p6n3Npi3YY4","libScePad","libSceCamera",1,0x0101,2}))
        throw std::runtime_error("Disconnected-camera fallback or strict resolver identity failed.");
    const auto move=resolve_pad_services({"j1ITE-EoJmE","libSceMove","libSceMove",1,0x0101,2});
    if(!move||reinterpret_cast<CameraCheck>(*move)()!=0||resolve_pad_services({"j1ITE-EoJmE","libScePad","libSceMove",1,0x0101,2}))
        throw std::runtime_error("Motion-controller init fallback or strict resolver identity failed.");
    using SizeQuery=std::size_t (LBP3_GUEST_ABI *)() noexcept;
    const auto tracker=resolve_pad_services({"gg1d4KsyVVs","libSceMoveTracker","libSceMoveTracker",1,0x0101,2});
    if(!tracker||reinterpret_cast<SizeQuery>(*tracker)()!=0||resolve_pad_services({"gg1d4KsyVVs","libSceMove","libSceMoveTracker",1,0x0101,2}))
        throw std::runtime_error("Disconnected motion tracker working-memory query failed.");
    using TrackerInit=std::int32_t (LBP3_GUEST_ABI *)(void*,void*,std::int32_t,std::int32_t) noexcept;
    const auto tracker_init=resolve_pad_services({"F4w2atwG428","libSceMoveTracker","libSceMoveTracker",1,0x0101,2});
    if(!tracker_init||reinterpret_cast<TrackerInit>(*tracker_init)(nullptr,nullptr,4,3)!=0||resolve_pad_services({"F4w2atwG428","libScePad","libSceMoveTracker",1,0x0101,2}))
        throw std::runtime_error("Disconnected motion tracker initialization failed.");
    output<<"Pad/camera tests passed: init/open/close handles, neutral 120-byte data ABI, controller info, pointer validation, XInput and keyboard button/axis mapping, disconnected-camera response and no-tracker memory query.\n";
}
}
