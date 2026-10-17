#include "lbp3_host/libc_probe.hpp"
#include "lbp3_host/elf_mapping.hpp"
#include "lbp3_host/hle.hpp"
#include "lbp3_host/abi.hpp"
#include "lbp3_host/guest_filesystem.hpp"
#include "lbp3_host/guest_fs.hpp"
#include "lbp3_host/game_metadata.hpp"
#include "lbp3_host/iss_backend.hpp"
#include "lbp3_host/gpu_memory.hpp"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lbp3::diagnostics {
namespace {
void output(const char* message) noexcept {DWORD n{};WriteFile(GetStdHandle(STD_ERROR_HANDLE),message,static_cast<DWORD>(std::strlen(message)),&n,nullptr);}
[[noreturn]] void stop(unsigned code) noexcept {for(;;)TerminateProcess(GetCurrentProcess(),code);}
[[noreturn]] __declspec(noinline) void LBP3_GUEST_ABI missing_import(const char* message) noexcept {
    output("PROBE_UNIMPLEMENTED: ");output(message);output("\n");stop(0xe04c4301u);
}
void classify_data_fault(std::uintptr_t) noexcept;
LONG WINAPI exception_filter(EXCEPTION_POINTERS* info) noexcept {
    if(abi::emulate_guest_fs(info))return EXCEPTION_CONTINUE_EXECUTION;
    const auto code=info->ExceptionRecord->ExceptionCode;
    if(code!=EXCEPTION_ACCESS_VIOLATION&&code!=EXCEPTION_ILLEGAL_INSTRUCTION&&code!=EXCEPTION_INT_DIVIDE_BY_ZERO&&code!=EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    if(code==EXCEPTION_ACCESS_VIOLATION&&info->ExceptionRecord->NumberParameters>=2)
        classify_data_fault(info->ExceptionRecord->ExceptionInformation[1]);
    char text[256]{};std::snprintf(text,sizeof(text),"PROBE_MACHINE_FAULT: code=0x%08lx rip=0x%llx access=0x%llx address=0x%llx\n",code,info->ContextRecord->Rip,
        info->ExceptionRecord->NumberParameters>=2?static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]):0,
        info->ExceptionRecord->NumberParameters>=2?static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]):0);
    output(text);stop(0xe04c4302u);
}
struct Trap {
    std::string message;void* code{};bool data{};std::size_t size{4096};
    ~Trap(){if(code)VirtualFree(code,0,MEM_RELEASE);}
};
struct Probe {
    void* handler{};std::map<std::string,std::unique_ptr<Trap>> traps;
    Probe(){SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
        ULONG reserve=64*1024;(void)SetThreadStackGuarantee(&reserve);
        handler=AddVectoredExceptionHandler(1,&exception_filter);if(!handler)throw std::runtime_error("Cannot install probe fault handler.");}
    ~Probe(){if(handler)RemoveVectoredExceptionHandler(handler);}
    std::uintptr_t trap(const HostImport& request){
        auto key=std::string(request.type==1?"data ":"")+std::string(request.library)+"/"+std::string(request.module)+" nid="+std::string(request.nid)+
            " library_version="+std::to_string(request.library_version)+" module_version="+std::to_string(request.module_version);
        if(const auto found=traps.find(key);found!=traps.end())return reinterpret_cast<std::uintptr_t>(found->second->code);
        if(traps.size()>=1024)throw std::runtime_error("Probe trap budget exceeded.");
        auto value=std::make_unique<Trap>();value->message=key;
        if(request.type==1){value->data=true;value->size=1024*1024;
            value->code=VirtualAlloc(nullptr,value->size,MEM_RESERVE,PAGE_NOACCESS);
            if(!value->code)throw std::runtime_error("Cannot reserve inaccessible data proxy.");
            const auto address=reinterpret_cast<std::uintptr_t>(value->code);traps.emplace(key,std::move(value));return address;
        }
        value->code=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if(!value->code)throw std::runtime_error("Cannot allocate probe trap.");
        std::array<unsigned char,22> stub{0x48,0xbf};
        const auto message=reinterpret_cast<std::uintptr_t>(value->message.c_str());
        const auto handler_address=reinterpret_cast<std::uintptr_t>(&missing_import);
        std::memcpy(stub.data()+2,&message,8);stub[10]=0x48;stub[11]=0xb8;
        std::memcpy(stub.data()+12,&handler_address,8);stub[20]=0xff;stub[21]=0xe0;
        std::memcpy(value->code,stub.data(),stub.size());DWORD old{};
        if(!VirtualProtect(value->code,4096,PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),value->code,stub.size()))
            throw std::runtime_error("Cannot seal probe trap RX.");
        const auto address=reinterpret_cast<std::uintptr_t>(value->code);traps.emplace(key,std::move(value));return address;
    }
};
Probe* active{};
bool iss_gpu_active{};
void classify_data_fault(std::uintptr_t address) noexcept {
    if(active)for(const auto& [key,trap]:active->traps){(void)key;
        const auto begin=reinterpret_cast<std::uintptr_t>(trap->code);
        if(trap->data&&address>=begin&&address-begin<trap->size){
            output("PROBE_UNIMPLEMENTED_DATA: ");output(trap->message.c_str());output("\n");stop(0xe04c4303u);
        }
    }
}
std::optional<std::uintptr_t> probe_resolve(const HostImport& request){
    const bool video_import=request.library=="libSceVideoOut"||request.module=="libSceVideoOut";
    if(iss_gpu_active)
        if(auto address=gpu::resolve_iss_backend_import(request)){
            if(video_import){std::cout<<"PROBE_VIDEO_IMPORT: ISS "<<request.nid<<"\n"<<std::flush;}
            return address;
        }
    if(auto actual=hle::resolve(request)){
        if(video_import){std::cout<<"PROBE_VIDEO_IMPORT: HLE "<<request.nid<<"\n"<<std::flush;}
        return actual;
    }
    if(request.type==2||request.type==1)return active->trap(request);
    return std::nullopt; // Unknown data is inaccessible; no fake contents or TLS/function aliasing.
}
}
void libc_probe(const std::filesystem::path& directory,bool dependencies,bool constructors,bool entry,const std::filesystem::path& app,const std::filesystem::path& save,bool initialize_gpu){
    Probe probe;active=&probe;hle::initialize();
    std::unique_ptr<io::ScopedFileSystem> filesystem;
    if(entry){
        if(!std::filesystem::is_regular_file(app/"eboot.bin"))throw std::runtime_error("Probe app root must contain extracted eboot.bin.");
        const auto temp=save.parent_path()/L"temp";std::filesystem::create_directories(temp);
        filesystem=std::make_unique<io::ScopedFileSystem>(app,save,temp);
    }
    std::unique_ptr<memory::ScopedGpuMemoryTracking> gpu_faults;
    if(initialize_gpu){
        if(!entry)throw std::runtime_error("GPU probe requires the pinned entry mode.");
        const auto metadata=loader::read_game_metadata(app/L"sce_sys"/L"param.sfo",directory/L"eboot.elf");
        const auto user_directory=save.parent_path();
        const auto user_utf8=user_directory.u8string();
        const std::string user(user_utf8.begin(),user_utf8.end());
        const int status=gpu::initialize_iss_backend(metadata.title.c_str(),metadata.title_id.c_str(),user.c_str(),metadata.sdk_version,metadata.psf_attributes,1280,720);
        if(status!=0)throw std::system_error(status,std::system_category(),"initialize ISS GPU backend for diagnostic entry");
        iss_gpu_active=true;
        gpu_faults=std::make_unique<memory::ScopedGpuMemoryTracking>(&gpu::iss_backend_handle_fault);
        std::cout<<"PROBE_GPU_BACKEND: ISS GPU backend initialized; guest entry may submit work.\n"<<std::flush;
    }
    std::cout<<std::unitbuf;
    std::cout<<"PROBE_POLICY: link counts include stop traps and inaccessible data proxies, not completed implementations.\n";
    loader::probe_lbp3_libc(directory,std::cout,&probe_resolve,dependencies,constructors,entry);
    iss_gpu_active=false;active=nullptr;
}
void probe_fixture(std::wstring_view mode){
    Probe probe;active=&probe;
    if(mode==L"trap"){
        const auto address=probe.trap({"SyntheticNID","fixture","fixture",1,0x0101,2});
        using Entry=void (LBP3_GUEST_ABI *)(std::uint64_t,std::uint64_t) noexcept;
        Entry volatile entry=reinterpret_cast<Entry>(address);entry(123,456);
    }else if(mode==L"fault"){
        RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
    }else if(mode==L"data"){
        const auto address=probe.trap({"SyntheticData","fixture","fixture",1,0x0101,1});
        const volatile auto byte=*reinterpret_cast<volatile const unsigned char*>(address);(void)byte;
    }else throw std::runtime_error("Unknown probe fixture mode.");
    throw std::runtime_error("Probe fixture unexpectedly returned.");
}
}
