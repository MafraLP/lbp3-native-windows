#include "lbp3_host/network_control.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <map>
#include <mutex>
#include <stdexcept>

namespace lbp3::hle {
namespace {
constexpr auto not_connected=std::bit_cast<std::int32_t>(0x80412108u);
constexpr auto invalid_address=std::bit_cast<std::int32_t>(0x80412107u);
constexpr auto invalid_handle=std::bit_cast<std::int32_t>(0x80412103u);
std::atomic<std::int32_t> next_callback{1};
std::mutex callback_mutex;
std::map<std::int32_t,std::pair<void*,void*>> callbacks;

bool writable(void* pointer,std::size_t size) noexcept {
    auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at||size>UINTPTR_MAX-at)return false;
    const auto end=at+size;
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

std::int32_t LBP3_GUEST_ABI check_callback() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI net_init() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI net_term() noexcept {return 0;}
// ISS uses an opaque positive handle for the offline pool. Network operations
// remain disconnected, but startup code can create and destroy its pool.
std::int32_t LBP3_GUEST_ABI net_pool_create(const char*,std::int32_t,std::int32_t) noexcept {
    const auto id=next_callback.fetch_add(1,std::memory_order_relaxed);
    return id>0?id:invalid_handle;
}
std::int32_t LBP3_GUEST_ABI net_pool_destroy(std::int32_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI ssl_init(std::int32_t) noexcept {
    const auto id=next_callback.fetch_add(1,std::memory_order_relaxed);
    return id>0?id:invalid_handle;
}
std::int32_t LBP3_GUEST_ABI ssl_term(std::int32_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI http_init(std::int32_t,std::int32_t,std::int32_t) noexcept {
    const auto id=next_callback.fetch_add(1,std::memory_order_relaxed);
    return id>0?id:invalid_handle;
}
std::int32_t LBP3_GUEST_ABI http_term(std::int32_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI http_object() noexcept {
    const auto id=next_callback.fetch_add(1,std::memory_order_relaxed);
    return id>0?id:invalid_handle;
}
std::int32_t LBP3_GUEST_ABI http_ok() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI http_network_error() noexcept {return std::bit_cast<std::int32_t>(0x80431063u);}
std::int32_t LBP3_GUEST_ABI http_create_epoll(std::int32_t,void** output) noexcept {
    if(!output)return std::bit_cast<std::int32_t>(0x80431077u);
    const auto id=next_callback.fetch_add(1,std::memory_order_relaxed);
    if(id<=0)return invalid_handle;
    void* handle=reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100+id));
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(),output,&handle,sizeof(handle),&written)&&written==sizeof(handle)
        ?0:invalid_address;
}
std::int32_t LBP3_GUEST_ABI http_wait(void*,void*,std::int32_t,std::int64_t) noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI https_load_cert() noexcept {return 0;}
std::int32_t LBP3_GUEST_ABI get_state(std::int32_t* output) noexcept {
    constexpr std::int32_t disconnected=0;
    return copy_out(output,&disconnected,sizeof(disconnected))?0:invalid_address;
}
std::int32_t LBP3_GUEST_ABI get_info(std::int32_t,void*) noexcept {return not_connected;}
std::int32_t LBP3_GUEST_ABI register_callback(void* callback,void* user,std::int32_t* output) noexcept {
    if(!callback||!writable(output,sizeof(*output)))return invalid_address;
    try{
        const auto id=next_callback.fetch_add(1);
        if(id<=0)return invalid_handle;
        {
            std::lock_guard lock(callback_mutex);
            if(callbacks.size()>=64)return invalid_handle;
            callbacks.emplace(id,std::pair{callback,user});
        }
        if(!copy_out(output,&id,sizeof(id))){std::lock_guard lock(callback_mutex);callbacks.erase(id);return invalid_address;}
        return 0;
    }catch(...){return invalid_handle;}
}
std::int32_t LBP3_GUEST_ABI unregister_callback(std::int32_t id) noexcept {
    std::lock_guard lock(callback_mutex);
    return callbacks.erase(id)?0:invalid_handle;
}
std::int32_t LBP3_GUEST_ABI get_nat_info(std::uint32_t* output) noexcept {
    // Connected-network NAT data is unavailable offline; report no STUN data,
    // restrictive NAT and no mapped address. The API preserves output[0].
    const std::array<std::uint32_t,3> result{0,3,0};
    if(!writable(output,4*sizeof(std::uint32_t)))return invalid_address;
    auto* fields=reinterpret_cast<std::byte*>(output)+sizeof(std::uint32_t);
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(),fields,result.data(),sizeof(result),&written)&&written==sizeof(result)?0:invalid_address;
}
}

