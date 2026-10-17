#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <iosfwd>
#include "lbp3_host/abi.hpp"

namespace lbp3::abi {
struct TlsTemplate {
    std::uint64_t module_id{}, memory_size{}, alignment{1};
    std::vector<std::byte> initial;
};
// Explicit per-thread guest blocks. Does not change Windows FS/GS or TEB.
class ThreadTls {
public:
    explicit ThreadTls(std::span<const TlsTemplate> templates);
    std::span<std::byte> resolve(std::uint64_t module_id, std::uint64_t offset, std::size_t size);
    std::byte* static_thread_pointer(std::uint64_t module_id) noexcept;
private:
    struct Free { void operator()(std::byte* ptr) const noexcept; };
    struct Block {
        std::uint64_t id{};
        std::size_t size{};
        std::unique_ptr<std::byte, Free> bytes;
    };
    std::vector<Block> blocks_;
};
struct TlsIndex { std::uint64_t module_id{}, offset{}; };
static_assert(sizeof(TlsIndex) == 16);
// Stack-scoped attachment, supports nesting and restores the previous context.
// The storage must outlive the scope; create/destroy scopes on the same thread.
class ScopedThreadTls {
public:
    explicit ScopedThreadTls(ThreadTls& storage) noexcept;
    ~ScopedThreadTls();
    ScopedThreadTls(const ScopedThreadTls&) = delete;
    ScopedThreadTls& operator=(const ScopedThreadTls&) = delete;
private:
    ThreadTls* previous_{};
};
std::byte* LBP3_GUEST_ABI tls_get_addr(const TlsIndex* index) noexcept;
std::byte* current_static_thread_pointer(std::uint64_t module_id) noexcept;
void tls_self_test(std::ostream& output);
}
