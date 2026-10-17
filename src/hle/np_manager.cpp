#include "lbp3_host/np_manager.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace lbp3::hle {
namespace {
constexpr std::int32_t signed_out=static_cast<std::int32_t>(0x80550006u);
constexpr std::int32_t invalid_argument=static_cast<std::int32_t>(0x80550003u);
constexpr std::int32_t np_state_signed_out=1;
std::atomic<std::int32_t> next_callback{1};
std::atomic<unsigned> np_trace_count{};
void trace_np(const char* name,std::int32_t a=0,std::uintptr_t b=0) noexcept {
    static const bool enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"LBP3_NP_TRACE",value,static_cast<DWORD>(std::size(value)))>0;}();
    if(enabled){const auto n=np_trace_count.fetch_add(1,std::memory_order_relaxed);if(n<128)
        std::fprintf(stderr,"[LBP3_NP_TRACE] #%u %s %d 0x%llx\n",n+1,name,a,static_cast<unsigned long long>(b));}
}
// This process has no PSN session. With no locally queued callback, polling
// succeeds and reports no work, as the API's callback pump contract requires.
std::int32_t LBP3_GUEST_ABI check_callback() noexcept {trace_np("sceNpCheckCallback");return 0;}
std::int32_t LBP3_GUEST_ABI get_np_id(void* output) noexcept {trace_np("sceNpGetNpId",0,reinterpret_cast<std::uintptr_t>(output));return signed_out;}
std::int32_t LBP3_GUEST_ABI get_np_state(std::int32_t user_id, std::int32_t* state) noexcept {
    trace_np("sceNpGetState",user_id,reinterpret_cast<std::uintptr_t>(state));
    if(!state)return invalid_argument;
    const std::int32_t value=np_state_signed_out;
    SIZE_T written{};
    if(!WriteProcessMemory(GetCurrentProcess(),state,&value,sizeof(value),&written)||written!=sizeof(value))
        return invalid_argument;
    return 0;
}
std::int32_t LBP3_GUEST_ABI set_np_title_id(const char* title) noexcept {trace_np("sceNpSetNpTitleId",0,reinterpret_cast<std::uintptr_t>(title));return 0;}
// The title uses this only to announce its content-rating policy to NP
// services. The ISS offline compatibility layer treats it as a no-op; there
// is no PSN account or network policy service in this host process.
std::int32_t LBP3_GUEST_ABI set_content_restriction(const void*) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI register_state_callback(void* callback,void* user) noexcept {trace_np("sceNpRegisterStateCallback",static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(callback)&0x7fffffff),reinterpret_cast<std::uintptr_t>(user));return next_callback.fetch_add(1,std::memory_order_relaxed);}
std::int32_t LBP3_GUEST_ABI webapi_initialize() noexcept {return next_callback.fetch_add(1,std::memory_order_relaxed);}
std::int32_t LBP3_GUEST_ABI webapi_terminate(std::int32_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI signaling_ok() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI signaling_signed_out() noexcept {return signed_out;}
std::int32_t LBP3_GUEST_ABI compare_online_id(const void* left,const void* right) noexcept {
    if(!left||!right)return -1;
    std::array<unsigned char,16> a{},b{};SIZE_T read_a{},read_b;
    if(!ReadProcessMemory(GetCurrentProcess(),left,a.data(),a.size(),&read_a)||read_a!=a.size()||
       !ReadProcessMemory(GetCurrentProcess(),right,b.data(),b.size(),&read_b)||read_b!=b.size())return -1;
    for(std::size_t i=0;i<a.size();++i){
        if(a[i]!=b[i])return static_cast<std::int32_t>(a[i])-static_cast<std::int32_t>(b[i]);
        if(a[i]==0)return 0;
    }
    return 0;
}
}