std::optional<std::uintptr_t> resolve_network_control(const HostImport& r){
    if(r.type==2&&r.library=="libSceNet"&&r.module=="libSceNet"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="Nlev7Lg8k3A")return reinterpret_cast<std::uintptr_t>(&net_init);
        if(r.nid=="cTGkc6-TBlI")return reinterpret_cast<std::uintptr_t>(&net_term);
        if(r.nid=="dgJBaeJnGpo")return reinterpret_cast<std::uintptr_t>(&net_pool_create);
        if(r.nid=="K7RlrTkI-mw")return reinterpret_cast<std::uintptr_t>(&net_pool_destroy);
    }
    if(r.type==2&&r.library=="libSceSsl"&&r.module=="libSceSsl"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="hdpVEUDFW3s")return reinterpret_cast<std::uintptr_t>(&ssl_init);
        if(r.nid=="0K1yQ6Lv-Yc")return reinterpret_cast<std::uintptr_t>(&ssl_term);
    }
    if(r.type==2&&r.library=="libSceHttp"&&r.module=="libSceHttp"&&
       r.library_version==1&&r.module_version==0x0101){
        if(r.nid=="A9cVMUtEp4Y")return reinterpret_cast<std::uintptr_t>(&http_init);
        if(r.nid=="Ik-KpLTlf7Q")return reinterpret_cast<std::uintptr_t>(&http_term);
        if(r.nid=="DK+GoXCNT04")return reinterpret_cast<std::uintptr_t>(&https_load_cert);
        // This LBP3 build imports an additional versioned HTTP object creator
        // not present in the ISS import-name table; keep it in the same
        // offline opaque-handle class and let transfer APIs report offline.
        if(r.nid=="0gYjPTR-6cY"||r.nid=="qgxDBjorUxs"||r.nid=="Aeu5wVKkF9w"||r.nid=="XNUoD2B9a6A")return reinterpret_cast<std::uintptr_t>(&http_object);
        if(r.nid=="4I8vEpuEhZ8"||r.nid=="s2-NPIvz+iA"||r.nid=="sWQiqKvYTVA"||
           r.nid=="wYhXVfS2Et4"||r.nid=="f42K37mm5RM"||r.nid=="mSQCxzWTwVI"||
           r.nid=="EY28T2bkN7k"||r.nid=="PTiFIUxCpJc"||r.nid=="0S9tTH0uqTU"||
           r.nid=="-xm7kZQNpHI"||r.nid=="59tL1AQBb8U"||r.nid=="qe7oZ+v4PWA"||
           r.nid=="P6A3ytpsiYc"||r.nid=="sWQiqKvYTVA"||r.nid=="qFg2SuyTJJY"||
           r.nid=="hvG6GfBMXg8"||r.nid=="K1d1LqZRQHQ"||r.nid=="xegFfZKBVlw"||
           r.nid=="yigr4V0-HTM"||r.nid=="Tc-hAYDKtQc"||r.nid=="T-mGo9f3Pu4"||
           r.nid=="PDxS48xGQLs"||r.nid=="htyBOoWeS58")return reinterpret_cast<std::uintptr_t>(&http_ok);
        if(r.nid=="6381dWF+xsQ")return reinterpret_cast<std::uintptr_t>(&http_create_epoll);
        if(r.nid=="qISjDHrxONc")return reinterpret_cast<std::uintptr_t>(&http_wait);
        if(r.nid=="1e2BNwI-XzE"||r.nid=="0a2TBNfE3BU"||r.nid=="yuO2H2Uvnos"||
           r.nid=="P5pdoykPYTk"||r.nid=="JBN6N-EY+3M"||r.nid=="hPTXo3bICzI")
            return reinterpret_cast<std::uintptr_t>(&http_network_error);
    }
    if(r.type!=2||r.library!="libSceNetCtl"||r.module!="libSceNetCtl"||
       r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="iQw3iQPhvUQ")return reinterpret_cast<std::uintptr_t>(&check_callback);
    if(r.nid=="UJ+Z7Q+4ck0")return reinterpret_cast<std::uintptr_t>(&register_callback);
    if(r.nid=="Rqm2OnZMCz0")return reinterpret_cast<std::uintptr_t>(&unregister_callback);
    if(r.nid=="uBPlr0lbuiI")return reinterpret_cast<std::uintptr_t>(&get_state);
    if(r.nid=="obuxdTiwkF8")return reinterpret_cast<std::uintptr_t>(&get_info);
    if(r.nid=="JO4yuTuMoKI")return reinterpret_cast<std::uintptr_t>(&get_nat_info);
    return std::nullopt;
}

