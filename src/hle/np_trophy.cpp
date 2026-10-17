#include "lbp3_host/np_trophy.hpp"
#include "lbp3_host/abi.hpp"
#include <Windows.h>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace lbp3::hle {
namespace {
constexpr std::int32_t invalid=std::bit_cast<std::int32_t>(0x80551604u);
std::atomic<std::int32_t> next_id{1};
std::int32_t new_id() noexcept {
    auto value=next_id.fetch_add(1,std::memory_order_relaxed);
    if(value<=0){next_id.store(2,std::memory_order_relaxed);value=1;}
    return value;
}
bool copy_out(void* destination,const void* source,std::size_t size) noexcept {
    if(!destination)return false;
    SIZE_T written{};
    return WriteProcessMemory(GetCurrentProcess(),destination,source,size,&written)&&written==size;
}
std::int32_t LBP3_GUEST_ABI create_context(std::int32_t* output,std::int32_t,std::uint32_t,std::uint64_t) noexcept {
    if(!output)return invalid;
    const auto id=new_id();return copy_out(output,&id,sizeof(id))?0:invalid;
}
std::int32_t LBP3_GUEST_ABI create_handle(std::int32_t* output) noexcept {
    if(!output)return invalid;
    const auto id=new_id();return copy_out(output,&id,sizeof(id))?0:invalid;
}
std::int32_t LBP3_GUEST_ABI register_context(std::int32_t context,std::int32_t handle,std::uint64_t) noexcept {
    return context>0&&handle>0?0:invalid;
}
std::int32_t LBP3_GUEST_ABI unlock_trophy(std::int32_t context,std::int32_t handle,std::int32_t id,std::int32_t* platinum) noexcept {
    if(context<=0||handle<=0||id<0)return invalid;
    if(platinum){const std::int32_t none=-1;if(!copy_out(platinum,&none,sizeof(none)))return invalid;}
    return 0;
}
bool clear_sized(void* object) noexcept {
    if(!object)return true;
    std::uint64_t size{};SIZE_T read{};
    if(!ReadProcessMemory(GetCurrentProcess(),object,&size,sizeof(size),&read)||read!=sizeof(size)||size<sizeof(size)||size>4096)return false;
    std::array<std::byte,256> zero{};
    auto cursor=reinterpret_cast<std::byte*>(object)+sizeof(size);
    auto remaining=static_cast<std::size_t>(size-sizeof(size));
    while(remaining){const auto count=remaining<zero.size()?remaining:zero.size();if(!copy_out(cursor,zero.data(),count))return false;cursor+=count;remaining-=count;}
    return true;
}
std::int32_t LBP3_GUEST_ABI get_game_info(std::int32_t context,std::int32_t handle,void* details,void* data) noexcept {
    if(context<=0||handle<=0)return invalid;
    return clear_sized(details)&&clear_sized(data)?0:invalid;
}
std::int32_t LBP3_GUEST_ABI get_unlock_state(std::int32_t context,std::int32_t handle,std::uint32_t* flags,std::uint32_t* count) noexcept {
    if(context<=0||handle<=0||!flags||!count)return invalid;
    constexpr std::array<std::uint32_t,4> empty{};constexpr std::uint32_t unlocked{};
    return copy_out(flags,empty.data(),sizeof(empty))&&copy_out(count,&unlocked,sizeof(unlocked))?0:invalid;
}
std::int32_t LBP3_GUEST_ABI release(std::int32_t id) noexcept {return id>0?0:invalid;}
}

std::optional<std::uintptr_t> resolve_np_trophy(const HostImport& r){
    if(r.type!=2||r.library!="libSceNpTrophy"||r.module!="libSceNpTrophy"||r.library_version!=1||r.module_version!=0x0101)return std::nullopt;
    if(r.nid=="XbkjbobZlCY")return reinterpret_cast<std::uintptr_t>(&create_context);
    if(r.nid=="q7U6tEAQf7c")return reinterpret_cast<std::uintptr_t>(&create_handle);
    if(r.nid=="TJCAxto9SEU")return reinterpret_cast<std::uintptr_t>(&register_context);
    if(r.nid=="28xmRUFao68")return reinterpret_cast<std::uintptr_t>(&unlock_trophy);
    if(r.nid=="YYP3f2W09og"||r.nid=="qqUVGDgQBm0")return reinterpret_cast<std::uintptr_t>(&get_game_info);
    if(r.nid=="LHuSmO3SLd8")return reinterpret_cast<std::uintptr_t>(&get_unlock_state);
    if(r.nid=="aTnHs7W-9Uk"||r.nid=="GNcF4oidY0Y"||r.nid=="E1Wrwd07Lr8")return reinterpret_cast<std::uintptr_t>(&release);
    return std::nullopt;
}

void np_trophy_self_test(){
    auto get=[](const char* nid){return resolve_np_trophy({nid,"libSceNpTrophy","libSceNpTrophy",1,0x0101,2});};
    using Create=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*);
    using Context=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t*,std::int32_t,std::uint32_t,std::uint64_t);
    using Unlocks=std::int32_t (LBP3_GUEST_ABI *)(std::int32_t,std::int32_t,std::uint32_t*,std::uint32_t*);
    std::int32_t context{},handle{};auto ctx=get("XbkjbobZlCY"),h=get("q7U6tEAQf7c");
    if(!ctx||!h||reinterpret_cast<Context>(*ctx)(&context,1,0,0)||reinterpret_cast<Create>(*h)(&handle)||context<=0||handle<=0)
        throw std::runtime_error("Offline trophy context/handle contract failed.");
    std::array<std::uint32_t,4> flags{1,1,1,1};std::uint32_t count=99;auto state=get("LHuSmO3SLd8");
    if(!state||reinterpret_cast<Unlocks>(*state)(context,handle,flags.data(),&count)||count||flags!=std::array<std::uint32_t,4>{})
        throw std::runtime_error("Offline trophy unlock state was not empty.");
    if(reinterpret_cast<Create>(*h)(nullptr)!=invalid||resolve_np_trophy({"q7U6tEAQf7c","libSceNpTrophy","libSceNpTrophy",1,0x0101,1})||
       resolve_np_trophy({"q7U6tEAQf7c","libSceNpTrophy","libSceNpTrophy",1,0x0100,2}))
        throw std::runtime_error("Trophy bad-pointer or strict-import check failed.");
}
}
