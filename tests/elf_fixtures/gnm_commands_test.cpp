#include "lbp3_host/gnm_commands.hpp"
#include "lbp3_host/video_out.hpp"
#include "../../third_party/shadps4/gnm_initial_state.hpp"
#include <Windows.h>
#include <array>
#include <iostream>
#include <stdexcept>
using namespace lbp3::gpu;
void check(bool valid){if(!valid)throw std::runtime_error("GNM command encoding assertion failed");}
int main(){try{
    std::array<std::uint32_t,258> output{};output.fill(0xdeadbeef);
    check(dispatch_initial_state(output.data()+1,255)==0&&output[1]==0xdeadbeef);
    check(dispatch_initial_state(output.data()+1,256)==256&&output.front()==0xdeadbeef&&output.back()==0xdeadbeef);
    std::size_t at=1;unsigned packets{};const unsigned opcodes[]{0x76,0x76,0x76,0x58,0x10};
    while(at<257){const auto header=output[at];check((header>>30)==3&&packets<5&&((header>>8)&255)==opcodes[packets]);
        const auto length=((header>>16)&0x3fff)+2;check(length<=257-at);
        if(packets<3)check((header&2)&&length==3&&output[at+1]==(packets==0?0x216u:packets==1?0x217u:0x215u)&&output[at+2]==(packets==2?0x170u:0xffffffffu));
        if(packets==3)check(length==7&&output[at+1]==0x28000000&&output[at+6]==0xa);
        at+=length;++packets;
    }check(at==257&&packets==5);
    check(dispatch_initial_state(nullptr,256)==0&&dispatch_initial_state(reinterpret_cast<std::uint32_t*>(1),256)==0);
    auto* readonly=static_cast<std::uint32_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY));check(readonly);
    check(dispatch_initial_state(readonly,256)==0);VirtualFree(readonly,0,MEM_RELEASE);
    check(resolve_gnm_commands({"nF6bFRUBRAU","libSceGnmDriver","libSceGnmDriver",1,0x0101,2}).has_value());
    output.fill(0xdeadbeef);
    check(draw_initial_state(output.data()+1,255)==0&&output[1]==0xdeadbeef);
    check(draw_initial_state(output.data()+1,256)==256&&output.front()==0xdeadbeef&&output.back()==0xdeadbeef);
    at=1;unsigned resets{},registers{},barriers{},nops{};
    while(at<257){const auto header=output[at];check((header>>30)==3);const auto length=((header>>16)&0x3fff)+2;check(length<=257-at);
        const auto opcode=(header>>8)&255;
        if(opcode==0x12){++resets;check(length==2&&output[at+1]==0);}
        if(opcode==0x69||opcode==0x76||opcode==0x79)++registers;
        if(opcode==0x58)++barriers;
        if(opcode==0x10){++nops;check(at+length==257);}
        at+=length;
    }check(at==257&&resets==1&&registers>30&&barriers==1&&nops==1);
    for(std::size_t i=0;i<reference::ClearStateSequence.size();++i)
        check(output[1+i]==reference::ClearStateSequence[i]);
    for(std::size_t i=2;i<reference::InitSequence.size();++i)
        check(output[1+reference::ClearStateSequence.size()+i-2]==reference::InitSequence[i]);
    constexpr std::size_t nop_index=1+reference::ClearStateSequence.size()+reference::InitSequence.size()-2;
    check(output[nop_index]==0xc07f1000u);
    for(std::size_t i=nop_index+1;i<257;++i)check(output[i]==0);
    check(draw_initial_state(nullptr,256)==0);
    lbp3::gpu::ScopedVideoOut video(64,32);const auto handle=video_open(-1,0,0,nullptr);check(handle>0);
    std::array<std::uint32_t,9> wait{};wait.fill(0xdeadbeef);
    check(insert_wait_flip(wait.data()+1,7,handle,15)==0&&wait.front()==0xdeadbeef&&wait.back()==0xdeadbeef);
    std::uintptr_t labels{};check(video_buffer_labels(handle,&labels)==16);
    const auto address=static_cast<std::uint64_t>(wait[3])|(static_cast<std::uint64_t>(wait[4])<<32);
    check(wait[1]==0xc0053c00&&wait[2]==0x13&&address==labels+15*8&&wait[5]==0&&wait[6]==0xffffffff&&wait[7]==10);
    check(insert_wait_flip(wait.data(),7,handle,16)==-1&&insert_wait_flip(wait.data(),6,handle,0)==-1&&insert_wait_flip(wait.data(),7,-1,0)==-1);
    const std::array<std::uint32_t,7> shader{0x12345678,0,0x111,0x222,8,4,1};
    std::array<std::uint32_t,27> encoded{};encoded.fill(0xdeadbeef);
    check(set_compute_shader(encoded.data()+1,25,shader.data())==0&&encoded.front()==0xdeadbeef&&encoded.back()==0xdeadbeef);
    check(encoded[1]==0xc0027600&&encoded[2]==0x20c&&encoded[3]==shader[0]&&encoded[4]==0&&encoded[6]==0x212&&encoded[10]==0x207&&encoded[11]==8&&encoded[12]==4&&encoded[13]==1);
    check(set_compute_shader(encoded.data(),24,shader.data())==-1&&set_compute_shader(encoded.data(),25,reinterpret_cast<const std::uint32_t*>(1))==-1);
    auto bad_shader=shader;bad_shader[1]=1;check(set_compute_shader(encoded.data(),25,bad_shader.data())==-1);
    check(pop_marker(encoded.data(),6)==0&&encoded[0]==0xc0041000&&encoded[1]==0x68750002&&pop_marker(encoded.data(),5)==-1);
    std::array<std::uint32_t,8> marker{};
    check(push_marker(marker.data(),6,"abc")==0&&marker[0]==0xc0031000&&marker[1]==0x68750001&&marker[2]==0x00636261&&
          marker[3]==0&&marker[4]==0&&marker[5]==0);
    check(push_marker(marker.data(),5,"abc")==-1&&push_marker(marker.data(),6,reinterpret_cast<const char*>(1))==-1&&
          push_marker(reinterpret_cast<std::uint32_t*>(1),6,"abc")==-1);
    check(resolve_gnm_commands({"W1Etj-jlW7Y","libSceGnmDriver","libSceGnmDriver",1,0x0101,2}).has_value());
    const auto submit=resolve_gnm_commands({"xbxNatawohc","libSceGnmDriver","libSceGnmDriver",1,0x0101,2});
    check(submit.has_value());
    const auto submit_done=resolve_gnm_commands({"yvZ73uQUqrk","libSceGnmDriver","libSceGnmDriver",1,0x0101,2});
    check(submit_done.has_value());
    using SubmitDone=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    check(reinterpret_cast<SubmitDone>(*submit_done)()==static_cast<std::int32_t>(0x80d11000u));
    using GnmQuery=std::int32_t (LBP3_GUEST_ABI *)() noexcept;
    const auto capture=resolve_gnm_commands({"TLV4mswiZ4A","libSceGnmDriver","libSceGnmDriver",1,0x0101,2});
    const auto pa=resolve_gnm_commands({"jg33rEKLfVs","libSceGnmDriver","libSceGnmDriver",1,0x0101,2});
    const auto debug_status=resolve_gnm_commands({"qpGITzPE+Zc","libSceGnmDriver","libSceGnmDriver",1,0x0101,2});
    check(capture&&pa&&debug_status&&reinterpret_cast<GnmQuery>(*capture)()==0&&
          reinterpret_cast<GnmQuery>(*pa)()==0&&reinterpret_cast<GnmQuery>(*debug_status)()==0);
    using Submit=std::int32_t (LBP3_GUEST_ABI *)(std::uint32_t,const std::uint32_t* const*,
        const std::uint32_t*,const std::uint32_t* const*,const std::uint32_t*,std::uint32_t,
        std::uint32_t,std::uint32_t,std::int64_t);
    const auto call=reinterpret_cast<Submit>(*submit);
    std::array<std::uint32_t,8> dcb{};const std::uint32_t* dcb_ptr=dcb.data();
    std::uint32_t dcb_size=sizeof(dcb);const std::uint32_t* ccb_ptr=dcb.data();
    std::uint32_t ccb_size=sizeof(dcb);
    check(call(1,&dcb_ptr,&dcb_size,nullptr,nullptr,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    check(call(1,nullptr,&dcb_size,nullptr,nullptr,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    check(call(1,&dcb_ptr,nullptr,nullptr,nullptr,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    check(call(1,&dcb_ptr,&dcb_size,&ccb_ptr,&ccb_size,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    dcb_size=0;check(call(1,&dcb_ptr,&dcb_size,nullptr,nullptr,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    check(call(0,nullptr,nullptr,nullptr,nullptr,1,0,0,0)==static_cast<std::int32_t>(0x80d11000u));
    std::cout<<"GNM packet tests passed: decoded initialization, wait-flip, compute-shader, push/pop marker and protected-output cases. Packet generation does not execute GPU work.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