std::optional<std::uintptr_t> resolve_np_manager(const HostImport& r){
    if(r.type==2&&r.library=="libSceNpCommon"&&r.module=="libSceNpCommon"&&
       r.library_version==1&&r.module_version==0x0101&&r.nid=="dj+O5aD2a0Q")
        return reinterpret_cast<std::uintptr_t>(&compare_online_id);
    if(r.type!=2||r.library!="libSceNpManager"||r.module!="libSceNpManager"||
       r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="3Zl8BePTh9Y")return reinterpret_cast<std::uintptr_t>(&check_callback);
    if(r.nid=="p-o74CnoNzY")return reinterpret_cast<std::uintptr_t>(&get_np_id);
    if(r.nid=="eQH7nWPcAgc")return reinterpret_cast<std::uintptr_t>(&get_np_state);
    if(r.nid=="Ec63y59l9tw")return reinterpret_cast<std::uintptr_t>(&set_np_title_id);
    if(r.nid=="A2CQ3kgSopQ")return reinterpret_cast<std::uintptr_t>(&set_content_restriction);
    if(r.nid=="VfRSmPmj8Q8")return reinterpret_cast<std::uintptr_t>(&register_state_callback);
    return std::nullopt;
}

std::optional<std::uintptr_t> resolve_np_webapi(const HostImport& r){
    if(r.type!=2||r.library!="libSceNpWebApi"||r.module!="libSceNpWebApi"||
       r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="G3AnLNdRBjE")return reinterpret_cast<std::uintptr_t>(&webapi_initialize);
    if(r.nid=="asz3TtIqGF8")return reinterpret_cast<std::uintptr_t>(&webapi_terminate);
    return std::nullopt;
}

std::optional<std::uintptr_t> resolve_np_signaling(const HostImport& r){
    if(r.type!=2||r.library!="libSceNpSignaling"||r.module!="libSceNpSignaling"||
       r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="3KOuC4RmZZU"||r.nid=="NPhw0UXaNrk"||r.nid=="hx+LIg-1koI"||r.nid=="6UEembipgrM")
        return reinterpret_cast<std::uintptr_t>(&signaling_ok);
    if(r.nid=="5yYjEdd4t8Y"||r.nid=="0UvTFeomAUM"||r.nid=="bD-JizUb3JM")
        return reinterpret_cast<std::uintptr_t>(&signaling_signed_out);
    return std::nullopt;
}

