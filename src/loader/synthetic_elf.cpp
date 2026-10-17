#include "lbp3_host/synthetic_elf.hpp"
#include <cstdint>
#include <string>
#include <utility>
#include <initializer_list>

namespace lbp3::loader {
std::vector<std::byte> synthetic_elf(bool fault) {
    std::vector<std::byte> data(0x3000);
    const auto put=[&](std::size_t offset,std::uint64_t value,unsigned size) {
        for(unsigned i=0;i<size;++i) data.at(offset+i)=static_cast<std::byte>((value>>(8*i))&255);
    };
    const auto ph=[&](std::size_t index,std::uint32_t type,std::uint32_t flags,std::uint64_t offset,
                      std::uint64_t va,std::uint64_t filesz,std::uint64_t memsz,std::uint64_t align) {
        const auto at=64+index*56;
        put(at,type,4);put(at+4,flags,4);put(at+8,offset,8);put(at+16,va,8);
        put(at+32,filesz,8);put(at+40,memsz,8);put(at+48,align,8);
    };
    put(0,0x464c457f,4);put(4,2,1);put(5,1,1);put(6,1,1);put(7,9,1);
    put(16,0xfe10,2);put(18,62,2);put(20,1,4);put(24,0x1000,8);put(32,64,8);
    put(52,64,2);put(54,56,2);put(56,5,2);
    ph(0,1,5,0,0,0x2000,0x2000,0x1000);
    ph(1,1,6,0x2000,0x2000,0x1000,0x1000,0x1000);
    ph(2,2,6,0x2200,0x2200,11*16,11*16,8);
    ph(3,0x61000000,4,0x2400,0,0x400,0,8);
    ph(4,7,4,0x2100,0x2100,2,32,16);
    put(0x2100,0x42,1);
    std::string strings(1,'\0');
    const auto str=[&](const char* s) { auto offset=strings.size(); strings+=s;strings+='\0';return offset; };
    const auto lib=str("libkernel"),mod=str("libkernel");
    const auto tls=str("vNe1w4diLCs#B#C"),yield=str("6XG4B33N09g#B#C"),guard=str("f7uOxY9mM1U#B#C");
    for(std::size_t i=0;i<strings.size();++i) put(0x2400+i,static_cast<unsigned char>(strings[i]),1);
    std::size_t tag_index{};
    const auto tag=[&](std::uint64_t type,std::uint64_t value) {
        put(0x2200+tag_index*16,type,8);put(0x2208+tag_index*16,value,8);++tag_index;
    };
    tag(0x61000035,0);tag(0x61000037,strings.size());tag(0x61000039,0x100);
    tag(0x6100003f,96);tag(0x6100003b,24);
    tag(0x61000015,(1ull<<48)|(1ull<<32)|lib);
    tag(0x6100000f,(2ull<<48)|(0x101ull<<32)|mod);
    tag(0x6100002f,0x200);tag(0x61000031,120);tag(0x61000033,24);tag(0,0);
    put(0x2500+24,tls,4);put(0x2500+28,0x12,1);
    put(0x2500+48,yield,4);put(0x2500+52,0x12,1);
    put(0x2500+72,guard,4);put(0x2500+76,0x11,1);
    const auto rel=[&](std::size_t i,std::uint64_t target,std::uint32_t type,std::uint32_t symbol,std::uint64_t addend) {
        put(0x2600+i*24,target,8);put(0x2608+i*24,(static_cast<std::uint64_t>(symbol)<<32)|type,8);
        put(0x2610+i*24,addend,8);
    };
    rel(0,0x2000,16,0,0);rel(1,0x2010,7,1,0);rel(2,0x2018,7,2,0);rel(3,0x2020,8,0,0x2100);
    rel(4,0x2028,6,3,0);
    std::size_t pc=0x1000;
    const auto emit=[&](std::initializer_list<unsigned> bytes) { for(auto b:bytes) put(pc++,b,1); };
    const auto rip=[&](std::initializer_list<unsigned> bytes,std::size_t target) {
        emit(bytes);const auto after=pc+4;put(pc,static_cast<std::uint32_t>(target-after),4);pc+=4;
    };
    if(fault) { emit({0x31,0xc0,0x48,0x8b,0x00,0xc3}); return data; }
    emit({0x53}); // push rbx: preserve SysV nonvolatile, align stack for calls.
    rip({0x48,0x8b,0x05},0x2028); // Load imported object address, then its canary value.
    emit({0x48,0x85,0xc0,0x74});const auto guard_address_jump=pc++;
    emit({0x48,0x8b,0x00,0x48,0x85,0xc0,0x74});const auto guard_value_jump=pc++;
    rip({0x48,0x8d,0x3d},0x2000); // lea rdi,[rip+TlsIndex]
    rip({0xff,0x15},0x2010); // call imported __tls_get_addr
    emit({0x48,0x85,0xc0,0x74});const auto null_jump=pc++; // jz failure
    emit({0x0f,0xb6,0x18}); // movzx ebx,byte[rax]
    emit({0x80,0x78,0x01,0x00,0x75});const auto zero_jump=pc++;
    emit({0xc6,0x00,0x55}); // mutate this thread's initialized TLS
    rip({0xff,0x15},0x2018); // call imported sched_yield
    emit({0x85,0xc0,0x75});const auto yield_jump=pc++;
    emit({0x89,0xd8,0x83,0xc0,0x07,0x5b,0xc3}); // original TLS byte + 7 = 73
    const auto failure=pc;
    emit({0xb8,0xff,0xff,0xff,0xff,0x5b,0xc3});
    for(auto jump:{guard_address_jump,guard_value_jump,null_jump,zero_jump,yield_jump}) put(jump,failure-(jump+1),1);
    return data;
}
}
