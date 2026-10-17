#include "lbp3_host/tls.hpp"
#include <malloc.h>
#include <algorithm>
#include <barrier>
#include <cstring>
#include <exception>
#include <ostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <Windows.h>

namespace lbp3::abi {
namespace { thread_local ThreadTls* current_tls{}; }

ScopedThreadTls::ScopedThreadTls(ThreadTls& storage) noexcept : previous_(current_tls) { current_tls=&storage; }
ScopedThreadTls::~ScopedThreadTls() { current_tls=previous_; }

__declspec(noinline) std::byte* LBP3_GUEST_ABI tls_get_addr(const TlsIndex* index) noexcept {
    if (!current_tls || !index) return nullptr;
    TlsIndex local{};
    SIZE_T copied{};
    if (!ReadProcessMemory(GetCurrentProcess(), index, &local, sizeof(local), &copied) || copied != sizeof(local))
        return nullptr;
    try { return current_tls->resolve(local.module_id, local.offset, 1).data(); }
    catch (...) { return nullptr; }
}
void ThreadTls::Free::operator()(std::byte* ptr) const noexcept { _aligned_free(ptr); }
std::byte* ThreadTls::static_thread_pointer(std::uint64_t id) noexcept {
    for(auto& block:blocks_)if(block.id==id)return block.bytes.get()+block.size;
    return nullptr;
}
std::byte* current_static_thread_pointer(std::uint64_t id) noexcept {
    return current_tls?current_tls->static_thread_pointer(id):nullptr;
}

ThreadTls::ThreadTls(std::span<const TlsTemplate> templates) {
    std::set<std::uint64_t> ids;
    std::uint64_t total{};
    for (const auto& item : templates) {
        if (!item.module_id || !ids.insert(item.module_id).second || !item.memory_size ||
            item.memory_size > 16ull*1024*1024 || item.initial.size() > item.memory_size ||
            !item.alignment || (item.alignment & (item.alignment-1)) || item.alignment > 1024*1024)
            throw std::runtime_error("Invalid TLS template or resource budget.");
        total += item.memory_size;
        if (total > 64ull*1024*1024) throw std::runtime_error("Thread TLS exceeds budget.");
        Block block;
        block.id = item.module_id;
        block.size = static_cast<std::size_t>(item.memory_size);
        block.bytes.reset(static_cast<std::byte*>(_aligned_malloc(block.size,
            static_cast<std::size_t>(std::max<std::uint64_t>(item.alignment, alignof(void*))))));
        if (!block.bytes) throw std::bad_alloc();
        std::memset(block.bytes.get(), 0, block.size);
        std::copy(item.initial.begin(), item.initial.end(), block.bytes.get());
        blocks_.push_back(std::move(block));
    }
}

std::span<std::byte> ThreadTls::resolve(std::uint64_t id, std::uint64_t offset, std::size_t size) {
    for (auto& block : blocks_) if (block.id == id) {
        if (offset > block.size || size > block.size-offset)
            throw std::runtime_error("TLS access is outside module block.");
        return {block.bytes.get()+static_cast<std::size_t>(offset), size};
    }
    throw std::runtime_error("Unknown guest TLS module ID.");
}

void tls_self_test(std::ostream& output) {
    using Getter = std::byte* (LBP3_GUEST_ABI *)(const TlsIndex*) noexcept;
    Getter volatile getter = &tls_get_addr;
    const TlsIndex first{1,0};
    if (getter(&first)) throw std::runtime_error("TLS getter accepted unattached thread.");
    const std::vector<TlsTemplate> templates{{1,256,128,{std::byte{0x42},std::byte{0x17}}}, {3,64,16,{}}};
    std::barrier sync(2);
    std::exception_ptr errors[2];
    std::uintptr_t addresses[2]{};
    bool okay[2]{};
    const auto worker = [&](int index) {
        bool arrived = false;
        try {
            ThreadTls tls(templates);
            ScopedThreadTls attached(tls);
            auto data = tls.resolve(1,0,256);
            addresses[index] = reinterpret_cast<std::uintptr_t>(data.data());
            const auto zeros = tls.resolve(3,0,64);
            const TlsIndex last{1,255};
            const TlsIndex bad_id{99,0}, bad_offset{1,256}, overflow{1,UINT64_MAX};
            bool valid = getter(&first) == data.data() && getter(&last) == data.data()+255 &&
                !getter(nullptr) && !getter(&bad_id) && !getter(&bad_offset) && !getter(&overflow) &&
                !getter(reinterpret_cast<const TlsIndex*>(1)) &&
                addresses[index] % 128 == 0 && data[0] == std::byte{0x42} &&
                data[1] == std::byte{0x17} && std::all_of(data.begin()+2,data.end(),[](auto b){return b==std::byte{};}) &&
                std::all_of(zeros.begin(),zeros.end(),[](auto b){return b==std::byte{};});
            data[0] = static_cast<std::byte>(index+1);
            arrived = true;
            sync.arrive_and_wait();
            valid = valid && addresses[0] != addresses[1] && *getter(&first) == static_cast<std::byte>(index+1);
            okay[index] = valid;
        } catch (...) {
            errors[index] = std::current_exception();
            if (!arrived) sync.arrive_and_drop();
        }
    };
    std::thread a(worker,0), b(worker,1);
    a.join(); b.join();
    for (auto e : errors) if (e) std::rethrow_exception(e);
    if (!okay[0] || !okay[1] || templates[0].initial[0] != std::byte{0x42})
        throw std::runtime_error("Thread TLS isolation test failed.");
    ThreadTls tls(templates);
    {
        ScopedThreadTls outer(tls);
        auto* original = getter(&first);
        {
            ThreadTls other(templates);
            ScopedThreadTls inner(other);
            if (getter(&first) == original || getter(&first) != other.resolve(1,0,1).data())
                throw std::runtime_error("Nested TLS attachment failed.");
        }
        if (getter(&first) != original) throw std::runtime_error("TLS context restoration failed.");
    }
    if (getter(&first)) throw std::runtime_error("TLS context leaked after detach.");
    for (int invalid = 0; invalid < 3; ++invalid) {
        bool rejected = false;
        try { (void)tls.resolve(invalid==0 ? 99 : 1, invalid==2 ? UINT64_MAX : 256, invalid==1 ? 1 : 8); }
        catch (const std::runtime_error&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Invalid TLS access accepted.");
    }
    auto duplicate = templates; duplicate[1].module_id=1;
    bool rejected = false;
    try { ThreadTls invalid(duplicate); }
    catch (const std::runtime_error&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Duplicate TLS module ID accepted.");
    output << "TLS self-test passed: two threads, isolated aligned blocks, initialized bytes and zero-filled tails.\n"
              "__tls_get_addr verified through SysV calls: offsets, isolation, nested attachment and detach.\n"
              "Invalid IDs, ranges, descriptors and duplicate IDs rejected. Windows FS/GS was not modified.\n";
}
}
