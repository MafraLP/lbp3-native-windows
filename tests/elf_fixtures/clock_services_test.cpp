#include "lbp3_host/clock_services.hpp"
#include "lbp3_host/hle.hpp"
#include "lbp3_host/filesystem_exports.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
struct Time {std::int64_t seconds{},fraction{};};
struct Zone {std::int32_t west{},dst{};};
using Clock=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t,Time*);
void check(bool value){if(!value)throw std::runtime_error("clock services assertion failed");}
auto resolve(const char* nid,const char* library="libkernel"){return lbp3::hle::resolve({nid,library,"libkernel",1,0x0101,2});}
auto resolve_rtc(const char* nid){return lbp3::hle::resolve({nid,"libSceRtc","libSceRtc",1,0x0101,2});}
int main(){try{
    lbp3::hle::initialize();
    auto clock=reinterpret_cast<Clock>(resolve("QBi7HCK03hw").value());
    auto posix=reinterpret_cast<Clock>(resolve("lLMT9vJAck0","libScePosix").value());
    Time real{},mono{},later{};
    check(clock(0,&real)==0&&real.seconds>1700000000&&real.fraction>=0&&real.fraction<1000000000);
    check(clock(4,&mono)==0);Sleep(2);check(clock(4,&later)==0);
    check(later.seconds>mono.seconds||(later.seconds==mono.seconds&&later.fraction>=mono.fraction));
    for(auto id:{9,10,11,12,14,15}){Time value{};check(clock(id,&value)==0&&value.seconds>=0&&value.fraction>=0&&value.fraction<1000000000);}
    Time seconds{};check(clock(13,&seconds)==0&&seconds.fraction==0);
    check(clock(4,reinterpret_cast<Time*>(1))==std::bit_cast<std::int32_t>(0x8002000eu));
    check(posix(99,&real)==-1&&*lbp3::hle::guest_errno_address()==22);
    check(posix(4,nullptr)==-1&&*lbp3::hle::guest_errno_address()==14);
    check(posix(16,&real)==-1&&*lbp3::hle::guest_errno_address()==45);
    const auto parent_errno=*lbp3::hle::guest_errno_address();
    std::thread worker([&]{Time t{};check(posix(99,&t)==-1&&*lbp3::hle::guest_errno_address()==22);});worker.join();
    check(*lbp3::hle::guest_errno_address()==parent_errno);
    using Day=std::int32_t (LBP3_GUEST_ABI *)(Time*);
    auto day=reinterpret_cast<Day>(resolve("ejekcaNQNq0").value());
    Time value{};check(day(&value)==0&&value.fraction>=0&&value.fraction<1000000);
    using PosixDay=std::int32_t (LBP3_GUEST_ABI *)(Time*,Zone*);
    auto posix_day=reinterpret_cast<PosixDay>(resolve("n88vx3C5nW8").value());
    Zone zone{};check(posix_day(&value,&zone)==0&&zone.west>=-1440&&zone.west<=1440&&(zone.dst==0||zone.dst==1));
    check(posix_day(nullptr,nullptr)==0);
    using Counter=std::uint64_t (LBP3_GUEST_ABI *)();
    auto tsc_frequency=reinterpret_cast<Counter>(resolve("1j3S3n-tTW4").value());
    auto read_tsc=reinterpret_cast<Counter>(resolve("-2IRUCO--PM").value());
    check(tsc_frequency()==1000000000ull);const auto t0=read_tsc();Sleep(1);check(read_tsc()>=t0);
    auto* read_only=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);check(read_only);
    DWORD previous{};check(VirtualProtect(read_only,4096,PAGE_READONLY,&previous));
    check(clock(4,static_cast<Time*>(read_only))==std::bit_cast<std::int32_t>(0x8002000eu));
    VirtualFree(read_only,0,MEM_RELEASE);
    using Pid=std::int32_t (LBP3_GUEST_ABI *)();
    check(static_cast<DWORD>(reinterpret_cast<Pid>(resolve("HoLVWNanBBc").value())())==GetCurrentProcessId());
    using SleepFn=std::int32_t (LBP3_GUEST_ABI *)(const Time*,Time*);
    auto sleep=reinterpret_cast<SleepFn>(resolve("yS8U2TGCe1A").value());
    Time request{0,2000000},remaining{99,99};
    const auto start=std::chrono::steady_clock::now();check(sleep(&request,&remaining)==0);
    check(std::chrono::steady_clock::now()-start>=std::chrono::milliseconds(2)&&remaining.seconds==0&&remaining.fraction==0);
    request={0,0};check(sleep(&request,&request)==0&&request.seconds==0&&request.fraction==0);
    request={-1,0};check(sleep(&request,nullptr)==-1&&*lbp3::hle::guest_errno_address()==22);
    request={0,1000000000};check(sleep(&request,nullptr)==-1&&*lbp3::hle::guest_errno_address()==22);
    request={INT64_MAX,0};check(sleep(&request,nullptr)==-1&&*lbp3::hle::guest_errno_address()==22);
    request={0,0};check(sleep(&request,reinterpret_cast<Time*>(1))==-1&&*lbp3::hle::guest_errno_address()==14);
    check(sleep(reinterpret_cast<const Time*>(1),nullptr)==-1&&*lbp3::hle::guest_errno_address()==14);
    check(!lbp3::hle::resolve_clock({"QBi7HCK03hw","libScePosix","libkernel",1,0x0101,2}));
    check(!lbp3::hle::resolve_clock({"lLMT9vJAck0","libkernel","libkernel",2,0x0101,2}));
    using RtcTick=std::int32_t (LBP3_GUEST_ABI *)(std::uint64_t*);
    auto rtc=reinterpret_cast<RtcTick>(resolve_rtc("18B2NS1y9UU").value());
    std::uint64_t first{},second{};check(rtc(&first)==0&&first>60000000000000000ull);
    Sleep(2);check(rtc(&second)==0&&second>=first);
    check(rtc(nullptr)<0&&rtc(reinterpret_cast<std::uint64_t*>(1))<0);
    check(!lbp3::hle::resolve_clock({"18B2NS1y9UU","libSceRtc","libSceRtc",2,0x0101,2}));
    check(!lbp3::hle::resolve_clock({"18B2NS1y9UU","libSceRtc","libSceRtc",1,0x0100,2}));
    std::cout<<"Clock tests passed: realtime, monotonic, CPU, timeval, timezone, process ID, errno isolation, protected pointers and resolver identities.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
