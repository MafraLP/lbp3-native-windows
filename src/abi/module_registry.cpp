#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <thread>

namespace lbp3::abi {
namespace {
std::mutex registry_mutex;
const std::vector<ModuleInfoEx>* modules{};
const std::vector<SignalReturnRange>* signal_returns{};
constexpr std::int32_t error(unsigned e){return std::bit_cast<std::int32_t>(0x80020000u|e);}
bool contains(const ModuleInfoEx& module,std::uintptr_t address){
    for(unsigned i=0;i<module.segment_count;++i){const auto& s=module.segments[i];
        if(address>=s.address && address-s.address<s.size)return true;
    }return false;
}
bool read(const ModuleInfoEx* source,ModuleInfoEx& value){SIZE_T n{};
    return source&&ReadProcessMemory(GetCurrentProcess(),source,&value,sizeof(value),&n)&&n==sizeof(value);}
bool write(ModuleInfoEx* target,const ModuleInfoEx& value){
    auto at=reinterpret_cast<std::uintptr_t>(target);if(!at||at>UINTPTR_MAX-sizeof(value))return false;
    const auto end=at+sizeof(value);
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;
        at=base+info.RegionSize;
    }SIZE_T n{};return WriteProcessMemory(GetCurrentProcess(),target,&value,sizeof(value),&n)&&n==sizeof(value);
}
}
ScopedModuleRegistry::ScopedModuleRegistry(std::span<const ModuleInfoEx> records,std::span<const SignalReturnRange> returns)
    :modules_(records.begin(),records.end()),returns_(returns.begin(),returns.end()){
    if(modules_.size()>64)throw std::runtime_error("Module registry budget exceeded.");
    std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;
    std::vector<std::int32_t> ids;
    for(const auto& record:modules_){
        if(record.size!=sizeof(record)||!record.segment_count||record.segment_count>4||
           std::find(record.name.begin(),record.name.end(),'\0')==record.name.end()||
           std::find(ids.begin(),ids.end(),record.id)!=ids.end())throw std::runtime_error("Invalid module record.");
        ids.push_back(record.id);
        for(unsigned i=0;i<record.segment_count;++i){const auto& segment=record.segments[i];
            if(!segment.address||!segment.size||segment.size>UINT64_MAX-segment.address)throw std::runtime_error("Invalid module segment.");
            ranges.emplace_back(segment.address,segment.address+segment.size);
        }
    }
    std::sort(ranges.begin(),ranges.end());
    for(std::size_t i=1;i<ranges.size();++i)if(ranges[i].first<ranges[i-1].second)throw std::runtime_error("Overlapping module records.");
    for(const auto& range:returns_){
        if(!range.size||range.size>UINTPTR_MAX-range.address||!std::any_of(ranges.begin(),ranges.end(),[&](const auto& r){return range.address>=r.first&&range.address+range.size<=r.second;}))
            throw std::runtime_error("Signal return range outside registered modules.");
    }
    std::lock_guard lock(registry_mutex);if(modules)throw std::runtime_error("Module registry already active.");
    modules=&modules_;signal_returns=&returns_;
}
ScopedModuleRegistry::~ScopedModuleRegistry(){std::lock_guard lock(registry_mutex);modules=nullptr;signal_returns=nullptr;}
std::int32_t LBP3_GUEST_ABI module_from_address(std::uintptr_t address,int flags,ModuleInfoEx* output) noexcept {
    try{std::lock_guard lock(registry_mutex);
        if(flags<0||flags>2)return error(22);if(!output)return error(14);
        if(!modules)return error(3);
        for(const auto& record:*modules)if(contains(record,address))return write(output,record)?0:error(14);
        return error(3);
    }catch(...){return error(5);}
}
std::int32_t LBP3_GUEST_ABI module_matches_address(const ModuleInfoEx* input,std::uintptr_t address) noexcept {
    try{std::lock_guard lock(registry_mutex);ModuleInfoEx record{};
        if(!modules||!read(input,record)||record.size!=sizeof(record))return 0;
        for(const auto& known:*modules)if(known.id==record.id){
            if(record.segment_count!=known.segment_count)return 0;
            for(unsigned i=0;i<known.segment_count;++i)if(record.segments[i].address!=known.segments[i].address||record.segments[i].size!=known.segments[i].size)return 0;
            return contains(known,address)?1:0;
        }
        return 0;
    }catch(...){return 0;}
}
std::int32_t LBP3_GUEST_ABI is_signal_return(std::uintptr_t address) noexcept {
    try{std::lock_guard lock(registry_mutex);if(signal_returns)for(const auto& range:*signal_returns)
        if(address>=range.address&&address-range.address<range.size)return 1;
        return 0;
    }catch(...){return 0;}
}
std::optional<memory::QueryInfo> query_module_memory(std::uintptr_t address,bool next){
    std::lock_guard lock(registry_mutex);std::optional<memory::QueryInfo> result;
    if(modules)for(const auto& module:*modules)for(unsigned i=0;i<module.segment_count;++i){const auto& segment=module.segments[i];
        const bool inside=address>=segment.address&&address-segment.address<segment.size;
        if(!inside&&(!next||segment.address<address))continue;
        if(result&&result->start<segment.address)continue;
        const auto flags=segment.protection;
        const int protection=((flags&4)?1:0)|((flags&2)?2:0)|((flags&1)?4:0);
        result=memory::QueryInfo{segment.address,segment.address+segment.size,0,protection,0,0x10,{}};
        std::copy_n(module.name.begin(),std::min<std::size_t>(31,std::find(module.name.begin(),module.name.end(),'\0')-module.name.begin()),result->name.begin());
    }
    return result;
}
void module_registry_self_test(std::ostream& output){
    ModuleInfoEx record{};record.name[0]='x';record.id=7;record.tls_index=2;record.init=0x10000;
    record.segments[0]={0x10000,0x4000,5};record.segments[1]={0x20000,0x8000,6};record.segment_count=2;
    record.eh_header=0x11000;record.eh_frame=0x12000;record.eh_frame_size=0x100;
    const std::array<ModuleInfoEx,1> records{record};const std::array<SignalReturnRange,1> returns{{{0x10100,8}}};
    ModuleInfoEx actual{};
    if(module_from_address(0x10000,2,&actual)!=error(3))throw std::runtime_error("Module registry leaked context.");
    {
        ScopedModuleRegistry scope(records,returns);
        if(module_from_address(0x10000,2,&actual)||actual.id!=7||actual.init!=0x10000||actual.eh_frame_size!=0x100||
           !module_matches_address(&actual,0x27fff)||module_matches_address(&actual,0x14000)||
           module_from_address(0x14000,2,&actual)!=error(3)||!is_signal_return(0x10100)||!is_signal_return(0x10107)||is_signal_return(0x10108))
            throw std::runtime_error("Module range/query/return classification failed.");
        bool child=false;std::thread worker([&]{ModuleInfoEx value{};child=!module_from_address(0x20000,0,&value)&&value.id==7;});worker.join();
        if(!child||module_from_address(0x10000,3,&actual)!=error(22)||module_from_address(0x10000,2,nullptr)!=error(14)||
           module_from_address(0x10000,2,reinterpret_cast<ModuleInfoEx*>(8))!=error(14)||module_matches_address(reinterpret_cast<ModuleInfoEx*>(8),0x10000))
            throw std::runtime_error("Module pointer/flag/thread validation failed.");
        actual.segments[0].address=0;
        if(module_matches_address(&actual,0x10000))throw std::runtime_error("Forged module ranges accepted.");
    }
    if(is_signal_return(0x10100)||module_from_address(0x10000,0,&actual)!=error(3))throw std::runtime_error("Module registry survived teardown.");
    auto bad=records;bad[0].segments[1].address=0x11000;
    bool rejected=false;try{ScopedModuleRegistry invalid(bad);}catch(const std::runtime_error&){rejected=true;}
    if(!rejected)throw std::runtime_error("Overlapping module registry accepted.");
    output << "Module registry self-test passed: 424-byte ABI, owned metadata, exact load ranges, worker queries and signal-return ranges.\n";
}
}
