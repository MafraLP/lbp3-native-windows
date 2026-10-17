#include "lbp3_host/guest_filesystem.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace lbp3::io {
namespace {
std::mutex registry;
struct File {HANDLE handle{INVALID_HANDLE_VALUE};bool readable{},writable{},append{};std::filesystem::path path;bool directory{};std::vector<std::byte> directory_data;std::vector<std::size_t> directory_offsets;std::size_t directory_cursor{};};
struct Mounts {std::filesystem::path app,save,temp;std::map<int,File> files;int next{3};};
Mounts* active{};
constexpr std::size_t transfer_limit=1024*1024;
void trace_fs(const char* operation,const std::filesystem::path& path,std::int64_t result,
              std::uint64_t size=0,std::int64_t offset=0){
    static const bool enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"LBP3_FS_TRACE",value,2)>0;}();
    static std::atomic_uint calls{};
    if(!enabled)return;const auto index=calls.fetch_add(1,std::memory_order_relaxed);if(index>=16384)return;
    const auto encoded=path.u8string();const std::string utf8(encoded.begin(),encoded.end());
    std::fprintf(stderr,"[LBP3_FS_TRACE] #%u op=%s path=\"%s\" size=%llu offset=%lld result=%lld\n",
        index,operation,utf8.c_str(),static_cast<unsigned long long>(size),static_cast<long long>(offset),static_cast<long long>(result));
}
int host_error(){switch(GetLastError()){
    case ERROR_FILE_NOT_FOUND:case ERROR_PATH_NOT_FOUND:return 2;
    case ERROR_ACCESS_DENIED:case ERROR_SHARING_VIOLATION:return 13;
    case ERROR_ALREADY_EXISTS:case ERROR_FILE_EXISTS:return 17;
    case ERROR_DISK_FULL:case ERROR_HANDLE_DISK_FULL:return 28;
    case ERROR_DIR_NOT_EMPTY:return 66;
    case ERROR_INVALID_HANDLE:return 9;
    case ERROR_NOT_ENOUGH_MEMORY:case ERROR_OUTOFMEMORY:return 12;
    case ERROR_INVALID_PARAMETER:case ERROR_NEGATIVE_SEEK:return 22;
    default:return 5;
}}
bool copy_in(const void* source,void* target,std::size_t count){if(!count)return true;SIZE_T n{};
    return source&&ReadProcessMemory(GetCurrentProcess(),source,target,count,&n)&&n==count;}
bool writable(void* pointer,std::size_t count){
    if(!count)return true;auto at=reinterpret_cast<std::uintptr_t>(pointer);
    if(!at||count>UINTPTR_MAX-at)return false;const auto end=at+count;
    while(at<end){MEMORY_BASIC_INFORMATION info{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info))||info.State!=MEM_COMMIT||info.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
        const auto p=info.Protect&255;
        if(p!=PAGE_READWRITE&&p!=PAGE_WRITECOPY&&p!=PAGE_EXECUTE_READWRITE&&p!=PAGE_EXECUTE_WRITECOPY)return false;
        const auto base=reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if(info.RegionSize>UINTPTR_MAX-base||base+info.RegionSize<=at)return false;at=base+info.RegionSize;
    }return true;
}
bool copy_out(void* target,const void* source,std::size_t count){if(!count)return true;SIZE_T n{};
    return writable(target,count)&&WriteProcessMemory(GetCurrentProcess(),target,source,count,&n)&&n==count;}
