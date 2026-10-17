#include "lbp3_host/clock_services.hpp"
#include "lbp3_host/filesystem_exports.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <bit>
#include <chrono>
#include <limits>

namespace lbp3::hle {
namespace {
const auto tsc_epoch=std::chrono::steady_clock::now();
std::uint64_t monotonic_counter() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-tsc_epoch).count());
}
std::uint64_t LBP3_GUEST_ABI tsc_frequency() noexcept {return 1000000000ull;}
std::uint64_t LBP3_GUEST_ABI read_tsc() noexcept {return monotonic_counter();}
struct Time {std::int64_t seconds{},fraction{};};
struct Zone {std::int32_t minutes_west{},dst{};};
static_assert(sizeof(Time)==16&&sizeof(Zone)==8);
template<class T> bool writable(T* pointer){
    auto at=reinterpret_cast<std::uintptr_t>(pointer);if(!at||at>UINTPTR_MAX-sizeof(T))return false;
    const auto end=at+sizeof(T);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }return true;
}
template<class T> bool write(T* pointer,const T& value){SIZE_T n{};
    return writable(pointer)&&WriteProcessMemory(GetCurrentProcess(),pointer,&value,sizeof(value),&n)&&n==sizeof(value);}
// SceRtc ticks are microseconds from the Gregorian epoch (0001-01-01).
// Windows FILETIME has the same epoch with 100 ns units.
std::int32_t LBP3_GUEST_ABI rtc_current_tick(std::uint64_t* output) noexcept {
    if(!output||!writable(output))return -1;
    FILETIME file{};GetSystemTimePreciseAsFileTime(&file);
    const auto file_ticks=(static_cast<std::uint64_t>(file.dwHighDateTime)<<32)|file.dwLowDateTime;
    constexpr std::uint64_t file_epoch_to_gregorian=504911232000000000ull;
    if(file_ticks>UINT64_MAX-file_epoch_to_gregorian)return -1;
    const auto ticks=(file_ticks+file_epoch_to_gregorian)/10;
    return write(output,ticks)?0:-1;
}
unsigned get_time(std::uint32_t id,Time& value){
    if(id==0||id==9||id==10||id==13){
        FILETIME file{};GetSystemTimePreciseAsFileTime(&file);
        const auto ticks=(static_cast<std::uint64_t>(file.dwHighDateTime)<<32)|file.dwLowDateTime;
        constexpr std::uint64_t unix_epoch=116444736000000000ull;
        if(ticks<unix_epoch)return 5;
        const auto elapsed=ticks-unix_epoch;value.seconds=static_cast<std::int64_t>(elapsed/10000000);
        value.fraction=id==13?0:static_cast<std::int64_t>((elapsed%10000000)*100);return 0;
    }
    if(id==4||id==11||id==12){
        LARGE_INTEGER ticks{},frequency{};
        if(!QueryPerformanceCounter(&ticks)||!QueryPerformanceFrequency(&frequency)||ticks.QuadPart<0||frequency.QuadPart<=0||
           frequency.QuadPart>std::numeric_limits<std::int64_t>::max()/1000000000)return 5;
        value.seconds=ticks.QuadPart/frequency.QuadPart;
        value.fraction=(ticks.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart;return 0;
    }
    if(id==14||id==15){
        FILETIME created{},exited{},kernel{},user{};
        const bool ok=id==14?GetThreadTimes(GetCurrentThread(),&created,&exited,&kernel,&user):GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
        if(!ok)return 5;
        const auto k=(static_cast<std::uint64_t>(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime;
        const auto u=(static_cast<std::uint64_t>(user.dwHighDateTime)<<32)|user.dwLowDateTime;
        if(k>UINT64_MAX-u)return 5;const auto total=k+u;
        value.seconds=static_cast<std::int64_t>(total/10000000);value.fraction=static_cast<std::int64_t>(total%10000000*100);return 0;
    }
    // Uptime and external clocks need distinct suspend/network semantics.
    return id<=19?45u:22u;
}
std::int32_t result(unsigned error,bool kernel){
    if(!error)return 0;
    if(kernel)return std::bit_cast<std::int32_t>(0x80020000u|error);
    *guest_errno_address()=static_cast<std::int32_t>(error);return -1;
}
std::int32_t clock_impl(std::uint32_t id,Time* output,bool kernel){
    Time value{};const auto error=get_time(id,value);
    return result(error?error:(write(output,value)?0:14),kernel);
}
std::int32_t LBP3_GUEST_ABI kernel_clock(std::uint32_t id,Time* output) noexcept {return clock_impl(id,output,true);}
std::int32_t LBP3_GUEST_ABI posix_clock(std::uint32_t id,Time* output) noexcept {return clock_impl(id,output,false);}
std::int32_t LBP3_GUEST_ABI kernel_day(Time* output) noexcept {
    Time value{};const auto error=get_time(0,value);value.fraction/=1000;
    return result(error?error:(write(output,value)?0:14),true);
}
unsigned timezone(Zone& zone){TIME_ZONE_INFORMATION information{};
    const auto state=GetTimeZoneInformation(&information);if(state==TIME_ZONE_ID_INVALID)return 5;
    zone.minutes_west=information.Bias;
    if(state==TIME_ZONE_ID_DAYLIGHT)zone.minutes_west+=information.DaylightBias;
    else if(state==TIME_ZONE_ID_STANDARD)zone.minutes_west+=information.StandardBias;
    zone.dst=state==TIME_ZONE_ID_DAYLIGHT?1:0;return 0;
}
std::int32_t LBP3_GUEST_ABI kernel_zone(Zone* output) noexcept {Zone zone{};const auto error=timezone(zone);return result(error?error:(write(output,zone)?0:14),true);}
std::int32_t LBP3_GUEST_ABI posix_day(Time* output,Zone* zone_output) noexcept {
    if((output&&!writable(output))||(zone_output&&!writable(zone_output)))return result(14,false);
    Time value{};Zone zone{};auto error=get_time(0,value);value.fraction/=1000;
    if(!error&&zone_output)error=timezone(zone);
    if(!error&&output&&!write(output,value))error=14;
    if(!error&&zone_output&&!write(zone_output,zone))error=14;
    return result(error,false);
}
std::int32_t LBP3_GUEST_ABI process_id() noexcept {return static_cast<std::int32_t>(GetCurrentProcessId());}
std::int32_t LBP3_GUEST_ABI posix_sleep(const Time* request,Time* remaining) noexcept {
    Time duration{};SIZE_T copied{};
    if(!request||!ReadProcessMemory(GetCurrentProcess(),request,&duration,sizeof(duration),&copied)||copied!=sizeof(duration))return result(14,false);
    if(duration.seconds<0||duration.fraction<0||duration.fraction>=1000000000)return result(22,false);
    if(remaining&&!writable(remaining))return result(14,false);
    const auto subsecond=(duration.fraction+99)/100;
    if(duration.seconds>(INT64_MAX-subsecond)/10000000)return result(22,false);
    const auto ticks=duration.seconds*10000000+subsecond;
    if(ticks){
        HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer&&GetLastError()==ERROR_INVALID_PARAMETER)
            timer=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_MODIFY_STATE|SYNCHRONIZE);
        if(!timer)return result(5,false);
        LARGE_INTEGER deadline{};deadline.QuadPart=-ticks;
        const bool armed=SetWaitableTimer(timer,&deadline,0,nullptr,nullptr,FALSE)!=0;
        const auto waited=armed?WaitForSingleObject(timer,INFINITE):WAIT_FAILED;
        CloseHandle(timer);if(!armed||waited!=WAIT_OBJECT_0)return result(5,false);
    }
    // Signal delivery is not enabled yet; no supported path interrupts this wait.
    if(remaining&&!write(remaining,Time{}))return result(14,false);
    return 0;
}
}
std::optional<std::uintptr_t> resolve_clock(const HostImport& r){
    if(r.type==2&&r.library=="libSceRtc"&&r.module=="libSceRtc"&&r.library_version==1&&r.module_version==0x0101&&r.nid=="18B2NS1y9UU")
        return reinterpret_cast<std::uintptr_t>(&rtc_current_tick);
    if(r.type!=2||r.module!="libkernel"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.library=="libkernel"){
        if(r.nid=="1j3S3n-tTW4")return reinterpret_cast<std::uintptr_t>(&tsc_frequency);
        if(r.nid=="-2IRUCO--PM")return reinterpret_cast<std::uintptr_t>(&read_tsc);
        if(r.nid=="QBi7HCK03hw")return reinterpret_cast<std::uintptr_t>(&kernel_clock);
        if(r.nid=="ejekcaNQNq0")return reinterpret_cast<std::uintptr_t>(&kernel_day);
        if(r.nid=="kOcnerypnQA")return reinterpret_cast<std::uintptr_t>(&kernel_zone);
    }
    if(r.library!="libkernel"&&r.library!="libScePosix")return std::nullopt;
    if(r.nid=="lLMT9vJAck0")return reinterpret_cast<std::uintptr_t>(&posix_clock);
    if(r.nid=="n88vx3C5nW8")return reinterpret_cast<std::uintptr_t>(&posix_day);
    if(r.nid=="HoLVWNanBBc")return reinterpret_cast<std::uintptr_t>(&process_id);
    if(r.nid=="yS8U2TGCe1A")return reinterpret_cast<std::uintptr_t>(&posix_sleep);
    return std::nullopt;
}
}