void network_control_self_test(){
    using Check=std::int32_t (LBP3_GUEST_ABI *)();
    using State=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*);
    using Info=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,void*);
    using Register=std::int32_t (LBP3_GUEST_ABI *)(void*,void*,std::int32_t*);
    using Unregister=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    using Nat=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t*);
    const auto get=[](const char* nid){return resolve_network_control({nid,"libSceNetCtl","libSceNetCtl",1,0x0101,2});};
    const auto check_callback=reinterpret_cast<Check>(get("iQw3iQPhvUQ").value());
    const auto state=reinterpret_cast<State>(get("uBPlr0lbuiI").value());
    const auto info=reinterpret_cast<Info>(get("obuxdTiwkF8").value());
    const auto reg=reinterpret_cast<Register>(get("UJ+Z7Q+4ck0").value());
    const auto unreg=reinterpret_cast<Unregister>(get("Rqm2OnZMCz0").value());
    const auto nat=reinterpret_cast<Nat>(get("JO4yuTuMoKI").value());
    using NetLifecycle=std::int32_t (LBP3_GUEST_ABI *)();
    using PoolCreate=std::int32_t (LBP3_GUEST_ABI *)(const char*,std::int32_t,std::int32_t);
    using PoolDestroy=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    const auto net_init_address=resolve_network_control({"Nlev7Lg8k3A","libSceNet","libSceNet",1,0x0101,2});
    const auto net_term_address=resolve_network_control({"cTGkc6-TBlI","libSceNet","libSceNet",1,0x0101,2});
    const auto pool_create_address=resolve_network_control({"dgJBaeJnGpo","libSceNet","libSceNet",1,0x0101,2});
    const auto pool_destroy_address=resolve_network_control({"K7RlrTkI-mw","libSceNet","libSceNet",1,0x0101,2});
    if(!net_init_address||!net_term_address||!pool_create_address||!pool_destroy_address||reinterpret_cast<NetLifecycle>(*net_init_address)()!=0||
       reinterpret_cast<NetLifecycle>(*net_term_address)()!=0||
       resolve_network_control({"Nlev7Lg8k3A","libSceNet","libSceNet",1,0x0101,1}))
        throw std::runtime_error("Offline network initialization contract failed.");
    const auto pool=reinterpret_cast<PoolCreate>(*pool_create_address)("lbp3-offline",64*1024,0);
    if(pool<=0||reinterpret_cast<PoolDestroy>(*pool_destroy_address)(pool)!=0||
       resolve_network_control({"dgJBaeJnGpo","libSceNet","libSceNet",1,0x0101,1}))
        throw std::runtime_error("Offline network pool handle contract failed.");
    const auto ssl_init_address=resolve_network_control({"hdpVEUDFW3s","libSceSsl","libSceSsl",1,0x0101,2});
    const auto ssl_term_address=resolve_network_control({"0K1yQ6Lv-Yc","libSceSsl","libSceSsl",1,0x0101,2});
    using SslInit=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    using SslTerm=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    const auto ssl_context=ssl_init_address?reinterpret_cast<SslInit>(*ssl_init_address)(1024*1024):0;
    if(ssl_context<=0||!ssl_term_address||reinterpret_cast<SslTerm>(*ssl_term_address)(ssl_context)!=0||
       resolve_network_control({"hdpVEUDFW3s","libSceSsl","libSceSsl",1,0x0101,1}))
        throw std::runtime_error("Offline SSL context lifecycle contract failed.");
    const auto http_init_address=resolve_network_control({"A9cVMUtEp4Y","libSceHttp","libSceHttp",1,0x0101,2});
    const auto http_term_address=resolve_network_control({"Ik-KpLTlf7Q","libSceHttp","libSceHttp",1,0x0101,2});
    using HttpInit=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t,std::int32_t);
    using HttpTerm=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t);
    const auto http_context=http_init_address?reinterpret_cast<HttpInit>(*http_init_address)(pool,ssl_context,1024*1024):0;
    if(http_context<=0||!http_term_address||reinterpret_cast<HttpTerm>(*http_term_address)(http_context)!=0||
       resolve_network_control({"A9cVMUtEp4Y","libSceHttp","libSceHttp",1,0x0101,1}))
        throw std::runtime_error("Offline HTTP context lifecycle contract failed.");
    const auto cert=resolve_network_control({"DK+GoXCNT04","libSceHttp","libSceHttp",1,0x0101,2});
    using LoadCert=std::int32_t (LBP3_GUEST_ABI *)();
    if(!cert||reinterpret_cast<LoadCert>(*cert)()!=0||
       resolve_network_control({"DK+GoXCNT04","libSceHttp","libSceHttp",1,0x0101,1}))
        throw std::runtime_error("Offline HTTPS certificate setup contract failed.");
    const auto http_template=resolve_network_control({"0gYjPTR-6cY","libSceHttp","libSceHttp",1,0x0101,2});
    using HttpObject=std::int32_t (LBP3_GUEST_ABI *)();
    if(!http_template||reinterpret_cast<HttpObject>(*http_template)()<=0||
       resolve_network_control({"0gYjPTR-6cY","libSceHttp","libSceHttp",1,0x0101,1}))
        throw std::runtime_error("Offline HTTP template lifecycle contract failed.");
    std::int32_t value=-1;if(check_callback()!=0||state(&value)!=0||value!=0||state(reinterpret_cast<int*>(1))!=invalid_address)
        throw std::runtime_error("Offline network state contract failed.");
    if(info(0,nullptr)!=not_connected)throw std::runtime_error("Offline network info did not report disconnected.");
    std::array<std::uint32_t,4> nat_data{9,9,9,9};
    if(nat(nat_data.data())||nat_data[0]!=9||nat_data[1]!=0||nat_data[2]!=3||nat_data[3]!=0||nat(reinterpret_cast<std::uint32_t*>(1))!=invalid_address)
        throw std::runtime_error("Offline NAT information contract failed.");
    std::int32_t id{};auto* fake_callback=reinterpret_cast<void*>(0x10000);int user{};
    if(reg(fake_callback,&user,&id)||id<=0||reg(fake_callback,&user,nullptr)!=invalid_address||
       unreg(id)||unreg(id)!=invalid_handle||check_callback()!=0)
        throw std::runtime_error("Offline network callback lifecycle failed.");
    for(const auto nid:{"iQw3iQPhvUQ","UJ+Z7Q+4ck0","Rqm2OnZMCz0","uBPlr0lbuiI","obuxdTiwkF8","JO4yuTuMoKI"}){
        auto wrong=HostImport{nid,"libSceNetCtl","libSceNetCtl",1,0x0101,1};
        if(resolve_network_control(wrong))throw std::runtime_error("Network control function resolved as data.");
    }
}
}