struct Path {std::filesystem::path native;bool writable{};};
int translate(const char* guest,Path& out){
    if(!active)return 22;
    std::string name;bool terminated=false;
    for(unsigned i=0;i<4096;++i){char value{};if(!copy_in(guest?guest+i:nullptr,&value,1))return 14;
        if(!value){terminated=true;break;}name.push_back(value);}
    if(!terminated)return 63;
    std::string relative;
    if(name=="/app0"||name.starts_with("/app0/")){out.native=active->app;relative=name.substr(std::min<std::size_t>(name.size(),6));}
    else if(name=="/savedata0"||name.starts_with("/savedata0/")){out.native=active->save;out.writable=true;relative=name.substr(std::min<std::size_t>(name.size(),11));}
    else if(name=="/temp0"||name.starts_with("/temp0/")){out.native=active->temp;out.writable=true;relative=name.substr(std::min<std::size_t>(name.size(),7));}
    else return 2;
    if(!relative.empty()&&relative.front()=='/')return 13;
    if(relative.find_first_of("\\:")!=std::string::npos)return 13;
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,relative.data(),static_cast<int>(relative.size()),nullptr,0);
    if(!relative.empty()&&!count)return 22;
    std::wstring wide(static_cast<std::size_t>(count),L'\0');
    if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,relative.data(),static_cast<int>(relative.size()),wide.data(),count);
    for(const auto& component:std::filesystem::path(wide)){
        const auto part=component.native();if(part.empty()||part==L".")continue;
        if(part==L".."||part.find_first_of(L"<>\"|?*")!=std::wstring::npos||part.back()==L'.'||part.back()==L' ')return 13;
        auto device=part.substr(0,part.find(L'.'));for(auto& c:device)if(c>=L'a'&&c<=L'z')c-=L'a'-L'A';
        if(device==L"CON"||device==L"PRN"||device==L"AUX"||device==L"NUL"||
           (device.size()==4&&(device.starts_with(L"COM")||device.starts_with(L"LPT"))&&device[3]>=L'1'&&device[3]<=L'9'))return 13;
        out.native/=component;
        const auto attributes=GetFileAttributesW(out.native.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return 13;
    }return 0;
}
File* file(int fd){if(!active)return nullptr;const auto it=active->files.find(fd);return it==active->files.end()?nullptr:&it->second;}
std::int64_t do_open(const char* path,int flags,int mode){
    (void)mode;Path name;if(const auto e=translate(path,name))return -e;
    constexpr int supported=3|4|8|0x200|0x400|0x800|0x20000;
    if(flags&~supported || (flags&3)==3){trace_fs("open_unsupported_flags",name.native,-45,static_cast<std::uint32_t>(flags));return -45;}
    const bool reading=(flags&3)!=1,writing=(flags&3)!=0;
    if((writing||flags&(0x200|0x400))&&!name.writable)return -30;
    if((flags&(0x200|0x400))&&!writing)return -22;
    if(active->files.size()>=128||active->next==INT_MAX)return -24;
    DWORD disposition=OPEN_EXISTING;
    if(flags&0x200)disposition=flags&0x800?CREATE_NEW:flags&0x400?CREATE_ALWAYS:OPEN_ALWAYS;
    else if(flags&0x400)disposition=TRUNCATE_EXISTING;
    const DWORD access=(reading?GENERIC_READ:0)|(writing?GENERIC_WRITE:0);
    const auto handle=CreateFileW(name.native.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,disposition,
                                  flags&0x20000?FILE_FLAG_BACKUP_SEMANTICS:FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE){const auto error=-host_error();trace_fs("open",name.native,error);return error;}
    if(flags&0x20000){BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){CloseHandle(handle);return -20;}}
    const auto fd=active->next++;
    try{auto [it,inserted]=active->files.emplace(fd,File{handle,reading,writing,(flags&8)!=0,name.native,(flags&0x20000)!=0,{},{},0});(void)inserted;(void)it;}catch(...){CloseHandle(handle);throw;}
    trace_fs("open",name.native,fd);return fd;
}
std::int64_t transfer(int fd,void* buffer,std::uint64_t size,bool writing,std::int64_t offset,bool positioned){
    if(size>INT64_MAX)return -22;
    if(positioned&&offset<0)return -22;
    HANDLE handle{};File* entry=nullptr;
    if(writing&&!positioned&&(fd==1||fd==2))handle=GetStdHandle(fd==1?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
    else {entry=file(fd);if(!entry|| (writing?!entry->writable:!entry->readable))return -9;handle=entry->handle;}
    if(!size)return 0;
    const auto count=static_cast<DWORD>(std::min<std::uint64_t>(size,transfer_limit));
    std::vector<std::byte> bytes(count);
    if(writing?!copy_in(buffer,bytes.data(),count):!writable(buffer,count))return -14;
    LARGE_INTEGER saved{},zero{};bool restore=false;
    if(positioned){if(!SetFilePointerEx(handle,zero,&saved,FILE_CURRENT))return -host_error();
        LARGE_INTEGER at{};at.QuadPart=offset;if(!SetFilePointerEx(handle,at,nullptr,FILE_BEGIN))return -host_error();restore=true;}
    else if(writing&&entry&&entry->append){if(!SetFilePointerEx(handle,zero,nullptr,FILE_END))return -host_error();}
    DWORD done{};const auto ok=writing?WriteFile(handle,bytes.data(),count,&done,nullptr):ReadFile(handle,bytes.data(),count,&done,nullptr);
    const auto error=ok?0:host_error();
    if(restore&&!SetFilePointerEx(handle,saved,nullptr,FILE_BEGIN))return -5;
    if(error)return -error;
    if(!writing&&!copy_out(buffer,bytes.data(),done))return -14;
    return done;
}
TimeSpec timestamp(FILETIME value){ULARGE_INTEGER n{};n.LowPart=value.dwLowDateTime;n.HighPart=value.dwHighDateTime;
    const auto ticks=static_cast<std::int64_t>(n.QuadPart)-116444736000000000ll;
    auto seconds=ticks/10000000,nanos=(ticks%10000000)*100;if(nanos<0){--seconds;nanos+=1000000000;}
    return {seconds,nanos};}
std::int64_t inspect(HANDLE handle,Stat* output){
    if(!writable(output,sizeof(*output)))return -14;
    BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(handle,&info))return -host_error();
    FILE_STANDARD_INFO standard{};if(!GetFileInformationByHandleEx(handle,FileStandardInfo,&standard,sizeof(standard)))return -host_error();
    Stat result{};result.device=info.dwVolumeSerialNumber;result.inode=info.nFileIndexLow;
    result.mode=static_cast<std::uint16_t>(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY?0040755:0100644);
    result.links=static_cast<std::uint16_t>(std::min<DWORD>(info.nNumberOfLinks,UINT16_MAX));
    result.atime=timestamp(info.ftLastAccessTime);result.mtime=timestamp(info.ftLastWriteTime);result.ctime=result.mtime;
    result.birthtime=timestamp(info.ftCreationTime);result.size=standard.EndOfFile.QuadPart;
    result.blocks=(standard.AllocationSize.QuadPart+511)/512;result.block_size=0x4000;
    return copy_out(output,&result,sizeof(result))?0:-14;
}
std::int64_t set_length(File& entry,std::int64_t length){
    if(length<0)return -22;if(!entry.writable)return -9;
    FILE_END_OF_FILE_INFO info{};info.EndOfFile.QuadPart=length;
    return SetFileInformationByHandle(entry.handle,FileEndOfFileInfo,&info,sizeof(info))?0:-host_error();
}
template<class Fn> std::int64_t guarded(Fn&& fn) noexcept {try{std::lock_guard lock(registry);return fn();}
    catch(const std::bad_alloc&){return -12;}catch(...){return -5;}}
}
struct ScopedFileSystem::State:Mounts{};
ScopedFileSystem::ScopedFileSystem(const std::filesystem::path& app,const std::filesystem::path& save,const std::filesystem::path& temp):state_(std::make_unique<State>()){
    state_->app=std::filesystem::canonical(app);state_->save=std::filesystem::canonical(save);state_->temp=std::filesystem::canonical(temp);
    if(!std::filesystem::is_directory(state_->app)||!std::filesystem::is_directory(state_->save)||!std::filesystem::is_directory(state_->temp))throw std::runtime_error("Filesystem mounts must be existing directories.");
    std::lock_guard lock(registry);if(active)throw std::runtime_error("A filesystem is already active.");active=state_.get();
}
ScopedFileSystem::~ScopedFileSystem(){std::lock_guard lock(registry);for(const auto& [fd,entry]:state_->files){(void)fd;CloseHandle(entry.handle);}active=nullptr;}
std::int64_t open(const char* path,int flags,int mode) noexcept {return guarded([&]{return do_open(path,flags,mode);});}
std::int64_t close(int fd) noexcept {return guarded([&]()->std::int64_t {auto* entry=file(fd);if(!entry)return -9;
    if(!CloseHandle(entry->handle))return -host_error();active->files.erase(fd);return 0;});}
