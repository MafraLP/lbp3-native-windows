#include "lbp3_host/process_param.hpp"
#include <atomic>
#include <cstdint>
#include <stdexcept>

namespace lbp3::abi {
namespace { std::atomic<const void*> process_param{}; }
ScopedProcessParam::ScopedProcessParam(std::span<const std::byte> bytes) {
    const auto read=[&](std::size_t offset,unsigned size) {
        if(offset>bytes.size() || size>bytes.size()-offset) throw std::runtime_error("Truncated process parameters.");
        std::uint64_t value{};
        for(unsigned i=0;i<size;++i) value|=static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[offset+i]))<<(i*8);
        return value;
    };
    const auto size=read(0,8);
    if(size<0x40 || size>bytes.size() || size>1024*1024 || read(8,4)!=0x4942524f || read(12,4)!=1)
        throw std::runtime_error("Unsupported process parameter size, magic or version.");
    previous_=process_param.exchange(bytes.data());
}
ScopedProcessParam::~ScopedProcessParam() { process_param.store(previous_); }
const void* LBP3_GUEST_ABI get_proc_param() noexcept { return process_param.load(); }
}
