#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <iosfwd>
namespace lbp3::io {
struct TimeSpec {std::int64_t seconds{},nanoseconds{};};
struct Stat {
    std::uint32_t device{},inode{};std::uint16_t mode{},links{};std::uint32_t uid{},gid{},rdev{};
    TimeSpec atime{},mtime{},ctime{};std::int64_t size{},blocks{};
    std::uint32_t block_size{},flags{},generation{};std::int32_t spare{};TimeSpec birthtime{};
};
static_assert(sizeof(Stat)==120);
class ScopedFileSystem {
public:
    ScopedFileSystem(const std::filesystem::path& app,const std::filesystem::path& save,
                     const std::filesystem::path& temp);
    ~ScopedFileSystem();
    ScopedFileSystem(const ScopedFileSystem&)=delete;
    ScopedFileSystem& operator=(const ScopedFileSystem&)=delete;
private:
    struct State;std::unique_ptr<State> state_;
};
// Host core returns negative guest POSIX errno; HLE wrappers translate SCE/POSIX.
std::int64_t open(const char*,int,int) noexcept;
std::int64_t close(int) noexcept;
std::int64_t read(int,void*,std::uint64_t) noexcept;
std::int64_t write(int,const void*,std::uint64_t) noexcept;
std::int64_t pread(int,void*,std::uint64_t,std::int64_t) noexcept;
std::int64_t pwrite(int,const void*,std::uint64_t,std::int64_t) noexcept;
std::int64_t seek(int,std::int64_t,int) noexcept;
std::int64_t fstat(int,Stat*) noexcept;
std::int64_t stat(const char*,Stat*) noexcept;
std::int64_t fsync(int) noexcept;
std::int64_t ftruncate(int,std::int64_t) noexcept;
std::int64_t truncate(const char*,std::int64_t) noexcept;
std::int64_t mkdir(const char*,int) noexcept;
std::int64_t rmdir(const char*) noexcept;
std::int64_t unlink(const char*) noexcept;
std::int64_t rename(const char*,const char*) noexcept;
std::int64_t getdirentries(int,void*,std::uint64_t,std::int64_t*) noexcept;
std::int64_t translate_path(const char* guest_path,char* host_utf8,std::uint64_t capacity) noexcept;
void self_test(const std::filesystem::path&,std::ostream&);
}
