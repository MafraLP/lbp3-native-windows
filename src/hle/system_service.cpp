#include "lbp3_host/system_service.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace lbp3::hle {
namespace {
constexpr std::int32_t parameter_error=std::bit_cast<std::int32_t>(0x80a10003u);
constexpr std::int32_t no_event=std::bit_cast<std::int32_t>(0x80a10004u);
constexpr std::int32_t discmap_no_bitmap=std::bit_cast<std::int32_t>(0x81100004u);
constexpr std::int32_t sysmodule_invalid_id=std::bit_cast<std::int32_t>(0x805a1000u);
constexpr std::int32_t sysmodule_not_loaded=std::bit_cast<std::int32_t>(0x805a1001u);
constexpr std::int32_t sysmodule_busy=std::bit_cast<std::int32_t>(0x80d90003u);
std::array<std::uint32_t,65536> sysmodule_references{};
std::mutex sysmodule_mutex;
std::array<std::uint32_t,5> appcontent_parameters{1,0,0,0,0};
bool appcontent_initialized{};
constexpr std::int32_t appcontent_parameter_error=std::bit_cast<std::int32_t>(0x80d90002u);
constexpr std::int32_t appcontent_busy=std::bit_cast<std::int32_t>(0x80d90003u);
constexpr std::int32_t appcontent_no_entitlement=std::bit_cast<std::int32_t>(0x80d90007u);
constexpr std::int32_t playgo_bad_handle=std::bit_cast<std::int32_t>(0x80b20009u);
constexpr std::int32_t playgo_bad_pointer=std::bit_cast<std::int32_t>(0x80b2000au);
constexpr std::int32_t playgo_bad_size=std::bit_cast<std::int32_t>(0x80b2000bu);
constexpr std::int32_t playgo_bad_chunk=std::bit_cast<std::int32_t>(0x80b2000cu);
constexpr std::uint32_t playgo_chunk_count=256;
std::int32_t playgo_handle{};
std::mutex playgo_mutex;
constexpr std::int32_t savedata_not_initialized=std::bit_cast<std::int32_t>(0x809f0001u);
constexpr std::int32_t savedata_parameter_error=std::bit_cast<std::int32_t>(0x809f0000u);
constexpr std::int32_t savedata_not_mounted=std::bit_cast<std::int32_t>(0x809f0004u);
constexpr std::int32_t savedata_already_mounted=std::bit_cast<std::int32_t>(0x809f000du);
constexpr std::int32_t savedata_invalid_user=std::bit_cast<std::int32_t>(0x809f0011u);
std::atomic<bool> savedata_initialized{};
std::atomic<bool> savedata_mount_active{};
constexpr std::int32_t user_invalid_argument=std::bit_cast<std::int32_t>(0x80960005u);
constexpr std::int32_t user_no_event=std::bit_cast<std::int32_t>(0x80960007u);
constexpr std::int32_t user_name_too_short=std::bit_cast<std::int32_t>(0x8096000au);
std::atomic<bool> user_login_event_pending{true};

bool readable(const void* pointer,std::size_t length) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at||length>UINTPTR_MAX-at)return false;
    const auto end=at+length;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }
    return true;
}
bool writable(const void* pointer,std::size_t length) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at||length>UINTPTR_MAX-at)return false;
    const auto end=at+length;
    while(at<end){
        MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||
           (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const auto protection=info.Protect&255;
        if(protection!=PAGE_READWRITE&&protection!=PAGE_WRITECOPY&&
           protection!=PAGE_EXECUTE_READWRITE&&protection!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }
    return true;
}
bool copy_out(void* output,const void* value,std::size_t size) noexcept {
    SIZE_T written{};
    return writable(output,size)&&WriteProcessMemory(GetCurrentProcess(),output,value,size,&written)&&written==size;
}

// ABI: event count followed by five status bytes and three reserved bytes.
std::int32_t LBP3_GUEST_ABI system_status(void* output) noexcept {
    constexpr std::array<std::byte,12> state{};
    return copy_out(output,state.data(),state.size())?0:parameter_error;
}
std::int32_t LBP3_GUEST_ABI safe_area(void* output) noexcept {
    struct SafeArea {float ratio;std::array<std::byte,128> reserved;};
    static_assert(sizeof(SafeArea)==132);
    const SafeArea value{1.0f,{}};
    return copy_out(output,&value,sizeof(value))?0:parameter_error;
}
std::int32_t LBP3_GUEST_ABI receive_event(void* output) noexcept {
    (void)output;
    return no_event;
}
std::int32_t LBP3_GUEST_ABI hide_splash() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI parameter_get_int(std::int32_t id,std::int32_t* output) noexcept {
    if(!readable(output,sizeof(*output))||!writable(output,sizeof(*output)))return parameter_error;
    std::int32_t value{};
    switch(id){
    case 1: value=1; break; // English (United States)
    case 2: value=1; break; // DD/MM/YYYY
    case 3: value=1; break; // 24-hour clock
    case 4: {
        TIME_ZONE_INFORMATION zone{};
        const auto result=GetTimeZoneInformation(&zone);
        if(result==TIME_ZONE_ID_INVALID)return parameter_error;
        LONG bias=zone.Bias;
        if(result==TIME_ZONE_ID_STANDARD)bias+=zone.StandardBias;
        else if(result==TIME_ZONE_ID_DAYLIGHT)bias+=zone.DaylightBias;
        value=-bias;
        break;
    }
    case 5: {
        TIME_ZONE_INFORMATION zone{};
        const auto result=GetTimeZoneInformation(&zone);
        if(result==TIME_ZONE_ID_INVALID)return parameter_error;
        value=result==TIME_ZONE_ID_DAYLIGHT?1:0;
        break;
    }
    case 7: value=0; break; // Parental controls disabled.
    case 1000: value=1; break; // Cross is confirm.
    default:return parameter_error;
    }
    return copy_out(output,&value,sizeof(value))?0:parameter_error;
}
std::int32_t LBP3_GUEST_ABI discmap_is_request_on_hdd(const char*,std::int64_t,std::int64_t,std::int32_t*) noexcept {
    // The extracted PC package has no optical-disc bitmap, matching the ISS
    // runtime's installed-game behavior. Keep its explicit no-bitmap status.
    return discmap_no_bitmap;
}
std::int32_t LBP3_GUEST_ABI discmap_query(const char*,std::int64_t,std::int64_t,std::int32_t*,std::int32_t*,std::int32_t*) noexcept {
    return discmap_no_bitmap;
}
std::int32_t LBP3_GUEST_ABI user_service_initialize(const void*) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI user_service_terminate() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI user_service_initial(std::int32_t* output) noexcept {
    constexpr std::int32_t user_id=1;
    return copy_out(output,&user_id,sizeof(user_id))?0:user_invalid_argument;
}
std::int32_t LBP3_GUEST_ABI user_service_list(std::int32_t* output) noexcept {
    constexpr std::array<std::int32_t,4> users{1,-1,-1,-1};
    return copy_out(output,users.data(),sizeof(users))?0:user_invalid_argument;
}
std::int32_t LBP3_GUEST_ABI user_service_name(std::int32_t id,char* output,std::uint64_t size) noexcept {
    constexpr char name[]="Hunter";
    if(id!=1||!output)return user_invalid_argument;
    if(size<sizeof(name))return user_name_too_short;
    return copy_out(output,name,sizeof(name))?0:user_invalid_argument;
}
std::int32_t LBP3_GUEST_ABI user_service_event(std::int32_t* output) noexcept {
    if(!output)return user_invalid_argument;
    if(!user_login_event_pending.exchange(false,std::memory_order_acq_rel))return user_no_event;
    constexpr std::array<std::int32_t,2> event{0,1};
    if(!copy_out(output,event.data(),sizeof(event))){user_login_event_pending.store(true,std::memory_order_release);return user_invalid_argument;}
    return 0;
}
std::int32_t LBP3_GUEST_ABI common_dialog_initialize() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI savedata_initialize(const void*) noexcept {savedata_initialized.store(true,std::memory_order_release);return 0;}
std::int32_t LBP3_GUEST_ABI savedata_terminate() noexcept {
    if(!savedata_initialized.exchange(false,std::memory_order_acq_rel))return savedata_not_initialized;
    savedata_mount_active.store(false,std::memory_order_release);return 0;
}
struct SaveDirName {char data[32];};
struct SaveMount1 {
    std::int32_t user,pad;
    const void* title;
    const SaveDirName* dir;
    const void* fingerprint;
    std::uint64_t blocks;
    std::uint32_t mode;
    std::array<std::uint8_t,32> reserved;
};
struct SaveMountResult {
    char mount_point[16];
    std::uint64_t required_blocks;
    std::uint32_t unused,status;
    std::array<std::uint8_t,28> reserved;
    std::int32_t pad;
};
static_assert(sizeof(SaveMount1)==80&&sizeof(SaveMountResult)==64);
bool copy_in(void* destination,const void* source,std::size_t size) noexcept {
    if(!source)return false;
    SIZE_T read{};
    return ReadProcessMemory(GetCurrentProcess(),source,destination,size,&read)&&read==size;
}
std::int32_t LBP3_GUEST_ABI savedata_mount(const SaveMount1* input,SaveMountResult* output) noexcept {
    if(!savedata_initialized.load(std::memory_order_acquire))return savedata_not_initialized;
    SaveMount1 request{};SaveDirName dir{};
    if(!copy_in(&request,input,sizeof(request))||!request.dir||!copy_in(&dir,request.dir,sizeof(dir))||!writable(output,sizeof(*output)))return savedata_parameter_error;
    const auto end=std::find(std::begin(dir.data),std::end(dir.data),'\0');
    if(end==std::end(dir.data)||end==std::begin(dir.data))return savedata_parameter_error;
    const std::string_view name(dir.data,static_cast<std::size_t>(end-std::begin(dir.data)));
    if(name=="."||name==".."||name.find('/')!=name.npos||name.find('\\')!=name.npos)return savedata_parameter_error;
    if(request.user!=1)return savedata_invalid_user;
    bool expected=false;
    if(!savedata_mount_active.compare_exchange_strong(expected,true,std::memory_order_acq_rel))return savedata_already_mounted;
    SaveMountResult result{};std::memcpy(result.mount_point,"/savedata0",11);result.status=1;
    if(!copy_out(output,&result,sizeof(result))){savedata_mount_active.store(false,std::memory_order_release);return savedata_parameter_error;}
    return 0;
}
std::int32_t LBP3_GUEST_ABI savedata_unmount(const char* point) noexcept {
    if(!savedata_initialized.load(std::memory_order_acquire))return savedata_not_initialized;
    std::array<char,16> mount{};
    if(!copy_in(mount.data(),point,mount.size()))return savedata_parameter_error;
    if(std::string_view(mount.data())!="/savedata0"||!savedata_mount_active.exchange(false,std::memory_order_acq_rel))return savedata_not_mounted;
    return 0;
}
std::int32_t LBP3_GUEST_ABI sysmodule_is_loaded(std::uint16_t id) noexcept {
    if(!id)return sysmodule_invalid_id;
    std::lock_guard guard(sysmodule_mutex);
    return sysmodule_references[id]?0:sysmodule_not_loaded;
}
std::int32_t LBP3_GUEST_ABI sysmodule_load(std::uint16_t id) noexcept {
    if(!id)return sysmodule_invalid_id;
    std::lock_guard guard(sysmodule_mutex);
    if(sysmodule_references[id]==UINT32_MAX)return sysmodule_busy;
    ++sysmodule_references[id];return 0;
}
std::int32_t LBP3_GUEST_ABI sysmodule_unload(std::uint16_t id) noexcept {
    if(!id)return sysmodule_invalid_id;
    std::lock_guard guard(sysmodule_mutex);
    if(!sysmodule_references[id])return sysmodule_not_loaded;
    --sysmodule_references[id];if(id==0xb4&&!sysmodule_references[id])appcontent_initialized=false;return 0;
}
std::int32_t LBP3_GUEST_ABI appcontent_initialize(const std::uint8_t* init,std::uint8_t* boot) noexcept {
    {std::lock_guard guard(sysmodule_mutex);if(!sysmodule_references[0xb4])return sysmodule_not_loaded;}
    if(!init||!boot)return appcontent_parameter_error;
    std::array<std::uint8_t,32> params{};SIZE_T read_count{};
    if(!ReadProcessMemory(GetCurrentProcess(),init,params.data(),params.size(),&read_count)||read_count!=params.size()||
       std::any_of(params.begin(),params.end(),[](auto byte){return byte!=0;}))return appcontent_parameter_error;
    std::array<std::uint8_t,40> result{};
    {std::lock_guard guard(sysmodule_mutex);if(appcontent_initialized)return appcontent_busy;
        if(!copy_out(boot,result.data(),result.size()))return appcontent_parameter_error;appcontent_initialized=true;}
    return 0;
}
std::int32_t LBP3_GUEST_ABI appcontent_get_parameter(std::uint32_t id,std::int32_t* output) noexcept {
    std::lock_guard guard(sysmodule_mutex);
    if(!sysmodule_references[0xb4]||!appcontent_initialized)return appcontent_parameter_error;
    if(id>=appcontent_parameters.size()||!output)return appcontent_parameter_error;
    const auto value=static_cast<std::int32_t>(appcontent_parameters[id]);
    return copy_out(output,&value,sizeof(value))?0:appcontent_parameter_error;
}
std::int32_t LBP3_GUEST_ABI appcontent_addon_list(std::uint32_t service,void* list,std::uint32_t capacity,std::uint32_t* hits) noexcept {
    std::lock_guard guard(sysmodule_mutex);
    if(!sysmodule_references[0xb4]||!appcontent_initialized)return appcontent_parameter_error;
    if(service||((!capacity||!list)&&!hits))return appcontent_parameter_error;
    const std::uint32_t none{};
    return !hits||copy_out(hits,&none,sizeof(none))?0:appcontent_parameter_error;
}
std::int32_t LBP3_GUEST_ABI appcontent_temp_mount(std::uint32_t,char* mount_point) noexcept {
    if(!mount_point)return appcontent_parameter_error;
    constexpr std::array<char,16> path{'/','t','e','m','p','0','\0'};
    return copy_out(mount_point,path.data(),path.size())?0:appcontent_parameter_error;
}
std::int32_t LBP3_GUEST_ABI appcontent_temp_unmount(const char*) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI appcontent_entitlement_key(const void*,void*) noexcept {return appcontent_no_entitlement;}
std::int32_t LBP3_GUEST_ABI appcontent_temp_available(const char*,std::uint64_t* output) noexcept {
    // The game only needs a bounded capacity estimate to decide whether to
    // create temporary data. Report the ISS-compatible 64 MiB guest quota.
    constexpr std::uint64_t available_kb=64*1024;
    return copy_out(output,&available_kb,sizeof(available_kb))?0:appcontent_parameter_error;
}
std::int32_t LBP3_GUEST_ABI appcontent_temp_format(const char*) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI content_search_ok() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI content_search_count(std::uint32_t* count) noexcept {if(count)*count=0;return 0;}
std::int32_t LBP3_GUEST_ABI content_search_run(const void*,std::uint32_t,std::uint32_t,void*,std::uint32_t* found) noexcept {if(found)*found=0;return 0;}
std::int32_t LBP3_GUEST_ABI content_search_metadata_missing() noexcept {return std::bit_cast<std::int32_t>(0x80d60003u);}
std::int32_t LBP3_GUEST_ABI playgo_initialize(const void*) noexcept {
    // The package is already extracted and installed on the host filesystem;
    // there is no PS4 chunk download/install operation to start here.
    return 0;
}
bool valid_playgo(std::int32_t handle) noexcept {
    std::lock_guard guard(playgo_mutex);return handle&&handle==playgo_handle;
}
std::int32_t LBP3_GUEST_ABI playgo_open(std::int32_t* output,const void*) noexcept {
    if(!output)return playgo_bad_pointer;
    constexpr std::int32_t handle=1;
    if(!copy_out(output,&handle,sizeof(handle)))return playgo_bad_pointer;
    std::lock_guard guard(playgo_mutex);playgo_handle=handle;return 0;
}
std::int32_t LBP3_GUEST_ABI playgo_chunk_ids(std::int32_t handle,std::uint16_t* ids,std::uint32_t count,std::uint32_t* output) noexcept {
    if(!valid_playgo(handle))return playgo_bad_handle;
    if(!output)return playgo_bad_pointer;
    if(ids&&!count)return playgo_bad_size;
    const auto returned=ids?std::min(count,playgo_chunk_count):playgo_chunk_count;
    if(ids){std::array<std::uint16_t,256> values{};for(std::uint32_t i=0;i<returned;++i)values[i]=static_cast<std::uint16_t>(i);
        if(!copy_out(ids,values.data(),returned*sizeof(values[0])))return playgo_bad_pointer;}
    return copy_out(output,&returned,sizeof(returned))?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_locus(std::int32_t handle,const std::uint16_t* ids,std::uint32_t count,std::int8_t* loci) noexcept {
    if(!valid_playgo(handle))return playgo_bad_handle;
    if(!ids||!loci)return playgo_bad_pointer;
    if(!count||count>playgo_chunk_count)return playgo_bad_size;
    std::array<std::uint16_t,256> input{};SIZE_T read_count{};
    if(!ReadProcessMemory(GetCurrentProcess(),ids,input.data(),count*sizeof(input[0]),&read_count)||read_count!=count*sizeof(input[0]))return playgo_bad_pointer;
    std::array<std::int8_t,256> values{};
    for(std::uint32_t i=0;i<count;++i){if(input[i]>=playgo_chunk_count)return playgo_bad_chunk;values[i]=3;}
    return copy_out(loci,values.data(),count)?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_set_speed(std::int32_t handle,std::int32_t) noexcept {return valid_playgo(handle)?0:playgo_bad_handle;}
std::int32_t LBP3_GUEST_ABI playgo_get_speed(std::int32_t handle,std::int32_t* output) noexcept {
    if(!valid_playgo(handle))return playgo_bad_handle;const std::int32_t speed=2;
    return copy_out(output,&speed,sizeof(speed))?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_progress(std::int32_t handle,const std::uint16_t* ids,std::uint32_t count,std::uint64_t* output) noexcept {
    if(!valid_playgo(handle))return playgo_bad_handle;
    if(!ids||!output)return playgo_bad_pointer;
    if(!count||count>playgo_chunk_count)return playgo_bad_size;
    std::array<std::uint16_t,256> input{};SIZE_T read_count{};
    if(!ReadProcessMemory(GetCurrentProcess(),ids,input.data(),count*sizeof(input[0]),&read_count)||read_count!=count*sizeof(input[0]))return playgo_bad_pointer;
    for(std::uint32_t i=0;i<count;++i)if(input[i]>=playgo_chunk_count)return playgo_bad_chunk;
    constexpr std::array<std::uint64_t,2> done{1,1};return copy_out(output,done.data(),sizeof(done))?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_set_todo(std::int32_t handle,const void* list,std::uint32_t count) noexcept {
    if(!valid_playgo(handle))return playgo_bad_handle;if(!list)return playgo_bad_pointer;return count?0:playgo_bad_size;
}
std::int32_t LBP3_GUEST_ABI playgo_get_todo(std::int32_t,std::uint32_t*,std::uint32_t,std::uint32_t* entries) noexcept {
    const std::uint32_t none{};return !entries||copy_out(entries,&none,sizeof(none))?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_eta(std::int32_t,const std::uint16_t*,std::uint32_t,std::uint64_t* output) noexcept {
    const std::uint64_t zero{};return !output||copy_out(output,&zero,sizeof(zero))?0:playgo_bad_pointer;
}
std::int32_t LBP3_GUEST_ABI playgo_language_mask(std::int32_t,std::uint64_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI playgo_close(std::int32_t handle) noexcept {
    std::lock_guard guard(playgo_mutex);if(!handle||handle!=playgo_handle)return playgo_bad_handle;playgo_handle=0;return 0;
}
std::int32_t LBP3_GUEST_ABI playgo_terminate() noexcept {std::lock_guard guard(playgo_mutex);playgo_handle=0;return 0;}
}

std::optional<std::uintptr_t> resolve_system_service(const HostImport& r){
    if(r.type==2&&r.library=="libSceDiscMap"&&r.module=="libSceDiscMap"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="lbQKqsERhtE")return reinterpret_cast<std::uintptr_t>(&discmap_is_request_on_hdd);
        if(r.nid=="ioKMruft1ek")return reinterpret_cast<std::uintptr_t>(&discmap_query);
    }
    if(r.type==2&&r.library=="libSceUserService"&&r.module=="libSceUserService"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="j3YMu1MVNNo")return reinterpret_cast<std::uintptr_t>(&user_service_initialize);
        if(r.nid=="bwFjS+bX9mA")return reinterpret_cast<std::uintptr_t>(&user_service_terminate);
        if(r.nid=="CdWp0oHWGr0")return reinterpret_cast<std::uintptr_t>(&user_service_initial);
        if(r.nid=="fPhymKNvK-A")return reinterpret_cast<std::uintptr_t>(&user_service_list);
        if(r.nid=="1xxcMiGu2fo")return reinterpret_cast<std::uintptr_t>(&user_service_name);
        if(r.nid=="yH17Q6NWtVg")return reinterpret_cast<std::uintptr_t>(&user_service_event);
    }
    if(r.type==2&&r.library=="libSceSysmodule"&&r.module=="libSceSysmodule"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="g8cM39EUZ6o")return reinterpret_cast<std::uintptr_t>(&sysmodule_load);
        if(r.nid=="fMP5NHUOaMk")return reinterpret_cast<std::uintptr_t>(&sysmodule_is_loaded);
        if(r.nid=="eR2bZFAAU0Q")return reinterpret_cast<std::uintptr_t>(&sysmodule_unload);
    }
    if(r.type==2&&r.library=="libSceCommonDialog"&&r.module=="libSceCommonDialog"&&
       r.library_version==1&&r.module_version==0x0101&&r.nid=="uoUpLGNkygk")
        return reinterpret_cast<std::uintptr_t>(&common_dialog_initialize);
    if(r.type==2&&r.library=="libSceSaveData"&&r.module=="libSceSaveData"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="ZkZhskCPXFw")return reinterpret_cast<std::uintptr_t>(&savedata_initialize);
        if(r.nid=="yKDy8S5yLA0")return reinterpret_cast<std::uintptr_t>(&savedata_terminate);
        if(r.nid=="32HQAQdwM2o")return reinterpret_cast<std::uintptr_t>(&savedata_mount);
        if(r.nid=="BMR4F-Uek3E")return reinterpret_cast<std::uintptr_t>(&savedata_unmount);
    }
    if(r.type==2&&r.library=="libSceAppContent"&&r.module=="libSceAppContentUtil"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="R9lA82OraNs")return reinterpret_cast<std::uintptr_t>(&appcontent_initialize);
        if(r.nid=="99b82IKXpH4")return reinterpret_cast<std::uintptr_t>(&appcontent_get_parameter);
        if(r.nid=="xnd8BJzAxmk")return reinterpret_cast<std::uintptr_t>(&appcontent_addon_list);
        if(r.nid=="buYbeLOGWmA")return reinterpret_cast<std::uintptr_t>(&appcontent_temp_mount);
        if(r.nid=="bcolXMmp6qQ")return reinterpret_cast<std::uintptr_t>(&appcontent_temp_unmount);
        if(r.nid=="XTWR0UXvcgs")return reinterpret_cast<std::uintptr_t>(&appcontent_entitlement_key);
        if(r.nid=="SaKib2Ug0yI")return reinterpret_cast<std::uintptr_t>(&appcontent_temp_available);
        if(r.nid=="a5N7lAG0y2Q")return reinterpret_cast<std::uintptr_t>(&appcontent_temp_format);
    }
    if(r.type==2&&r.library=="libSceContentSearch"&&r.module=="libSceContentSearch"&&
       r.library_version==1&&r.module_version==0x0100){
        if(r.nid=="dPj4ZtRcIWk"||r.nid=="1xSZodB2geA"||r.nid=="-YbpaF0XS-I")return reinterpret_cast<std::uintptr_t>(&content_search_ok);
        // Static call-site audit: LBP3 passes a single word-output pointer,
        // then queries its application index and searches. The installed
        // content catalog is empty in this offline runtime, so report zero.
        if(r.nid=="PD5bnBTsv6Y")return reinterpret_cast<std::uintptr_t>(&content_search_count);
        if(r.nid=="o-RBPV0qr8c")return reinterpret_cast<std::uintptr_t>(&content_search_count);
        if(r.nid=="TEW3IKxYfXc")return reinterpret_cast<std::uintptr_t>(&content_search_run);
        if(r.nid=="FRT4EYtZU1Y")return reinterpret_cast<std::uintptr_t>(&content_search_count);
        if(r.nid=="bjAlYWwRTJA"||r.nid=="ruNe-FgCzO8")return reinterpret_cast<std::uintptr_t>(&content_search_metadata_missing);
    }
    if(r.type==2&&r.library=="libScePlayGo"&&r.module=="libScePlayGo"&&
       r.library_version==1&&r.module_version==0x0100){
        if(r.nid=="ts6GlZOKRrE")return reinterpret_cast<std::uintptr_t>(&playgo_initialize);
        if(r.nid=="M1Gma1ocrGE")return reinterpret_cast<std::uintptr_t>(&playgo_open);
        if(r.nid=="73fF1MFU8hA")return reinterpret_cast<std::uintptr_t>(&playgo_chunk_ids);
        if(r.nid=="uWIYLFkkwqk")return reinterpret_cast<std::uintptr_t>(&playgo_locus);
        if(r.nid=="4AAcTU9R3XM")return reinterpret_cast<std::uintptr_t>(&playgo_set_speed);
        if(r.nid=="rvBSfTimejE")return reinterpret_cast<std::uintptr_t>(&playgo_get_speed);
        if(r.nid=="-RJWNMK3fC8")return reinterpret_cast<std::uintptr_t>(&playgo_progress);
        if(r.nid=="gUPGiOQ1tmQ")return reinterpret_cast<std::uintptr_t>(&playgo_set_todo);
        if(r.nid=="Nn7zKwnA5q0")return reinterpret_cast<std::uintptr_t>(&playgo_get_todo);
        if(r.nid=="v6EZ-YWRdMs")return reinterpret_cast<std::uintptr_t>(&playgo_eta);
        if(r.nid=="LosLlHOpNqQ")return reinterpret_cast<std::uintptr_t>(&playgo_language_mask);
        if(r.nid=="Uco1I0dlDi8")return reinterpret_cast<std::uintptr_t>(&playgo_close);
        if(r.nid=="MPe0EeBGM-E")return reinterpret_cast<std::uintptr_t>(&playgo_terminate);
    }
    if(r.type!=2||r.library!="libSceSystemService"||r.module!="libSceSystemService"||
       r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="rPo6tV8D9bM")return reinterpret_cast<std::uintptr_t>(&system_status);
    if(r.nid=="1n37q1Bvc5Y")return reinterpret_cast<std::uintptr_t>(&safe_area);
    if(r.nid=="656LMQSrg6U")return reinterpret_cast<std::uintptr_t>(&receive_event);
    if(r.nid=="Vo5V8KAwCmk")return reinterpret_cast<std::uintptr_t>(&hide_splash);
    if(r.nid=="fZo48un7LK4")return reinterpret_cast<std::uintptr_t>(&parameter_get_int);
    return std::nullopt;
}

void system_service_self_test(){
    using Status=std::int32_t (LBP3_GUEST_ABI *)(void*);
    using SafeArea=std::int32_t (LBP3_GUEST_ABI *)(void*);
    using Receive=std::int32_t (LBP3_GUEST_ABI *)(void*);
    using Hide=std::int32_t (LBP3_GUEST_ABI *)();
    using Param=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t*);
    const auto get=[](const char* nid){
        return resolve_system_service({nid,"libSceSystemService","libSceSystemService",1,0x0101,2});
    };
    const auto status=reinterpret_cast<Status>(get("rPo6tV8D9bM").value());
    const auto area=reinterpret_cast<SafeArea>(get("1n37q1Bvc5Y").value());
    const auto receive=reinterpret_cast<Receive>(get("656LMQSrg6U").value());
    const auto hide=reinterpret_cast<Hide>(get("Vo5V8KAwCmk").value());
    const auto param=reinterpret_cast<Param>(get("fZo48un7LK4").value());
    std::array<std::byte,133> bytes;bytes.fill(std::byte{0xff});
    if(status(bytes.data())||std::any_of(bytes.begin(),bytes.begin()+12,[](auto b){return b!=std::byte{};})||
       bytes[12]!=std::byte{0xff}||status(reinterpret_cast<void*>(1))!=parameter_error)
        throw std::runtime_error("System service status contract failed.");
    bytes.fill(std::byte{0xff});float ratio{};
    if(area(bytes.data()))throw std::runtime_error("System service safe-area call failed.");
    std::memcpy(&ratio,bytes.data(),sizeof(ratio));
    if(ratio!=1.0f||
       std::any_of(bytes.begin()+4,bytes.begin()+132,[](auto b){return b!=std::byte{};})||
       bytes[132]!=std::byte{0xff}||area(reinterpret_cast<void*>(1))!=parameter_error)
        throw std::runtime_error("System service safe-area contract failed.");
    if(receive(nullptr)!=no_event||hide()!=0)throw std::runtime_error("System service event/splash contract failed.");
    for(const auto [id,expected]:std::array<std::pair<int,int>,6>{{{1,1},{2,1},{3,1},{5,0},{7,0},{1000,1}}}){
        int value=-1;if(param(id,&value)||value!=expected)throw std::runtime_error("System service parameter contract failed.");
    }
    int timezone{};if(param(4,&timezone)||timezone < -720||timezone > 840)
        throw std::runtime_error("System service time-zone parameter contract failed.");
    int unchanged=42;
    if(param(999,&unchanged)!=parameter_error||unchanged!=42||param(1,reinterpret_cast<int*>(1))!=parameter_error)
        throw std::runtime_error("System service invalid parameter was not rejected.");
    auto wrong=HostImport{"rPo6tV8D9bM","libSceSystemService","libSceSystemService",1,0x0101,1};
    if(resolve_system_service(wrong))throw std::runtime_error("System service NID resolved as data.");
    using ContentInit=std::int32_t (LBP3_GUEST_ABI *)();
    using ContentCount=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t*);
    const auto content_init=resolve_system_service({"dPj4ZtRcIWk","libSceContentSearch","libSceContentSearch",1,0x0100,2});
    const auto content_count=resolve_system_service({"o-RBPV0qr8c","libSceContentSearch","libSceContentSearch",1,0x0100,2});
    const auto content_query_count=resolve_system_service({"PD5bnBTsv6Y","libSceContentSearch","libSceContentSearch",1,0x0100,2});
    const auto content_data=resolve_system_service({"o-RBPV0qr8c","libSceContentSearch","libSceContentSearch",1,0x0100,1});
    std::uint32_t content_items=9,query_items=9;
    if(!content_init||reinterpret_cast<ContentInit>(*content_init)()!=0||!content_count||
       reinterpret_cast<ContentCount>(*content_count)(&content_items)!=0||content_items!=0||content_data||
       !content_query_count||reinterpret_cast<ContentCount>(*content_query_count)(&query_items)!=0||query_items!=0||
       resolve_system_service({"dPj4ZtRcIWk","libSceContentSearch","libSceContentSearch",1,0x0101,2}))
        throw std::runtime_error("Offline content search no-installed-content contract failed.");
    const auto discmap=resolve_system_service({"lbQKqsERhtE","libSceDiscMap","libSceDiscMap",1,0x0101,2});
    const auto discmap_query_address=resolve_system_service({"ioKMruft1ek","libSceDiscMap","libSceDiscMap",1,0x0101,2});
    using DiscMap=std::int32_t (LBP3_GUEST_ABI *)(const char*,std::int64_t,std::int64_t,std::int32_t*);
    using DiscMapQuery=std::int32_t (LBP3_GUEST_ABI *)(const char*,std::int64_t,std::int64_t,std::int32_t*,std::int32_t*,std::int32_t*);
    if(!discmap||reinterpret_cast<DiscMap>(*discmap)(nullptr,-1,-1,nullptr)!=discmap_no_bitmap||
       !discmap_query_address||reinterpret_cast<DiscMapQuery>(*discmap_query_address)(nullptr,-1,-1,nullptr,nullptr,nullptr)!=discmap_no_bitmap||
       resolve_system_service({"lbQKqsERhtE","libSceDiscMap","libSceDiscMap",1,0x0101,1})||
       resolve_system_service({"ioKMruft1ek","libSceDiscMap","libSceDiscMap",1,0x0101,1}))
        throw std::runtime_error("DiscMap no-bitmap contract failed.");
    const auto user_init=resolve_system_service({"j3YMu1MVNNo","libSceUserService","libSceUserService",1,0x0101,2});
    const auto user_term=resolve_system_service({"bwFjS+bX9mA","libSceUserService","libSceUserService",1,0x0101,2});
    using UserInit=std::int32_t (LBP3_GUEST_ABI *)(const void*);
    using UserTerm=std::int32_t (LBP3_GUEST_ABI *)();
    if(!user_init||!user_term||reinterpret_cast<UserInit>(*user_init)(nullptr)!=0||reinterpret_cast<UserTerm>(*user_term)()!=0||
       resolve_system_service({"j3YMu1MVNNo","libSceUserService","libSceUserService",1,0x0101,1}))
        throw std::runtime_error("Offline user-service initialization contract failed.");
    const auto initial_user=resolve_system_service({"CdWp0oHWGr0","libSceUserService","libSceUserService",1,0x0101,2});
    using UserInitial=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*);
    std::int32_t user_id{};
    if(!initial_user||reinterpret_cast<UserInitial>(*initial_user)(&user_id)||user_id!=1||
       reinterpret_cast<UserInitial>(*initial_user)(nullptr)!=user_invalid_argument)
        throw std::runtime_error("Offline initial-user contract failed.");
    const auto users=resolve_system_service({"fPhymKNvK-A","libSceUserService","libSceUserService",1,0x0101,2});
    std::array<std::int32_t,4> user_ids{};
    if(!users||reinterpret_cast<UserInitial>(*users)(user_ids.data())||user_ids!=std::array<std::int32_t,4>{1,-1,-1,-1})
        throw std::runtime_error("Offline login-user list contract failed.");
    const auto user_name=resolve_system_service({"1xxcMiGu2fo","libSceUserService","libSceUserService",1,0x0101,2});
    const auto user_event=resolve_system_service({"yH17Q6NWtVg","libSceUserService","libSceUserService",1,0x0101,2});
    using UserName=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,char*,std::uint64_t);
    std::array<char,7> name{};std::array<std::int32_t,2> event{};
    if(!user_name||!user_event||reinterpret_cast<UserName>(*user_name)(1,name.data(),name.size())||
       std::string_view(name.data())!="Hunter"||reinterpret_cast<UserName>(*user_name)(1,name.data(),1)!=user_name_too_short||
       reinterpret_cast<UserInitial>(*user_event)(event.data())||event!=std::array<std::int32_t,2>{0,1}||
       reinterpret_cast<UserInitial>(*user_event)(event.data())!=user_no_event)
        throw std::runtime_error("Offline user name/login-event contract failed.");
    const auto loaded=resolve_system_service({"fMP5NHUOaMk","libSceSysmodule","libSceSysmodule",1,0x0101,2});
    using IsLoaded=std::int32_t (LBP3_GUEST_ABI *)(std::uint16_t);
    const auto load=resolve_system_service({"g8cM39EUZ6o","libSceSysmodule","libSceSysmodule",1,0x0101,2});
    const auto unload=resolve_system_service({"eR2bZFAAU0Q","libSceSysmodule","libSceSysmodule",1,0x0101,2});
    using ModuleOperation=std::int32_t (LBP3_GUEST_ABI *)(std::uint16_t);
    const auto loaded_fn=loaded?reinterpret_cast<IsLoaded>(*loaded):nullptr;
    const auto load_fn=load?reinterpret_cast<ModuleOperation>(*load):nullptr;
    const auto unload_fn=unload?reinterpret_cast<ModuleOperation>(*unload):nullptr;
    constexpr std::uint16_t synthetic_module=0x7ffe;
    if(!loaded_fn||!load_fn||!unload_fn||loaded_fn(0)!=sysmodule_invalid_id||
       loaded_fn(synthetic_module)!=sysmodule_not_loaded||load_fn(0)!=sysmodule_invalid_id||
       load_fn(synthetic_module)!=0||loaded_fn(synthetic_module)!=0||unload_fn(synthetic_module)!=0||
       loaded_fn(synthetic_module)!=sysmodule_not_loaded||unload_fn(synthetic_module)!=sysmodule_not_loaded||
       resolve_system_service({"fMP5NHUOaMk","libSceSysmodule","libSceSysmodule",1,0x0101,1}))
        throw std::runtime_error("Sysmodule unloaded-state contract failed.");
    using AppInit=std::int32_t (LBP3_GUEST_ABI *)(const std::uint8_t*,std::uint8_t*);
    using AppParam=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t,std::int32_t*);
    using AppList=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t,void*,std::uint32_t,std::uint32_t*);
    const auto app_init=resolve_system_service({"R9lA82OraNs","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto app_param=resolve_system_service({"99b82IKXpH4","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto app_list=resolve_system_service({"xnd8BJzAxmk","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    std::array<std::uint8_t,32> app_init_data{};std::array<std::uint8_t,40> app_boot;app_boot.fill(0xff);
    std::int32_t app_value=-1;std::uint32_t addon_count=99;
    const auto app_init_fn=app_init?reinterpret_cast<AppInit>(*app_init):nullptr;
    const auto app_param_fn=app_param?reinterpret_cast<AppParam>(*app_param):nullptr;
    const auto app_list_fn=app_list?reinterpret_cast<AppList>(*app_list):nullptr;
    if(!app_init_fn||!app_param_fn||!app_list_fn||app_init_fn(app_init_data.data(),app_boot.data())!=sysmodule_not_loaded||
       load_fn(0xb4)||app_init_fn(app_init_data.data(),app_boot.data())||
       std::any_of(app_boot.begin(),app_boot.end(),[](auto byte){return byte!=0;})||
       app_init_fn(app_init_data.data(),app_boot.data())!=appcontent_busy||app_param_fn(0,&app_value)||app_value!=1||
       app_list_fn(0,nullptr,0,&addon_count)||addon_count||app_list_fn(1,nullptr,0,&addon_count)!=appcontent_parameter_error||
       unload_fn(0xb4)||app_param_fn(0,&app_value)!=appcontent_parameter_error)
        throw std::runtime_error("AppContent base-game initialization contract failed.");
    appcontent_initialized=false;
    using TempMount=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t,char*);
    using TempUnmount=std::int32_t (LBP3_GUEST_ABI *)(const char*);
    using Entitlement=std::int32_t (LBP3_GUEST_ABI *)(const void*,void*);
    using TempAvailable=std::int32_t (LBP3_GUEST_ABI *)(const char*,std::uint64_t*);
    using TempFormat=std::int32_t (LBP3_GUEST_ABI *)(const char*);
    const auto temp_mount=resolve_system_service({"buYbeLOGWmA","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto temp_unmount=resolve_system_service({"bcolXMmp6qQ","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto entitlement=resolve_system_service({"XTWR0UXvcgs","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto temp_available=resolve_system_service({"SaKib2Ug0yI","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    const auto temp_format=resolve_system_service({"a5N7lAG0y2Q","libSceAppContent","libSceAppContentUtil",1,0x0101,2});
    std::array<char,16> mount{};
    std::uint64_t available_kb{};
    if(!temp_mount||!temp_unmount||!entitlement||!temp_available||!temp_format||
       reinterpret_cast<TempMount>(*temp_mount)(0,mount.data())||std::string_view(mount.data())!="/temp0"||
       reinterpret_cast<TempMount>(*temp_mount)(0,nullptr)!=appcontent_parameter_error||
       reinterpret_cast<TempUnmount>(*temp_unmount)(mount.data())||
       reinterpret_cast<Entitlement>(*entitlement)(nullptr,nullptr)!=appcontent_no_entitlement||
       reinterpret_cast<TempAvailable>(*temp_available)(mount.data(),&available_kb)||available_kb!=64*1024||
       reinterpret_cast<TempAvailable>(*temp_available)(mount.data(),nullptr)!=appcontent_parameter_error||
       reinterpret_cast<TempFormat>(*temp_format)(mount.data()))
        throw std::runtime_error("AppContent temporary mount/entitlement offline contract failed.");
    const auto common=resolve_system_service({"uoUpLGNkygk","libSceCommonDialog","libSceCommonDialog",1,0x0101,2});
    using CommonInit=std::int32_t (LBP3_GUEST_ABI *)();
    if(!common||reinterpret_cast<CommonInit>(*common)()!=0||
       resolve_system_service({"uoUpLGNkygk","libSceCommonDialog","libSceCommonDialog",1,0x0101,1}))
        throw std::runtime_error("Headless common-dialog initialization failed.");
    const auto save_init=resolve_system_service({"ZkZhskCPXFw","libSceSaveData","libSceSaveData",1,0x0101,2});
    const auto save_term=resolve_system_service({"yKDy8S5yLA0","libSceSaveData","libSceSaveData",1,0x0101,2});
    const auto save_mount=resolve_system_service({"32HQAQdwM2o","libSceSaveData","libSceSaveData",1,0x0101,2});
    const auto save_unmount=resolve_system_service({"BMR4F-Uek3E","libSceSaveData","libSceSaveData",1,0x0101,2});
    using SaveInit=std::int32_t (LBP3_GUEST_ABI *)(const void*);
    using SaveTerm=std::int32_t (LBP3_GUEST_ABI *)();
    using SaveMount=std::int32_t (LBP3_GUEST_ABI *)(const SaveMount1*,SaveMountResult*);
    using SaveUnmount=std::int32_t (LBP3_GUEST_ABI *)(const char*);
    SaveDirName savedir{};std::memcpy(savedir.data,"LBP3",5);
    SaveMount1 mount_request{};mount_request.user=1;mount_request.dir=&savedir;mount_request.mode=4;
    SaveMountResult mount_result{};
    if(!save_init||!save_term||!save_mount||!save_unmount||reinterpret_cast<SaveInit>(*save_init)(nullptr)!=0||
       reinterpret_cast<SaveMount>(*save_mount)(&mount_request,&mount_result)||
       std::string_view(mount_result.mount_point)!="/savedata0"||mount_result.status!=1||
       reinterpret_cast<SaveMount>(*save_mount)(&mount_request,&mount_result)!=savedata_already_mounted||
       reinterpret_cast<SaveUnmount>(*save_unmount)(mount_result.mount_point)||
       reinterpret_cast<SaveUnmount>(*save_unmount)(mount_result.mount_point)!=savedata_not_mounted||
       reinterpret_cast<SaveMount>(*save_mount)(nullptr,&mount_result)!=savedata_parameter_error||
       reinterpret_cast<SaveTerm>(*save_term)()!=0||reinterpret_cast<SaveTerm>(*save_term)()!=savedata_not_initialized||
       resolve_system_service({"ZkZhskCPXFw","libSceSaveData","libSceSaveData",1,0x0101,1}))
        throw std::runtime_error("SaveData offline mount/lifecycle contract failed.");
    const auto playgo=resolve_system_service({"ts6GlZOKRrE","libScePlayGo","libScePlayGo",1,0x0100,2});
    using PlayGoInit=std::int32_t (LBP3_GUEST_ABI *)(const void*);
    if(!playgo||reinterpret_cast<PlayGoInit>(*playgo)(nullptr)!=0||
       resolve_system_service({"ts6GlZOKRrE","libScePlayGo","libScePlayGo",1,0x0101,2})||
       resolve_system_service({"ts6GlZOKRrE","libScePlayGo","libScePlayGo",1,0x0100,1}))
        throw std::runtime_error("PlayGo installed-package initialization contract failed.");
    const auto pg=[](const char* nid){return resolve_system_service({nid,"libScePlayGo","libScePlayGo",1,0x0100,2});};
    using PlayGoOpen=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*,const void*);
    using PlayGoIds=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::uint16_t*,std::uint32_t,std::uint32_t*);
    using PlayGoLocus=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const std::uint16_t*,std::uint32_t,std::int8_t*);
    using PlayGoProgress=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const std::uint16_t*,std::uint32_t,std::uint64_t*);
    using PlayGoSpeed=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t*);
    using PlayGoClose=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    const auto open=pg("M1Gma1ocrGE"),ids=pg("73fF1MFU8hA"),locus=pg("uWIYLFkkwqk"),
        progress=pg("-RJWNMK3fC8"),speed=pg("rvBSfTimejE"),close=pg("Uco1I0dlDi8");
    std::int32_t handle{},speed_value{};std::uint32_t id_count{};std::array<std::uint16_t,2> chunks{0,1};
    std::array<std::int8_t,2> loci{};std::array<std::uint64_t,2> progress_values{};
    if(!open||!ids||!locus||!progress||!speed||!close||
       reinterpret_cast<PlayGoOpen>(*open)(&handle,nullptr)||handle!=1||
       reinterpret_cast<PlayGoIds>(*ids)(handle,nullptr,0,&id_count)||id_count!=256||
       reinterpret_cast<PlayGoLocus>(*locus)(handle,chunks.data(),2,loci.data())||loci[0]!=3||loci[1]!=3||
       reinterpret_cast<PlayGoProgress>(*progress)(handle,chunks.data(),2,progress_values.data())||progress_values[0]!=1||progress_values[1]!=1||
       reinterpret_cast<PlayGoSpeed>(*speed)(handle,&speed_value)||speed_value!=2||
       reinterpret_cast<PlayGoClose>(*close)(handle)||reinterpret_cast<PlayGoClose>(*close)(handle)!=playgo_bad_handle)
        throw std::runtime_error("PlayGo installed-chunk contract failed.");
}
}
