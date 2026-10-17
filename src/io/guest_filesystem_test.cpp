#include "lbp3_host/guest_filesystem.hpp"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace lbp3::io {
void self_test(const std::filesystem::path& root,std::ostream& output){
    const auto run=root/(L"run-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto app=run/"app",save=run/"save",temp=run/"temp";
    std::filesystem::create_directories(app);std::filesystem::create_directories(save);std::filesystem::create_directories(temp);
    {std::ofstream fixture(app/"fixture.bin",std::ios::binary);fixture.write("abcdef",6);if(!fixture)throw std::runtime_error("Filesystem fixture write failed.");}
    if(open("/app0/fixture.bin",0,0)!=-22||translate_path("/app0/fixture.bin",nullptr,0)!=-14)
        throw std::runtime_error("Filesystem escaped context.");
    int orphan{};
    {
        ScopedFileSystem scope(app,save,temp);
        std::array<char,512> translated{};
        const auto expected=std::filesystem::canonical(app/L"fixture.bin").u8string();
        std::string expected_utf8(expected.size(),'\0');
        std::transform(expected.begin(),expected.end(),expected_utf8.begin(),[](char8_t value){return static_cast<char>(value);});
        if(translate_path("/app0/fixture.bin",translated.data(),translated.size())||
           std::string_view(translated.data())!=expected_utf8||
           translate_path("/savedata0/../escape",translated.data(),translated.size())!=-13||
           translate_path("/temp0/hdd0/cache.bin",translated.data(),translated.size())||
           translate_path("/app0/fixture.bin",reinterpret_cast<char*>(8),translated.size())!=-14||
           expected.empty())
            throw std::runtime_error("Guest path translation did not preserve mounts or reject invalid paths.");
        const auto fd=static_cast<int>(open("/app0/fixture.bin",0,0));std::array<char,8> bytes{};
        if(fd<3||read(fd,bytes.data(),2)!=2||bytes[0]!='a'||bytes[1]!='b'||pread(fd,bytes.data(),2,4)!=2||bytes[0]!='e'||
           seek(fd,0,1)!=2||read(fd,bytes.data(),8)!=4||read(fd,bytes.data(),8)!=0||write(fd,"bad",3)!=-9)
            throw std::runtime_error("Read-only file offsets/EOF failed.");
        std::array<bool,2> good{};
        auto worker=[&](unsigned i){good[i]=true;for(unsigned n=0;n<64;++n){std::array<char,2> local{};
            if(pread(fd,local.data(),2,i?4:0)!=2||local[0]!=(i?'e':'a'))good[i]=false;
        }};
        std::thread a(worker,0),b(worker,1);a.join();b.join();
        if(!good[0]||!good[1]||seek(fd,0,1)!=6||close(fd)||close(fd)!=-9)
            throw std::runtime_error("Concurrent positioned reads changed cursor or stale descriptor accepted.");
        if(mkdir("/savedata0/folder",0755)||mkdir("/savedata0/folder",0755)!=-17)
            throw std::runtime_error("Guest mkdir semantics failed.");
        if(mkdir("/temp0/cache",0755))throw std::runtime_error("Writable volatile /temp0 directory creation failed.");
        const auto temp_fd=static_cast<int>(open("/temp0/cache/state.bin",2|0x200|0x400|0x800,0666));
        if(temp_fd<3||write(temp_fd,"volatile",8)!=8||close(temp_fd))
            throw std::runtime_error("Writable volatile /temp0 file creation failed.");
        const auto temp_read_fd=static_cast<int>(open("/temp0/cache/state.bin",0,0));
        if(temp_read_fd<3||close(temp_read_fd))
            throw std::runtime_error("Writable volatile /temp0 mapping failed.");
        const auto directory_fd=static_cast<int>(open("/app0",0x20004,0));
        const auto regular_fd=static_cast<int>(open("/app0/fixture.bin",0,0));
        std::array<std::byte,512> directory_buffer{};std::int64_t directory_base=-1;
        if(directory_fd<3||regular_fd<3||getdirentries(directory_fd,directory_buffer.data(),directory_buffer.size(),&directory_base)<0||
           directory_base!=0||getdirentries(directory_fd,directory_buffer.data(),128,&directory_base)!=-22||
           getdirentries(regular_fd,directory_buffer.data(),directory_buffer.size(),&directory_base)!=-20||close(directory_fd)||close(regular_fd))
            throw std::runtime_error("Directory enumeration format, cursor, or argument validation failed.");
        bool found_fixture=false;
        for(std::size_t offset=0;offset<directory_buffer.size();){
            std::uint16_t record_size{};if(offset+8>directory_buffer.size())break;
            std::memcpy(&record_size,directory_buffer.data()+offset+4,sizeof(record_size));
            if(record_size<8||offset+record_size>directory_buffer.size())break;
            const auto name=reinterpret_cast<const char*>(directory_buffer.data()+offset+8);
            if(std::string_view(name)=="fixture.bin")found_fixture=true;
            offset+=record_size;
        }
        if(!found_fixture)throw std::runtime_error("Directory enumeration omitted the app fixture.");
        const auto writable_fd=static_cast<int>(open("/savedata0/folder/test.bin",2|0x200|0x400|0x800,0666));
        if(writable_fd<3||open("/savedata0/folder/test.bin",2|0x200|0x800,0666)!=-17||write(writable_fd,"abcdef",6)!=6||
           seek(writable_fd,2,0)!=2||pwrite(writable_fd,"Z",1,0)!=1||seek(writable_fd,0,1)!=2||write(writable_fd,"XY",2)!=2)
            throw std::runtime_error("Guest create/pwrite/write semantics failed.");
        Stat info{};
        if(fstat(writable_fd,&info)||info.size!=6||(info.mode&0170000)!=0100000||info.block_size!=0x4000||
           ftruncate(writable_fd,4)||seek(writable_fd,0,2)!=4||truncate("/savedata0/folder/test.bin",2)||fsync(writable_fd)||
           stat("/savedata0/folder/test.bin",&info)||info.size!=2||close(writable_fd))throw std::runtime_error("Guest stat/truncate/sync failed.");
        const auto append=static_cast<int>(open("/savedata0/folder/test.bin",1|8,0));
        if(append<3||seek(append,0,0)||write(append,"!",1)!=1||close(append)||stat("/savedata0/folder/test.bin",&info)||info.size!=3||
           rename("/savedata0/folder/test.bin","/savedata0/folder/renamed.bin")||unlink("/savedata0/folder/renamed.bin")||rmdir("/savedata0/folder"))
            throw std::runtime_error("Guest append/rename/unlink/rmdir failed.");
        if(open("/app0/fixture.bin",1,0)!=-30||unlink("/app0/fixture.bin")!=-30||mkdir("/app0/new",0755)!=-30||
           open("/savedata0/../escape",0,0)!=-13||open("/app0//escape",0,0)!=-13||open("/savedata0/NUL",0,0)!=-13||
           open("/savedata0/file:stream",0,0)!=-13||open("/app0/fixture.bin",0x100000,0)!=-45||
           open("C:\\Windows\\win.ini",0,0)!=-2||open(nullptr,0,0)!=-14)
            throw std::runtime_error("Guest mount/traversal/device restrictions failed.");
        orphan=static_cast<int>(open("/app0/fixture.bin",0,0));
        auto* readonly=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READONLY);
        if(!readonly)throw std::runtime_error("Filesystem readonly fixture failed.");
        const auto denied=read(orphan,readonly,1);VirtualFree(readonly,0,MEM_RELEASE);
        if(denied!=-14||seek(orphan,0,1)!=0||read(orphan,reinterpret_cast<void*>(8),1)!=-14||pread(orphan,bytes.data(),1,-1)!=-22)
            throw std::runtime_error("Guest file invalid buffer changed cursor.");
        if(close(orphan))throw std::runtime_error("Filesystem pointer fixture cleanup failed.");
        std::array<int,128> all{};
        for(auto& value:all){value=static_cast<int>(open("/app0/fixture.bin",0,0));if(value<3)throw std::runtime_error("File capacity fixture failed.");}
        if(open("/app0/fixture.bin",0,0)!=-24)throw std::runtime_error("File descriptor capacity ignored.");
        for(auto value:all)if(close(value))throw std::runtime_error("File capacity cleanup failed.");
        orphan=static_cast<int>(open("/app0/fixture.bin",0,0));
        HANDLE input{},pipe{};const auto original=GetStdHandle(STD_OUTPUT_HANDLE);
        if(!CreatePipe(&input,&pipe,nullptr,0))throw std::runtime_error("Console pipe fixture failed.");
        SetStdHandle(STD_OUTPUT_HANDLE,pipe);
        const auto written=write(1,"test",4),invalid=write(1,reinterpret_cast<void*>(8),1);
        SetStdHandle(STD_OUTPUT_HANDLE,original);CloseHandle(pipe);
        DWORD received{};const auto read_ok=ReadFile(input,bytes.data(),8,&received,nullptr);CloseHandle(input);
        if(written!=4||invalid!=-14||!read_ok||received!=4||std::string_view(bytes.data(),4)!="test")throw std::runtime_error("Console write snapshot/descriptor contract failed.");
    }
    if(close(orphan)!=-9)throw std::runtime_error("File descriptors survived context.");
    {ScopedFileSystem clean(app,save,temp);const auto fd=static_cast<int>(open("/app0/fixture.bin",0,0));
        if(fd<3||close(fd))throw std::runtime_error("Fresh filesystem context failed.");}
    output << "Filesystem self-test passed: read-only /app0, writable /savedata0, UTF-8 paths, owned descriptors and 120-byte stat.\n"
              "Read/write/pread/pwrite/seek/truncate/sync, append, rename/unlink/directories, concurrent offsets, console and invalid paths verified.\n";
}
}
