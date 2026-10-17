#include "lbp3_host/filesystem_exports.hpp"
#include "lbp3_host/guest_filesystem.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/module_registry.hpp"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <intrin.h>

namespace lbp3::hle {
namespace {
std::int64_t sce(std::int64_t result){return result<0?std::bit_cast<std::int32_t>(0x80020000u|static_cast<std::uint32_t>(-result)):result;}
std::int64_t posix(std::int64_t result){if(result<0){*guest_errno_address()=static_cast<std::int32_t>(-result);return -1;}return result;}
#define IO_EXPORT(name,params,args,type) \
    type LBP3_GUEST_ABI sce_##name params noexcept {return static_cast<type>(sce(io::name args));}
std::int32_t LBP3_GUEST_ABI sce_open(const char* path,int flags,int mode) noexcept {
    const auto result=static_cast<std::int32_t>(sce(io::open(path,flags,mode)));
    static const bool trace=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"LBP3_FS_CALLER_TRACE",value,2)>0;}();
    if(trace&&result<0){
        char guest_path[512]{};std::size_t length{};
        if(path){SIZE_T copied{};ReadProcessMemory(GetCurrentProcess(),path,guest_path,sizeof(guest_path)-1,&copied);length=static_cast<std::size_t>(copied);}
        if(length<sizeof(guest_path))guest_path[length]='\0';else guest_path[sizeof(guest_path)-1]='\0';
        const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        abi::ModuleInfoEx module{};
        if(abi::module_from_address(caller,2,&module)==0){
            std::uintptr_t base=caller;
            for(std::uint32_t i=0;i<module.segment_count;++i)base=std::min(base,static_cast<std::uintptr_t>(module.segments[i].address));
            std::fprintf(stderr,"[LBP3_FS_CALLER] result=0x%08x flags=0x%x mode=0%o path=\"%s\" module=%s guest_rva=0x%llx\n",
                static_cast<unsigned>(result),static_cast<unsigned>(flags),static_cast<unsigned>(mode),guest_path,module.name.data(),static_cast<unsigned long long>(caller-base));
        }else{
            std::fprintf(stderr,"[LBP3_FS_CALLER] result=0x%08x flags=0x%x mode=0%o path=\"%s\" caller_host=0x%llx module=unknown\n",
                static_cast<unsigned>(result),static_cast<unsigned>(flags),static_cast<unsigned>(mode),guest_path,static_cast<unsigned long long>(caller));
        }
    }
    return result;
}
IO_EXPORT(close,(int fd),(fd),std::int32_t)
IO_EXPORT(read,(int fd,void* p,std::uint64_t n),(fd,p,n),std::int64_t)
IO_EXPORT(write,(int fd,const void* p,std::uint64_t n),(fd,p,n),std::int64_t)
IO_EXPORT(pread,(int fd,void* p,std::uint64_t n,std::int64_t o),(fd,p,n,o),std::int64_t)
IO_EXPORT(pwrite,(int fd,const void* p,std::uint64_t n,std::int64_t o),(fd,p,n,o),std::int64_t)
IO_EXPORT(seek,(int fd,std::int64_t n,int w),(fd,n,w),std::int64_t)
IO_EXPORT(fstat,(int fd,io::Stat* s),(fd,s),std::int32_t)
IO_EXPORT(stat,(const char* p,io::Stat* s),(p,s),std::int32_t)
IO_EXPORT(fsync,(int fd),(fd),std::int32_t)
IO_EXPORT(ftruncate,(int fd,std::int64_t n),(fd,n),std::int32_t)
IO_EXPORT(truncate,(const char* p,std::int64_t n),(p,n),std::int32_t)
IO_EXPORT(mkdir,(const char* p,int m),(p,m),std::int32_t)
IO_EXPORT(rmdir,(const char* p),(p),std::int32_t)
IO_EXPORT(unlink,(const char* p),(p),std::int32_t)
IO_EXPORT(rename,(const char* a,const char* b),(a,b),std::int32_t)
#undef IO_EXPORT
std::int64_t LBP3_GUEST_ABI sce_getdirentries(int fd,void* buffer,std::uint64_t size,std::int64_t* base) noexcept {
    return sce(io::getdirentries(fd,buffer,size,base));
}
std::int32_t LBP3_GUEST_ABI posix_fstat(int fd,io::Stat* output) noexcept {return static_cast<std::int32_t>(posix(io::fstat(fd,output)));}
}
std::optional<std::uintptr_t> resolve_filesystem(const HostImport& request){
    if(request.type!=2)return std::nullopt;
    const auto nid=request.nid;
    if(nid=="1G3lF1Gg1k8")return reinterpret_cast<std::uintptr_t>(&sce_open);
    if(nid=="UK2Tl2DWUns")return reinterpret_cast<std::uintptr_t>(&sce_close);
    if(nid=="Cg4srZ6TKbU")return reinterpret_cast<std::uintptr_t>(&sce_read);
    if(nid=="4wSze92BhLI")return reinterpret_cast<std::uintptr_t>(&sce_write);
    if(nid=="+r3rMFwItV4")return reinterpret_cast<std::uintptr_t>(&sce_pread);
    if(nid=="nKWi-N2HBV4")return reinterpret_cast<std::uintptr_t>(&sce_pwrite);
    if(nid=="oib76F-12fk")return reinterpret_cast<std::uintptr_t>(&sce_seek);
    if(nid=="kBwCPsYX-m4")return reinterpret_cast<std::uintptr_t>(&sce_fstat);
    if(nid=="eV9wAD2riIA")return reinterpret_cast<std::uintptr_t>(&sce_stat);
    if(nid=="fTx66l5iWIA")return reinterpret_cast<std::uintptr_t>(&sce_fsync);
    if(nid=="VW3TVZiM4-E")return reinterpret_cast<std::uintptr_t>(&sce_ftruncate);
    if(nid=="WlyEA-sLDf0")return reinterpret_cast<std::uintptr_t>(&sce_truncate);
    if(nid=="1-LFLmRFxxM")return reinterpret_cast<std::uintptr_t>(&sce_mkdir);
    if(nid=="naInUjYt3so")return reinterpret_cast<std::uintptr_t>(&sce_rmdir);
    if(nid=="AUXVxWeJU-A")return reinterpret_cast<std::uintptr_t>(&sce_unlink);
    if(nid=="52NcYU9+lEo")return reinterpret_cast<std::uintptr_t>(&sce_rename);
    if(nid=="mqQMh1zPPT8")return reinterpret_cast<std::uintptr_t>(&posix_fstat);
    if(nid=="taRWhTJFTgE")return reinterpret_cast<std::uintptr_t>(&sce_getdirentries);
    return std::nullopt;
}
}
