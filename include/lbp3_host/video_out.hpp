#pragma once
#include "lbp3_host/abi.hpp"
#include "lbp3_host/host_resolver.hpp"
#include <memory>
#include <array>
namespace lbp3::gpu {
struct VideoState;
struct BufferAttribute {std::uint32_t pixel_format{};std::int32_t tiling{},aspect{};std::uint32_t width{},height{},pitch{},option{},reserved0{};std::uint64_t reserved1{};};
struct Resolution {std::int32_t full_width{},full_height{},pane_width{},pane_height{};std::uint64_t refresh_rate{3};float screen_inches{};std::uint16_t flags{},reserved0{};std::array<std::uint32_t,3> reserved1{};};
struct FlipStatus {std::uint64_t count{},process_time{},tsc{};std::int64_t argument{-1};std::uint64_t submit_tsc{},reserved0{};std::int32_t queue{},pending{},current{-1};std::uint32_t reserved1{};};
static_assert(sizeof(BufferAttribute)==40&&sizeof(Resolution)==48&&sizeof(FlipStatus)==64);
class ScopedVideoOut {
public:
    explicit ScopedVideoOut(std::uint32_t width=1280,std::uint32_t height=720);
    ~ScopedVideoOut();
    ScopedVideoOut(const ScopedVideoOut&)=delete;
    ScopedVideoOut& operator=(const ScopedVideoOut&)=delete;
private:std::unique_ptr<VideoState> state_;
};
std::int32_t LBP3_GUEST_ABI video_open(std::int32_t,int,int,const void*) noexcept;
std::int32_t LBP3_GUEST_ABI video_close(std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI video_resolution(std::int32_t,Resolution*) noexcept;
std::int32_t LBP3_GUEST_ABI video_flip_status(std::int32_t,FlipStatus*) noexcept;
std::int32_t LBP3_GUEST_ABI video_flip_rate(std::int32_t,int) noexcept;
std::int32_t LBP3_GUEST_ABI video_register_buffers(std::int32_t,int,void* const*,int,const BufferAttribute*) noexcept;
std::int32_t LBP3_GUEST_ABI video_unregister_buffers(std::int32_t,int) noexcept;
std::int32_t LBP3_GUEST_ABI video_buffer_labels(std::int32_t,std::uintptr_t*) noexcept;
std::int32_t LBP3_GUEST_ABI video_add_flip_event(std::int64_t,std::int32_t,void*) noexcept;
std::int32_t LBP3_GUEST_ABI video_delete_flip_event(std::int64_t,std::int32_t) noexcept;
std::int32_t LBP3_GUEST_ABI video_window_margins(std::int32_t,int,int) noexcept;
struct RegisteredVideoBuffer {std::uintptr_t address{};BufferAttribute attribute;int group{-1};};
std::optional<RegisteredVideoBuffer> video_buffer(std::int32_t,int);
void LBP3_GUEST_ABI video_attribute(BufferAttribute*,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t) noexcept;
std::optional<std::uintptr_t> resolve_video_out(const HostImport&);
}
