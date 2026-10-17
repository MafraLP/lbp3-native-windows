#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace lbp3::platform {

// A synchronous file handle; read_at changes its cursor. Not thread-safe.
class Win32File final {
public:
    explicit Win32File(const std::filesystem::path& path);
    ~Win32File();
    Win32File(const Win32File&) = delete;
    Win32File& operator=(const Win32File&) = delete;
    Win32File(Win32File&&) = delete;
    Win32File& operator=(Win32File&&) = delete;

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    void read_at(std::uint64_t offset, std::span<std::byte> destination);

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::uint64_t size_{};
};

} // namespace lbp3::platform