std::int64_t read(int fd,void* buffer,std::uint64_t size) noexcept {return guarded([&]{auto* entry=file(fd);if(!entry)return std::int64_t{-9};const auto result=transfer(fd,buffer,size,false,0,false);trace_fs("read",entry->path,result,size);return result;});}
std::int64_t write(int fd,const void* buffer,std::uint64_t size) noexcept {return guarded([&]{auto* entry=file(fd);const auto result=transfer(fd,const_cast<void*>(buffer),size,true,0,false);
    if(entry)trace_fs("write",entry->path,result,size);return result;});}
std::int64_t pread(int fd,void* buffer,std::uint64_t size,std::int64_t offset) noexcept {return guarded([&]{auto* entry=file(fd);if(!entry)return std::int64_t{-9};const auto result=transfer(fd,buffer,size,false,offset,true);trace_fs("pread",entry->path,result,size,offset);return result;});}
std::int64_t pwrite(int fd,const void* buffer,std::uint64_t size,std::int64_t offset) noexcept {return guarded([&]{auto* entry=file(fd);const auto result=transfer(fd,const_cast<void*>(buffer),size,true,offset,true);
    if(entry)trace_fs("pwrite",entry->path,result,size,offset);return result;});}
std::int64_t seek(int fd,std::int64_t offset,int whence) noexcept {return guarded([&]()->std::int64_t {auto* entry=file(fd);if(!entry)return -9;if(whence<0||whence>2)return -22;
    LARGE_INTEGER by{},result{};by.QuadPart=offset;return SetFilePointerEx(entry->handle,by,&result,static_cast<DWORD>(whence))?result.QuadPart:-host_error();});}
