#include "lbp3_host/np_manager.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace lbp3::hle {
namespace {
constexpr std::int32_t signed_out=static_cast<std::int32_t>(0x80550006u);
constexpr std::int32_t invalid_argument=static_cast<std::int32_t>(0x80550003u);
constexpr std::int32_t np_state_signed_out=1;
std::atomic<std::int32_t> next_callback{1};
constexpr std::int32_t invalid_webapi=static_cast<std::int32_t>(0x80558003u);
constexpr std::int32_t webapi_signed_out=static_cast<std::int32_t>(0x80558011u);
constexpr std::int32_t request_id_offset=0x20000000;
constexpr std::size_t request_limit=0x20;
enum class RequestState { empty, ready, aborted, complete };
struct NpRequest { RequestState state{RequestState::empty}; bool async{}; std::int32_t result{}; };
std::mutex request_mutex;
std::vector<NpRequest> requests;
std::size_t active_requests{};
std::atomic<unsigned> np_trace_count{};
std::mutex webapi_mutex;
bool webapi_initialized{};
std::int32_t webapi_context{};
std::int32_t webapi_user_context{};
std::int32_t webapi_filter{};
struct WebApiRequest {std::int64_t id{};bool active{};};
std::array<WebApiRequest,32> webapi_requests{};
std::int64_t next_webapi_request{1};
void trace_np(const char* name,std::int32_t a=0,std::uintptr_t b=0) noexcept {
    static const bool enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"LBP3_NP_TRACE",value,static_cast<DWORD>(std::size(value)))>0;}();
    if(enabled){const auto n=np_trace_count.fetch_add(1,std::memory_order_relaxed);if(n<128)
        std::fprintf(stderr,"[LBP3_NP_TRACE] #%u %s %d 0x%llx\n",n+1,name,a,static_cast<unsigned long long>(b));}
}
// This process has no PSN session. With no locally queued callback, polling
// succeeds and reports no work, as the API's callback pump contract requires.
std::int32_t LBP3_GUEST_ABI check_callback() noexcept {trace_np("sceNpCheckCallback");return 0;}
std::int32_t LBP3_GUEST_ABI get_np_id(void* output) noexcept {trace_np("sceNpGetNpId",0,reinterpret_cast<std::uintptr_t>(output));return signed_out;}
std::int32_t LBP3_GUEST_ABI get_online_id(std::int32_t user_id,void* output) noexcept {trace_np("sceNpGetOnlineId",user_id,reinterpret_cast<std::uintptr_t>(output));return signed_out;}
std::int32_t create_request(bool async) noexcept {
    std::lock_guard lock(request_mutex);
    if(active_requests>=request_limit)return static_cast<std::int32_t>(0x8055000bu);
    std::size_t index{};
    while(index<requests.size()&&requests[index].state!=RequestState::empty)++index;
    if(index==requests.size())requests.emplace_back();
    requests[index]={RequestState::ready,async,0};++active_requests;
    return static_cast<std::int32_t>(request_id_offset+index+1);
}
std::int32_t LBP3_GUEST_ABI create_sync_request() noexcept {trace_np("sceNpCreateRequest");return create_request(false);}
std::int32_t LBP3_GUEST_ABI create_async_request(const void* parameter) noexcept {
    trace_np("sceNpCreateAsyncRequest",0,reinterpret_cast<std::uintptr_t>(parameter));
    if(!parameter)return invalid_argument;
    std::uint64_t size{};SIZE_T read{};
    if(!ReadProcessMemory(GetCurrentProcess(),parameter,&size,sizeof(size),&read)||read!=sizeof(size))return invalid_argument;
    if(size!=24)return static_cast<std::int32_t>(0x80550004u);
    return create_request(true);
}
std::int32_t request_index(std::int32_t id) noexcept {
    if(id<=request_id_offset)return -1;
    const auto index=static_cast<std::int64_t>(id)-request_id_offset-1;
    if(index<0||static_cast<std::uint64_t>(index)>=requests.size()||requests[static_cast<std::size_t>(index)].state==RequestState::empty)return -1;
    return static_cast<std::int32_t>(index);
}
std::int32_t LBP3_GUEST_ABI check_np_availability(std::int32_t id,const void* online_id) noexcept {
    trace_np("sceNpCheckNpAvailability",id,reinterpret_cast<std::uintptr_t>(online_id));
    if(!online_id)return invalid_argument;
    std::array<unsigned char,16> ignored{};SIZE_T read{};
    if(!ReadProcessMemory(GetCurrentProcess(),online_id,ignored.data(),ignored.size(),&read)||read!=ignored.size())return invalid_argument;
    std::lock_guard lock(request_mutex);const auto index=request_index(id);
    if(index<0)return static_cast<std::int32_t>(0x8055000au);
    auto& request=requests[static_cast<std::size_t>(index)];
    if(request.state==RequestState::complete)return invalid_argument;
    if(request.state==RequestState::aborted)return static_cast<std::int32_t>(0x8055000cu);
    request.state=RequestState::complete;request.result=signed_out;
    return request.async?0:signed_out;
}
std::int32_t LBP3_GUEST_ABI poll_async(std::int32_t id,std::int32_t* output) noexcept {
    trace_np("sceNpPollAsync",id,reinterpret_cast<std::uintptr_t>(output));
    if(!output)return invalid_argument;
    std::lock_guard lock(request_mutex);const auto index=request_index(id);
    if(index<0)return static_cast<std::int32_t>(0x8055000au);
    auto& request=requests[static_cast<std::size_t>(index)];
    if(!request.async||request.state==RequestState::ready)return static_cast<std::int32_t>(0x8055000du);
    if(request.state==RequestState::aborted)request.result=static_cast<std::int32_t>(0x8055000cu);
    SIZE_T written{};
    if(!WriteProcessMemory(GetCurrentProcess(),output,&request.result,sizeof(request.result),&written)||written!=sizeof(request.result))return invalid_argument;
    return 0;
}
std::int32_t LBP3_GUEST_ABI abort_request(std::int32_t id) noexcept {
    std::lock_guard lock(request_mutex);const auto index=request_index(id);if(index<0)return static_cast<std::int32_t>(0x8055000au);
    auto& request=requests[static_cast<std::size_t>(index)];if(request.state!=RequestState::complete)request.state=RequestState::aborted;return 0;
}
std::int32_t LBP3_GUEST_ABI delete_request(std::int32_t id) noexcept {
    std::lock_guard lock(request_mutex);const auto index=request_index(id);if(index<0)return static_cast<std::int32_t>(0x8055000au);
    requests[static_cast<std::size_t>(index)]={};--active_requests;return 0;
}
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
std::int32_t LBP3_GUEST_ABI webapi_initialize(std::int32_t http_context,std::uint64_t pool_size) noexcept {
    (void)pool_size;std::lock_guard lock(webapi_mutex);if(http_context<=0)return invalid_webapi;webapi_initialized=true;webapi_context=http_context;return 0;
}
std::int32_t LBP3_GUEST_ABI webapi_terminate() noexcept {
    std::lock_guard lock(webapi_mutex);if(!webapi_initialized)return invalid_webapi;webapi_initialized=false;webapi_context=0;webapi_user_context=0;webapi_filter=0;webapi_requests={};return 0;
}
std::int32_t LBP3_GUEST_ABI webapi_create_context(std::int32_t library_context,const void* online_id) noexcept {
    if(!online_id)return invalid_argument;std::array<unsigned char,16> ignored{};SIZE_T read{};
    if(!ReadProcessMemory(GetCurrentProcess(),online_id,ignored.data(),ignored.size(),&read)||read!=ignored.size())return invalid_argument;
    std::lock_guard lock(webapi_mutex);if(!webapi_initialized||library_context!=webapi_context||webapi_user_context)return invalid_webapi;
    webapi_user_context=next_callback.fetch_add(1,std::memory_order_relaxed);return webapi_user_context;
}
std::int32_t LBP3_GUEST_ABI webapi_delete_context(std::int32_t context) noexcept {
    std::lock_guard lock(webapi_mutex);if(!webapi_user_context||context!=webapi_user_context)return invalid_webapi;webapi_user_context=0;webapi_filter=0;webapi_requests={};return 0;
}
std::int32_t LBP3_GUEST_ABI webapi_create_filter(std::int32_t context,const void* filter,std::uint64_t count) noexcept {
    if(!filter||count==0)return invalid_argument;unsigned char probe{};SIZE_T read{};
    if(!ReadProcessMemory(GetCurrentProcess(),filter,&probe,1,&read)||read!=1)return invalid_argument;
    std::lock_guard lock(webapi_mutex);if(!webapi_user_context||context!=webapi_user_context)return invalid_webapi;
    if(webapi_filter)return static_cast<std::int32_t>(0x80558010u);webapi_filter=next_callback.fetch_add(1,std::memory_order_relaxed);return webapi_filter;
}
std::int32_t LBP3_GUEST_ABI webapi_register_push(std::int32_t context,std::int32_t filter,void* callback,void*) noexcept {
    std::lock_guard lock(webapi_mutex);if(!callback)return invalid_argument;if(context!=webapi_user_context||!webapi_filter||filter!=webapi_filter)return invalid_webapi;return webapi_signed_out;
}
std::int32_t LBP3_GUEST_ABI webapi_unregister_push(std::int32_t context,std::int32_t callback) noexcept {
    std::lock_guard lock(webapi_mutex);if(context!=webapi_user_context)return invalid_webapi;(void)callback;return 0;
}
std::int32_t LBP3_GUEST_ABI webapi_request(std::int32_t context,const char* group,const char* path,std::int32_t method,const void*,std::int64_t* output) noexcept {
    if(!group||!path||method<0||method>5||!output)return invalid_argument;
    char probe{};SIZE_T read{};if(!ReadProcessMemory(GetCurrentProcess(),group,&probe,1,&read)||read!=1||!ReadProcessMemory(GetCurrentProcess(),path,&probe,1,&read)||read!=1)return invalid_argument;
    std::lock_guard lock(webapi_mutex);if(!webapi_user_context||context!=webapi_user_context)return invalid_webapi;
    for(auto& request:webapi_requests)if(!request.active){request={next_webapi_request++,true};SIZE_T written{};
        if(!WriteProcessMemory(GetCurrentProcess(),output,&request.id,sizeof(request.id),&written)||written!=sizeof(request.id)){request={};return invalid_argument;}return 0;}
    return static_cast<std::int32_t>(0x80558004u);
}
bool webapi_request_exists(std::int64_t id) noexcept {for(const auto& request:webapi_requests)if(request.active&&request.id==id)return true;return false;}
std::int32_t LBP3_GUEST_ABI webapi_send_request(std::int64_t id) noexcept {std::lock_guard lock(webapi_mutex);return webapi_request_exists(id)?webapi_signed_out:invalid_webapi;}
std::int32_t LBP3_GUEST_ABI webapi_read_data(std::int64_t id,void* data,std::uint64_t size) noexcept {
    if(!data||!size)return invalid_argument;std::lock_guard lock(webapi_mutex);return webapi_request_exists(id)?webapi_signed_out:invalid_webapi;
}
std::int32_t LBP3_GUEST_ABI webapi_get_status(std::int64_t id,std::int32_t* output) noexcept {
    if(!output)return invalid_argument;std::lock_guard lock(webapi_mutex);return webapi_request_exists(id)?webapi_signed_out:invalid_webapi;
}
std::int32_t LBP3_GUEST_ABI webapi_get_error() noexcept {return webapi_signed_out;}
std::int32_t LBP3_GUEST_ABI webapi_abort_request(std::int64_t id) noexcept {std::lock_guard lock(webapi_mutex);return webapi_request_exists(id)?0:invalid_webapi;}
std::int32_t LBP3_GUEST_ABI webapi_delete_request(std::int64_t id) noexcept {std::lock_guard lock(webapi_mutex);for(auto& request:webapi_requests)if(request.active&&request.id==id){request={};return 0;}return invalid_webapi;}
std::int32_t LBP3_GUEST_ABI webapi_utility_parse_id(const void* input,void* output) noexcept {(void)input;(void)output;return webapi_signed_out;}
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
    if(r.nid=="XDncXQIJUSk")return reinterpret_cast<std::uintptr_t>(&get_online_id);
    if(r.nid=="GpLQDNKICac")return reinterpret_cast<std::uintptr_t>(&create_sync_request);
    if(r.nid=="eiqMCt9UshI")return reinterpret_cast<std::uintptr_t>(&create_async_request);
    if(r.nid=="2rsFmlGWleQ")return reinterpret_cast<std::uintptr_t>(&check_np_availability);
    if(r.nid=="uqcPJLWL08M")return reinterpret_cast<std::uintptr_t>(&poll_async);
    if(r.nid=="OzKvTvg3ZYU")return reinterpret_cast<std::uintptr_t>(&abort_request);
    if(r.nid=="S7QTn72PrDw")return reinterpret_cast<std::uintptr_t>(&delete_request);
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
    if(r.nid=="x1Y7yiYSk7c")return reinterpret_cast<std::uintptr_t>(&webapi_create_context);
    if(r.nid=="XUjdsSTTZ3U")return reinterpret_cast<std::uintptr_t>(&webapi_delete_context);
    if(r.nid=="y5Ta5JCzQHY")return reinterpret_cast<std::uintptr_t>(&webapi_create_filter);
    if(r.nid=="PfSTDCgNMgc")return reinterpret_cast<std::uintptr_t>(&webapi_register_push);
    if(r.nid=="qK4o2656W4w")return reinterpret_cast<std::uintptr_t>(&webapi_unregister_push);
    if(r.nid=="CQtPRSF6Ds8")return reinterpret_cast<std::uintptr_t>(&webapi_read_data);
    if(r.nid=="JzhYTP2fG18")return reinterpret_cast<std::uintptr_t>(&webapi_abort_request);
    if(r.nid=="or0e885BlXo")return reinterpret_cast<std::uintptr_t>(&webapi_utility_parse_id);
    if(r.nid=="rdgs5Z1MyFw")return reinterpret_cast<std::uintptr_t>(&webapi_request);
    if(r.nid=="kVbL4hL3K7w")return reinterpret_cast<std::uintptr_t>(&webapi_send_request);
    if(r.nid=="k210oKgP80Y")return reinterpret_cast<std::uintptr_t>(&webapi_get_status);
    if(r.nid=="2qSZ0DgwTsc")return reinterpret_cast<std::uintptr_t>(&webapi_get_error);
    if(r.nid=="noQgleu+KLE")return reinterpret_cast<std::uintptr_t>(&webapi_delete_request);
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
    {
        std::lock_guard lock(request_mutex);requests.clear();active_requests=0;
    }
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
    using GetOnlineId=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,void*) noexcept;
    const auto online_id=resolve_np_manager({"XDncXQIJUSk","libSceNpManager","libSceNpManager",1,0x0101,2});
    std::array<unsigned char,16> online_id_output{};
    if(!online_id||reinterpret_cast<GetOnlineId>(*online_id)(0x10000001,online_id_output.data())!=signed_out||
       reinterpret_cast<GetOnlineId>(*online_id)(0x10000001,nullptr)!=signed_out)
        throw std::runtime_error("Offline NP online-ID contract failed.");
    const auto create_sync=resolve_np_manager({"GpLQDNKICac","libSceNpManager","libSceNpManager",1,0x0101,2});
    const auto create_async=resolve_np_manager({"eiqMCt9UshI","libSceNpManager","libSceNpManager",1,0x0101,2});
    const auto check_availability=resolve_np_manager({"2rsFmlGWleQ","libSceNpManager","libSceNpManager",1,0x0101,2});
    const auto poll=resolve_np_manager({"uqcPJLWL08M","libSceNpManager","libSceNpManager",1,0x0101,2});
    const auto abort=resolve_np_manager({"OzKvTvg3ZYU","libSceNpManager","libSceNpManager",1,0x0101,2});
    const auto remove=resolve_np_manager({"S7QTn72PrDw","libSceNpManager","libSceNpManager",1,0x0101,2});
    using CreateSync=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    using CreateAsync=std::int32_t (LBP3_GUEST_ABI *)(const void*) noexcept;
    using CheckAvailability=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const void*) noexcept;
    using Poll=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t*) noexcept;
    using RequestOp=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t) noexcept;
    struct AsyncParameter {std::uint64_t size,affinity;std::int32_t priority;std::uint32_t padding;};
    if(!create_sync||!create_async||!check_availability||!poll||!abort||!remove)
        throw std::runtime_error("Offline NP request NID resolution failed.");
    if(reinterpret_cast<CreateAsync>(*create_async)(nullptr)!=invalid_argument)
        throw std::runtime_error("NP async-request null parameter validation failed.");
    AsyncParameter bad_parameter{16,0,0,0};
    if(reinterpret_cast<CreateAsync>(*create_async)(&bad_parameter)!=static_cast<std::int32_t>(0x80550004u))
        throw std::runtime_error("NP async-request size validation failed.");
    AsyncParameter parameter{24,0,0,0};
    const auto async_id=reinterpret_cast<CreateAsync>(*create_async)(&parameter);
    if(async_id!=request_id_offset+1||reinterpret_cast<Poll>(*poll)(async_id,nullptr)!=invalid_argument)
        throw std::runtime_error("NP async request creation or output validation failed.");
    std::int32_t poll_result{};
    if(reinterpret_cast<Poll>(*poll)(async_id,&poll_result)!=static_cast<std::int32_t>(0x8055000du))
        throw std::runtime_error("NP pending async poll state failed.");
    if(reinterpret_cast<CheckAvailability>(*check_availability)(async_id,online_id_output.data())!=0||
       reinterpret_cast<Poll>(*poll)(async_id,&poll_result)!=0||poll_result!=signed_out)
        throw std::runtime_error("Offline NP async request did not complete as signed out.");
    if(reinterpret_cast<RequestOp>(*remove)(async_id)!=0||reinterpret_cast<Poll>(*poll)(async_id,&poll_result)!=static_cast<std::int32_t>(0x8055000au))
        throw std::runtime_error("NP async request deletion or stale-handle validation failed.");
    const auto sync_id=reinterpret_cast<CreateSync>(*create_sync)();
    if(reinterpret_cast<CheckAvailability>(*check_availability)(sync_id,online_id_output.data())!=signed_out||
       reinterpret_cast<RequestOp>(*remove)(sync_id)!=0)
        throw std::runtime_error("Synchronous offline NP request completion failed.");
    const auto aborted_id=reinterpret_cast<CreateAsync>(*create_async)(&parameter);
    if(reinterpret_cast<RequestOp>(*abort)(aborted_id)!=0||reinterpret_cast<Poll>(*poll)(aborted_id,&poll_result)!=0||poll_result!=static_cast<std::int32_t>(0x8055000cu)||
       reinterpret_cast<RequestOp>(*remove)(aborted_id)!=0||reinterpret_cast<RequestOp>(*abort)(aborted_id)!=static_cast<std::int32_t>(0x8055000au))
        throw std::runtime_error("NP request abort lifecycle failed.");
    const auto max_async=reinterpret_cast<CreateAsync>(*create_async)(&parameter);
    std::array<std::int32_t,request_limit-1> request_ids{};
    for(auto& request_id:request_ids){request_id=reinterpret_cast<CreateAsync>(*create_async)(&parameter);if(request_id<0)throw std::runtime_error("NP request capacity was reached too early.");}
    if(reinterpret_cast<CreateAsync>(*create_async)(&parameter)!=static_cast<std::int32_t>(0x8055000bu))
        throw std::runtime_error("NP active-request capacity limit failed.");
    if(reinterpret_cast<RequestOp>(*remove)(max_async)!=0)throw std::runtime_error("NP request capacity cleanup failed.");
    for(const auto request_id:request_ids)if(reinterpret_cast<RequestOp>(*remove)(request_id)!=0)throw std::runtime_error("NP request capacity cleanup failed.");
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
    using WebInit=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::uint64_t) noexcept;
    using WebTerm=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    if(!web_init||!web_term||reinterpret_cast<WebInit>(*web_init)(7,0x4000)!=0||
       resolve_np_webapi({"G3AnLNdRBjE","libSceNpWebApi","libSceNpWebApi",1,0x0101,1}))
        throw std::runtime_error("Offline NP Web API context contract failed.");
    const HostImport web_identity{"libSceNpWebApi","libSceNpWebApi","libSceNpWebApi",1,0x0101,2};
    auto web_lookup=[&](const char* nid){auto id=web_identity;id.nid=nid;return resolve_np_webapi(id);};
    using WebCreateContext=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const void*) noexcept;
    using WebDeleteContext=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t) noexcept;
    using WebCreateFilter=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const void*,std::uint64_t) noexcept;
    using WebRegisterPush=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t,void*,void*) noexcept;
    using WebRequest=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,const char*,const char*,std::int32_t,const void*,std::int64_t*) noexcept;
    using WebSend=std::int32_t (LBP3_GUEST_ABI *)(std::int64_t) noexcept;
    using WebRead=std::int32_t (LBP3_GUEST_ABI *)(std::int64_t,void*,std::uint64_t) noexcept;
    using WebStatus=std::int32_t (LBP3_GUEST_ABI *)(std::int64_t,std::int32_t*) noexcept;
    using WebDelete=std::int32_t (LBP3_GUEST_ABI *)(std::int64_t) noexcept;
    const auto web_create_context=web_lookup("x1Y7yiYSk7c"),web_delete_context=web_lookup("XUjdsSTTZ3U");
    const auto web_create_filter=web_lookup("y5Ta5JCzQHY"),web_register=web_lookup("PfSTDCgNMgc");
    const auto web_request=web_lookup("rdgs5Z1MyFw"),web_send=web_lookup("kVbL4hL3K7w");
    const auto web_read=web_lookup("CQtPRSF6Ds8"),web_status=web_lookup("k210oKgP80Y");
    const auto web_delete=web_lookup("noQgleu+KLE"),web_abort=web_lookup("JzhYTP2fG18");
    std::array<unsigned char,16> fake_online_id{};
    const auto user_context=web_create_context?reinterpret_cast<WebCreateContext>(*web_create_context)(7,fake_online_id.data()):0;
    if(!web_create_context||!web_delete_context||!web_create_filter||!web_register||!web_request||!web_send||!web_read||!web_status||!web_delete||!web_abort||user_context<=0)
        throw std::runtime_error("Offline WebAPI user context creation failed.");
    const std::array<std::uint64_t,2> filter{16,0};
    const auto filter_id=reinterpret_cast<WebCreateFilter>(*web_create_filter)(user_context,filter.data(),1);
    if(filter_id<=0||reinterpret_cast<WebCreateFilter>(*web_create_filter)(user_context,filter.data(),1)!=static_cast<std::int32_t>(0x80558010u)||
       reinterpret_cast<WebRegisterPush>(*web_register)(user_context,filter_id,nullptr,nullptr)!=invalid_argument||
       reinterpret_cast<WebRegisterPush>(*web_register)(user_context,filter_id,reinterpret_cast<void*>(1),nullptr)!=webapi_signed_out)
        throw std::runtime_error("Offline WebAPI push-filter contract failed.");
    std::int64_t web_request_id{};
    if(reinterpret_cast<WebRequest>(*web_request)(user_context,"lbp","/status",0,nullptr,&web_request_id)!=0||web_request_id<=0||
       reinterpret_cast<WebSend>(*web_send)(web_request_id)!=webapi_signed_out||
       reinterpret_cast<WebRead>(*web_read)(web_request_id,fake_online_id.data(),fake_online_id.size())!=webapi_signed_out||
       reinterpret_cast<WebStatus>(*web_status)(web_request_id,&np_state)!=webapi_signed_out||
       reinterpret_cast<WebDelete>(*web_abort)(web_request_id)!=0||reinterpret_cast<WebDelete>(*web_delete)(web_request_id)!=0||
       reinterpret_cast<WebDelete>(*web_delete)(web_request_id)!=invalid_webapi)
        throw std::runtime_error("Offline WebAPI request lifecycle failed.");
    if(reinterpret_cast<WebDeleteContext>(*web_delete_context)(user_context)!=0||reinterpret_cast<WebTerm>(*web_term)()!=0||
       reinterpret_cast<WebTerm>(*web_term)()!=invalid_webapi)
        throw std::runtime_error("Offline WebAPI termination after context deletion failed.");
    const auto signaling_init=resolve_np_signaling({"3KOuC4RmZZU","libSceNpSignaling","libSceNpSignaling",1,0x0101,2});
    const auto signaling_context=resolve_np_signaling({"5yYjEdd4t8Y","libSceNpSignaling","libSceNpSignaling",1,0x0101,2});
    using OfflineOperation=std::int32_t (LBP3_GUEST_ABI *)();
    if(!signaling_init||reinterpret_cast<OfflineOperation>(*signaling_init)()!=0||!signaling_context||
       reinterpret_cast<OfflineOperation>(*signaling_context)()!=signed_out||
       resolve_np_signaling({"3KOuC4RmZZU","libSceNpSignaling","libSceNpSignaling",1,0x0101,1}))
        throw std::runtime_error("Offline NP Signaling contract failed.");
}
}