void np_manager_self_test(){
    using Callback=std::int32_t (LBP3_GUEST_ABI *)();
    const HostImport request{"3Zl8BePTh9Y","libSceNpManager","libSceNpManager",1,0x0101,2};
    const auto address=resolve_np_manager(request);
    if(!address||reinterpret_cast<Callback>(*address)()!=0)
        throw std::runtime_error("Offline NP callback pump contract failed.");
    using CompareId=std::int32_t (LBP3_GUEST_ABI *)(const void*,const void*) noexcept;
    const auto compare=resolve_np_manager({"dj+O5aD2a0Q","libSceNpCommon","libSceNpCommon",1,0x0101,2});
    std::array<char,16> id_a{},id_b{};std::memcpy(id_a.data(),"LucasOnlineId",13);std::memcpy(id_b.data(),"LucasOnlineId",13);
    if(!compare||reinterpret_cast<CompareId>(*compare)(id_a.data(),id_b.data())!=0)
        throw std::runtime_error("NP online-ID equality comparison failed.");
    id_b[0]='l';
    if(reinterpret_cast<CompareId>(*compare)(id_a.data(),id_b.data())==0||reinterpret_cast<CompareId>(*compare)(nullptr,id_b.data())==0||
       resolve_np_manager({"dj+O5aD2a0Q","libSceNpManager","libSceNpCommon",1,0x0101,2}))
        throw std::runtime_error("NP online-ID inequality, invalid pointer or resolver identity failed.");
    auto wrong=request;wrong.type=1;if(resolve_np_manager(wrong))
        throw std::runtime_error("NP function resolved as data.");
    wrong=request;wrong.module_version=0;if(resolve_np_manager(wrong))
        throw std::runtime_error("NP function resolved for the wrong module version.");
    const auto npid=resolve_np_manager({"p-o74CnoNzY","libSceNpManager","libSceNpManager",1,0x0101,2});
    using GetNpId=std::int32_t (LBP3_GUEST_ABI *)(void*);
    if(!npid||reinterpret_cast<GetNpId>(*npid)(nullptr)!=signed_out||
       resolve_np_manager({"p-o74CnoNzY","libSceNpManager","libSceNpManager",1,0x0101,1}))
        throw std::runtime_error("Offline NP identity contract failed.");
    const auto state=resolve_np_manager({"eQH7nWPcAgc","libSceNpManager","libSceNpManager",1,0x0101,2});
    using GetNpState=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t*) noexcept;
    std::int32_t np_state{};
    if(!state||reinterpret_cast<GetNpState>(*state)(0x10000001,&np_state)!=0||np_state!=np_state_signed_out||
       reinterpret_cast<GetNpState>(*state)(0x10000001,nullptr)!=invalid_argument||
       reinterpret_cast<GetNpState>(*state)(0x10000001,reinterpret_cast<std::int32_t*>(1))!=invalid_argument||
       resolve_np_manager({"eQH7nWPcAgc","libSceNpManager","libSceNpManager",1,0x0101,1}))
        throw std::runtime_error("Offline NP state query contract failed.");
    const auto title=resolve_np_manager({"Ec63y59l9tw","libSceNpManager","libSceNpManager",1,0x0101,2});
    using SetTitle=std::int32_t (LBP3_GUEST_ABI *)(const char*);
    if(!title||reinterpret_cast<SetTitle>(*title)("CUSA00000")||
       resolve_np_manager({"Ec63y59l9tw","libSceNpManager","libSceNpManager",1,0x0101,1}))
        throw std::runtime_error("Offline NP title-id setup contract failed.");
    const auto restriction=resolve_np_manager({"A2CQ3kgSopQ","libSceNpManager","libSceNpManager",1,0x0101,2});
    using SetRestriction=std::int32_t (LBP3_GUEST_ABI *)(const void*);
    if(!restriction||reinterpret_cast<SetRestriction>(*restriction)(nullptr)!=0||
       resolve_np_manager({"A2CQ3kgSopQ","libSceNpManager","libSceNpManager",1,0x0101,1}))
        throw std::runtime_error("Offline NP content restriction contract failed.");
    const auto callback=resolve_np_manager({"VfRSmPmj8Q8","libSceNpManager","libSceNpManager",1,0x0101,2});
    using Register=std::int32_t (LBP3_GUEST_ABI *)(void*,void*);
    const auto callback_id=callback?reinterpret_cast<Register>(*callback)(nullptr,nullptr):0;
    if(callback_id<=0||!callback||resolve_np_manager({"VfRSmPmj8Q8","libSceNpManager","libSceNpManager",1,0x0101,1}))
        throw std::runtime_error("Offline NP state callback registration contract failed.");
    const auto web_init=resolve_np_webapi({"G3AnLNdRBjE","libSceNpWebApi","libSceNpWebApi",1,0x0101,2});
    const auto web_term=resolve_np_webapi({"asz3TtIqGF8","libSceNpWebApi","libSceNpWebApi",1,0x0101,2});
    using WebInit=std::int32_t (LBP3_GUEST_ABI *)();
    using WebTerm=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    const auto web_context=web_init?reinterpret_cast<WebInit>(*web_init)():0;
    if(web_context<=0||!web_term||reinterpret_cast<WebTerm>(*web_term)(web_context)!=0||
       resolve_np_webapi({"G3AnLNdRBjE","libSceNpWebApi","libSceNpWebApi",1,0x0101,1}))
        throw std::runtime_error("Offline NP Web API context contract failed.");
    const auto signaling_init=resolve_np_signaling({"3KOuC4RmZZU","libSceNpSignaling","libSceNpSignaling",1,0x0101,2});
    const auto signaling_context=resolve_np_signaling({"5yYjEdd4t8Y","libSceNpSignaling","libSceNpSignaling",1,0x0101,2});
    using OfflineOperation=std::int32_t (LBP3_GUEST_ABI *)();
    if(!signaling_init||reinterpret_cast<OfflineOperation>(*signaling_init)()!=0||!signaling_context||
       reinterpret_cast<OfflineOperation>(*signaling_context)()!=signed_out||
       resolve_np_signaling({"3KOuC4RmZZU","libSceNpSignaling","libSceNpSignaling",1,0x0101,1}))
        throw std::runtime_error("Offline NP Signaling contract failed.");
}
}