std::int64_t fstat(int fd,Stat* output) noexcept {return guarded([&]()->std::int64_t {const auto* entry=file(fd);return entry?inspect(entry->handle,output):-9;});}
std::int64_t stat(const char* path,Stat* output) noexcept {return guarded([&]()->std::int64_t {Path name;if(const auto e=translate(path,name))return -e;
    const auto handle=CreateFileW(name.native.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return -host_error();const auto result=inspect(handle,output);CloseHandle(handle);return result;});}
std::int64_t fsync(int fd) noexcept {return guarded([&]()->std::int64_t {auto* entry=file(fd);if(!entry||!entry->writable)return -9;return FlushFileBuffers(entry->handle)?0:-host_error();});}
std::int64_t ftruncate(int fd,std::int64_t length) noexcept {return guarded([&]()->std::int64_t {auto* entry=file(fd);if(!entry)return -9;
    const auto result=set_length(*entry,length);trace_fs("ftruncate",entry->path,result,static_cast<std::uint64_t>(length));return result;});}
std::int64_t truncate(const char* path,std::int64_t length) noexcept {return guarded([&]()->std::int64_t {if(length<0)return -22;Path name;if(const auto e=translate(path,name))return -e;
    if(!name.writable)return -30;const auto fd=do_open(path,1,0);if(fd<0)return fd;
    const auto result=set_length(*file(static_cast<int>(fd)),length);trace_fs("truncate",name.native,result,static_cast<std::uint64_t>(length));CloseHandle(file(static_cast<int>(fd))->handle);active->files.erase(static_cast<int>(fd));return result;});}
std::int64_t mkdir(const char* path,int mode) noexcept {return guarded([&]()->std::int64_t {(void)mode;Path name;if(const auto e=translate(path,name))return -e;
    if(!name.writable)return -30;const auto result=CreateDirectoryW(name.native.c_str(),nullptr)?0:-host_error();trace_fs("mkdir",name.native,result);return result;});}
std::int64_t rmdir(const char* path) noexcept {return guarded([&]()->std::int64_t {Path name;if(const auto e=translate(path,name))return -e;
    if(!name.writable||name.native==active->save||name.native==active->temp)return -30;return RemoveDirectoryW(name.native.c_str())?0:-host_error();});}
std::int64_t unlink(const char* path) noexcept {return guarded([&]()->std::int64_t {Path name;if(const auto e=translate(path,name))return -e;
    if(!name.writable)return -30;return DeleteFileW(name.native.c_str())?0:-host_error();});}
std::int64_t rename(const char* source,const char* destination) noexcept {return guarded([&]()->std::int64_t {Path from,to;
    if(const auto e=translate(source,from))return -e;if(const auto e=translate(destination,to))return -e;
    if(!from.writable||!to.writable||from.native==active->save||to.native==active->save||from.native==active->temp||to.native==active->temp)return -30;
    return MoveFileExW(from.native.c_str(),to.native.c_str(),MOVEFILE_REPLACE_EXISTING)?0:-host_error();});}
std::int64_t getdirentries(int fd,void* buffer,std::uint64_t capacity,std::int64_t* base) noexcept {
    return guarded([&]()->std::int64_t {
        auto* entry=file(fd);if(!entry)return -9;if(!entry->directory)return -20;
        if(!buffer)return -14;if(capacity<512||capacity>transfer_limit)return -22;if(!writable(buffer,static_cast<std::size_t>(capacity)))return -14;
        if(base&&!writable(base,sizeof(*base)))return -14;
        if(entry->directory_data.empty()){
            struct DirentHead {std::uint32_t inode;std::uint16_t record_length;std::uint8_t type,name_length;};
            static_assert(sizeof(DirentHead)==8);
            auto append=[&](std::string_view name,std::uint8_t type){
                if(name.size()>255) return;
                const auto raw=sizeof(DirentHead)+name.size()+1;const auto length=(raw+7)&~std::size_t{7};
                const auto start=entry->directory_data.size();entry->directory_data.resize(start+length);
                entry->directory_offsets.push_back(start);
                auto* record=entry->directory_data.data()+start;DirentHead head{static_cast<std::uint32_t>(entry->directory_offsets.size()),static_cast<std::uint16_t>(length),type,static_cast<std::uint8_t>(name.size())};
                std::memcpy(record,&head,sizeof(head));std::memcpy(record+sizeof(head),name.data(),name.size());
            };
            append(".",4);append("..",4);
            WIN32_FIND_DATAW data{};const auto pattern=entry->path/L"*";HANDLE search=FindFirstFileW(pattern.c_str(),&data);
            if(search!=INVALID_HANDLE_VALUE){
                do{
                    if(std::wcscmp(data.cFileName,L".")==0||std::wcscmp(data.cFileName,L"..")==0)continue;
                    const auto encoded=std::filesystem::path(data.cFileName).u8string();
                    const std::string name(encoded.begin(),encoded.end());
                    append(name,(data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)?4:8);
                }while(FindNextFileW(search,&data));
                FindClose(search);
            }else if(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_NO_MORE_FILES)return -host_error();
        }
        const auto initial_position=entry->directory_cursor;std::size_t amount{};
        while(entry->directory_cursor<entry->directory_offsets.size()){
            const auto start=entry->directory_offsets[entry->directory_cursor];
            const auto* record=entry->directory_data.data()+start;
            std::uint16_t size{};std::memcpy(&size,record+4,sizeof(size));
            if(!size||size>entry->directory_data.size()-start)return -5;
            if(size>capacity-amount){if(!amount)return -22;break;}
            amount+=size;++entry->directory_cursor;
        }
        if(amount){const auto start=entry->directory_offsets[initial_position];if(!copy_out(buffer,entry->directory_data.data()+start,amount))return -14;}
        if(base){const auto position=static_cast<std::int64_t>(initial_position);if(!copy_out(base,&position,sizeof(position)))return -14;}
        return static_cast<std::int64_t>(amount);
    });
}
std::int64_t translate_path(const char* guest_path,char* host_utf8,std::uint64_t capacity) noexcept {
    return guarded([&]()->std::int64_t {
        if(!host_utf8||!capacity||capacity>SIZE_MAX)return -14;
        Path translated;if(const auto error=translate(guest_path,translated))return -error;
        const auto encoded=translated.native.u8string();
        if(encoded.size()>=capacity)return -63;
        std::vector<char> bytes(encoded.size()+1);
        std::transform(encoded.begin(),encoded.end(),bytes.begin(),[](char8_t value){return static_cast<char>(value);});
        return copy_out(host_utf8,bytes.data(),bytes.size())?0:-14;
    });
}
}
