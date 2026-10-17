#include "lbp3_host/win32_file.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace lbp3::platform {
namespace {
[[noreturn]] void win32_error(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}
} // namespace

Win32File::Win32File(const std::filesystem::path& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        win32_error("CreateFileW");
    }
    // Close explicitly on constructor failure: the destructor is not called then.
    if (GetFileType(handle_) != FILE_TYPE_DISK) {
        CloseHandle(handle_);
        throw std::runtime_error("The input must be a disk file.");
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(handle_, &length)) {
        const DWORD error = GetLastError();
        CloseHandle(handle_);
        win32_error("GetFileSizeEx", error);
    }
    if (length.QuadPart < 0) {
        CloseHandle(handle_);
        throw std::runtime_error("Invalid negative file size.");
    }
    size_ = static_cast<std::uint64_t>(length.QuadPart);
}

Win32File::~Win32File() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
    }
}

void Win32File::read_at(std::uint64_t offset, std::span<std::byte> destination) {
    // Subtraction avoids overflow in offset + length.
    if (offset > size_ || destination.size() > size_ - offset) {
        throw std::runtime_error("Read range exceeds file size.");
    }
    if (destination.empty()) {
        return;
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN)) {
        win32_error("SetFilePointerEx");
    }
    while (!destination.empty()) {
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            destination.size(), std::numeric_limits<DWORD>::max()));
        DWORD received{};
        if (!ReadFile(handle_, destination.data(), requested, &received, nullptr)) {
            win32_error("ReadFile");
        }
        if (received == 0) {
            throw std::runtime_error("Unexpected EOF while reading the file.");
        }
        destination = destination.subspan(received);
    }
}

} // namespace lbp3::platform
